//
//  log-request-handler.cc
//  Residue
//
//  Copyright 2017-present Muflihun Labs
//
//  Author: @abumusamq
//
//  Licensed under the Apache License, Version 2.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.
//

#include "logging/log.h"
#include "logging/log-request-handler.h"
#include "logging/log-request.h"
#include "logging/user-log-builder.h"
#include "logging/residue-log-dispatcher.h"
#include "core/configuration.h"
#include "tasks/client-integrity-task.h"
#include "logging/known-logger-configurator.h"

using namespace residue;

LogRequestHandler::LogRequestHandler(Registry* registry,
                                     el::LogBuilder* userLogBuilder) :
    RequestHandler("Log", registry),
    m_userLogBuilder(static_cast<UserLogBuilder*>(userLogBuilder))
{
    DRVLOG(RV_DEBUG) << "LogRequestHandler " << this << " with registry " << m_registry;
}

LogRequestHandler::~LogRequestHandler()
{
    m_stopped = true;
    m_backgroundWorker.join();
}

void LogRequestHandler::start()
{
    m_stopped = false;

    m_backgroundWorker = std::thread([&]() {
        el::Helpers::setThreadName("LogDispatcher");
        while (!m_stopped) {
            processRequestQueue();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
}

void LogRequestHandler::handle(RawRequest&& rawRequest)
{
    rawRequest.session->writeStandardResponse(Response::StatusCode::STATUS_OK);
    m_queue.push(std::move(rawRequest));
}

void LogRequestHandler::processRequestQueue()
{
    bool compressionEnabled = m_registry->configuration()->hasFlag(Configuration::Flag::COMPRESSION);
    bool allowBulkRequests = m_registry->configuration()->hasFlag(Configuration::ALLOW_BULK_LOG_REQUEST);
    auto maxItemsInBulk = m_registry->configuration()->maxItemsInBulk();

 #ifdef RESIDUE_PROFILING
    types::Time m_timeTaken;
    RESIDUE_PROFILE_START(t_process_queue);
    std::size_t totalRequests = 0; // 1 for 1 request so for bulk of 50 this will be 50
 #endif

    // we take snapshot to prevent potential race conditions (even though we have LoggingQueue that is safe)
    const std::size_t total = m_queue.size();

    const types::Time lastClientIntegrityRun = m_registry->clientIntegrityTask() == nullptr
            ? 0L : m_registry->clientIntegrityTask()->lastExecution();

    if (total > 0 && m_registry->clientIntegrityTask() != nullptr) {
        // we pause client integrity task until we clear this queue
        // so we don't clean a (now) dead client that passed initial validation
#ifdef RESIDUE_DEV
        DRVLOG(RV_DEBUG) << "Pausing schedule for client integrity task";
#endif
        m_registry->clientIntegrityTask()->pauseScheduledCleanup();
    }

    for (std::size_t i = 0; i < total; ++i) {

        if (m_registry->configuration()->dispatchDelay() > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(m_registry->configuration()->dispatchDelay()));
        }

#ifdef RESIDUE_DEBUG
        DRVLOG(RV_CRAZY) << "-----============= [ BEGIN ] =============-----";
#endif
        LogRequest request(m_registry->configuration());
        RawRequest rawRequest = m_queue.pull();

        std::shared_ptr<Session> session = rawRequest.session;
        RequestHandler::handle(std::move(rawRequest), &request, Request::StatusCode::BAD_REQUEST,
                               false, false, compressionEnabled);

        if ((!request.isValid() && !request.isBulk())
                || request.statusCode() != Request::StatusCode::CONTINUE) {
            RVLOG(RV_ERROR) << "Failed: " << request.errorText();
            continue;
        }
#ifdef RESIDUE_DEV
        DRVLOG(RV_DEBUG) << "Is bulk? " << request.isBulk();
#endif
        if (request.isBulk()) {
            if (allowBulkRequests) {
                // Create bulk request items
                unsigned int itemCount = 0U;
                Client* currentClient = request.client();
                bool forceClientValidation = true;
 #ifdef RESIDUE_DEV
                DRVLOG(RV_DEBUG) << "Request client: " << request.client();
 #endif
                JsonDoc d;
                for (const auto& js : request.jsonObject()) {
                    if (itemCount == maxItemsInBulk) {
                        RLOG(ERROR) << "Maximum number of bulk requests reached. Ignoring the rest of items in bulk";
                        break;
                    }
                    d.set(js);
                    std::string requestItemStr(d.dump());
                    LogRequest requestItem(m_registry->configuration());
                    requestItem.deserialize(std::move(requestItemStr));
                    if (requestItem.isValid()) {
                        requestItem.setIpAddr(request.ipAddr());
                        requestItem.setDateReceived(request.dateReceived());
                        requestItem.setClient(request.client());

                        if (processRequest(&requestItem, &currentClient, forceClientValidation, session.get())) {
                            forceClientValidation = false;
                        } else {
                            // force next client validation if last process was unsuccessful
                            forceClientValidation = true;
                        }
                        itemCount++;
 #ifdef RESIDUE_PROFILING
                        totalRequests++;
 #endif
                    } else {
                        RLOG(ERROR) << "Invalid request in bulk.";
                    }
                }
            } else {
                RLOG(ERROR) << "Bulk requests are not allowed";
            }
        } else {

            if (request.client() != nullptr) {
                request.setClientId(request.client()->id());
            }
            processRequest(&request, nullptr, true, session.get());
#ifdef RESIDUE_PROFILING
            totalRequests++;
#endif
        }

#ifdef RESIDUE_DEBUG
        DRVLOG(RV_CRAZY) << "-----============= [ ✓ ] =============-----";
#endif
    }

    if (m_registry->clientIntegrityTask() != nullptr &&
            lastClientIntegrityRun < m_registry->clientIntegrityTask()->lastExecution() &&
            m_queue.backlogEmpty()) {
        RVLOG(RV_DEBUG) << "Starting client integrity task after queue is processed.";
        // trigger client integrity task as it was run while this queue was being processed
        if (!m_registry->clientIntegrityTask()->isExecuting()) {
            m_registry->clientIntegrityTask()->performCleanup();
        }
    }

    if (total > 0 && m_registry->clientIntegrityTask() != nullptr && m_queue.backlogEmpty()) {
#ifdef RESIDUE_DEV
        DRVLOG(RV_DEBUG) << "Resuming schedule for client integrity task";
#endif
        m_registry->clientIntegrityTask()->resumeScheduledCleanup();
    }

 #ifdef RESIDUE_PROFILING
    RESIDUE_PROFILE_END(t_process_queue, m_timeTaken);
    float timeTakenInSec = static_cast<float>(m_timeTaken / 1000.0f);
    RLOG_IF(total > 0, DEBUG) << "Took " << timeTakenInSec << "s to process the queue of "
                                   << total << " items (" << totalRequests << " requests). Average: "
                                   << (static_cast<float>(m_timeTaken) / static_cast<float>(total)) << "ms/item ["
                                   << (static_cast<float>(m_timeTaken) / static_cast<float>(totalRequests)) << "ms/request]";
 #endif

    m_queue.switchContext();
}

bool LogRequestHandler::processRequest(LogRequest* request, Client** clientRef, bool forceCheck, Session *session)
{
    bool bypassChecks = !forceCheck && clientRef != nullptr && *clientRef != nullptr;
 #ifdef RESIDUE_DEV
    DRVLOG(RV_DEBUG_2) << "Force check: " << forceCheck << ", clientRef: " << clientRef << ", *clientRef: "
                     << (clientRef == nullptr ? "N/A" : *clientRef == nullptr ? "null" : (*clientRef)->id())
                     << ", bypassChecks: " << bypassChecks;
 #endif
    Client* client = clientRef != nullptr && *clientRef != nullptr ? *clientRef : request->client();

    if (client == nullptr) {
        RVLOG(RV_ERROR) << "Invalid request. No client found [" << request->clientId() << "]";
        return false;
    }

    if (!bypassChecks && !client->isAlive(request->dateReceived())) {
        RLOG(ERROR) << "Invalid request. Client is dead";
        RLOG(DEBUG) << "Req received: " << request->dateReceived() << ", client created: " << client->dateCreated() << ", age: " << client->age() << ", result: " << client->dateCreated() + client->age();
        return false;
    }

    request->setClientId(client->id());
    request->setClient(client);

    if (session != nullptr && session->client() == nullptr) {
        DRVLOG(RV_DEBUG) << "Updating session client";
        session->setClient(client);
    }

    if (!bypassChecks && client->isKnown()) {
        // take this opportunity to update the user for unknown logger

        // unknown loggers cannot be updated to specific user
        // without having a known client parent

        // make sure the current logger is unknown
        // otherwise we already know the user either from client or from logger itself
        if (m_registry->configuration()->hasFlag(Configuration::Flag::ALLOW_UNKNOWN_LOGGERS) // cannot be unknown logger unless server supports it
                && !m_registry->configuration()->isKnownLogger(request->loggerId())) {
            m_registry->configuration()->updateUnknownLoggerUserFromRequest(request->loggerId(), request);
        }
    }

    if (request->isValid()) {
        if (!bypassChecks && !isRequestAllowed(request)) {
            RLOG(WARNING) << "Ignoring log from unauthorized logger [" << request->loggerId() << "]";
            return false;
        }
        dispatch(request);
        return true;
    }
    return false;
}

void LogRequestHandler::dispatch(const LogRequest* request)
{
 #ifdef RESIDUE_DEV
    DRVLOG(RV_TRACE) << "Dispatching";
 #endif

 #ifdef RESIDUE_DEV
    DRVLOG(RV_TRACE) << "Writing";
 #endif
#if 0
    el::base::Writer(request->level(),
                     request->filename().c_str(),
                     request->lineNumber(),
                     request->function().c_str(),
                     el::base::DispatchAction::NormalLog,
                     request->verboseLevel()).construct(el::Loggers::getLogger(request->loggerId())) << request->msg();
#else

    KnownLoggerConfigurator* configurator = el::Loggers::loggerRegistrationCallback<KnownLoggerConfigurator>("KnownLoggerConfigurator");

    configurator->setLogRequest(request);

    el::Logger* logger = el::Loggers::getLogger(request->loggerId());
    el::base::threading::ScopedLock lock(logger->lock());

    el::base::MessageBuilder msgBuilder;
    msgBuilder.initialize(logger);
    msgBuilder << request->msg();

    UserMessage msg(request->level(), request->filename(), request->lineNumber(), request->function(), request->verboseLevel(), logger);
    msg.setRequest(request);

    std::string line = m_userLogBuilder->build(&msg, true);

    el::LogDispatchData data;
    data.setLogMessage(&msg);
    data.setDispatchAction(el::base::DispatchAction::NormalLog);

    ResidueLogDispatcher dispatcher;
    dispatcher.setConfiguration(m_registry->configuration());
    dispatcher.setLogLine(std::move(line));

    dispatcher.handle(&data);
#endif
 #ifdef RESIDUE_DEV
    DRVLOG(RV_TRACE) << "Write complete";
 #endif

 #ifdef RESIDUE_DEV
    DRVLOG(RV_TRACE) << "Dispatch complete";
 #endif
}

bool LogRequestHandler::isRequestAllowed(const LogRequest* request) const
{
    Client* client = request->client();
    if (client == nullptr) {
        RLOG(DEBUG) << "Client may have expired";
        return false;
    }
    // Ensure flag is on
    bool allowed = m_registry->configuration()->hasFlag(Configuration::Flag::ALLOW_UNKNOWN_LOGGERS);
    if (!allowed) {
        // we're not allowed to use unknown loggers. we make sure the current logger is actually known.
        allowed = m_registry->configuration()->isKnownLogger(request->loggerId());
    }
    if (allowed) {
         // We do not allow users to log using residue internal logger
        allowed = request->loggerId() != RESIDUE_LOGGER_ID;
    }
    if (allowed) {
         // Logger is blacklisted
        allowed = !m_registry->configuration()->isBlacklisted(request->loggerId());
    }
    if (allowed) {
        // Invalid token (either expired or not initialized)
        allowed = client->isValidToken(request->loggerId(), request->token(), m_registry, request->dateReceived());

        if (!allowed) {
            RLOG(WARNING) << "Token expired";
        }
    }
    return allowed;
}
