//
//  log-request.cc
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

#include <ctime>
#include "logging/log-request.h"
#include "core/json-document.h"

using namespace residue;

LogRequest::LogRequest(const Configuration* conf) :
    Request(conf),
    m_msg(LogRequestFieldMessage.defaultValue),
    m_isValid(true)
{
}

bool LogRequest::deserialize(std::string&& json)
{
    if (Request::deserialize(std::move(json))) {
        m_clientId = m_jsonDoc.getString("client_id", "");

        m_datetime = resolveValue<types::TimeMs>(&m_jsonDoc, &LogRequestFieldDateTime);
        m_token = resolveValue<std::string>(&m_jsonDoc, &LogRequestFieldToken);
        m_loggerId = resolveValue<std::string>(&m_jsonDoc, &LogRequestFieldLogger);
        m_filename = resolveValue<std::string>(&m_jsonDoc, &LogRequestFieldFile);
        m_function = resolveValue<std::string>(&m_jsonDoc, &LogRequestFieldFunction);
        m_applicationName = resolveValue<std::string>(&m_jsonDoc, &LogRequestFieldApplicationName);
        m_threadId = resolveValue<std::string>(&m_jsonDoc, &LogRequestFieldThreadId);
        m_msg = resolveValue<std::string>(&m_jsonDoc, &LogRequestFieldMessage);
        m_lineNumber = static_cast<el::base::type::LineNumber>(resolveValue<el::base::type::LineNumber>(&m_jsonDoc, &LogRequestFieldLine));
        m_level = el::LevelHelper::castFromInt(resolveValue<el::base::type::EnumType>(&m_jsonDoc, &LogRequestFieldLevel));
        m_verboseLevel = resolveValue<el::base::type::VerboseLevel>(&m_jsonDoc, &LogRequestFieldVLevel);

        m_isValid = m_datetime != 0L && !m_loggerId.empty() && !m_msg.empty();
    }
    return m_isValid;
}

bool LogRequest::validateTimestamp() const
{
    // always valid for log request
    return true;
}

std::string LogRequest::formattedDatetime(const char* format, const el::base::MillisecondsWidth* msWidth) const
{
    struct timeval tval;
    types::TimeMs epochInMs = datetime();
    tval.tv_sec = epochInMs / 1000;
    tval.tv_usec = epochInMs % 1000000;
    return el::base::utils::DateTime::timevalToString(tval, format, msWidth);
}
