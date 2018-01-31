//
//  rotate.cc
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

#include "src/plugins/rotate.h"
#include "src/core/registry.h"
#include "src/tasks/log-rotator.h"

using namespace residue;

Rotate::Rotate(Registry* registry) :
    Plugin("rotate",
              "Trigger log rotation manuall",
              "rotate --logger-id <id> [--check-only] [--ignore-archive]",
              registry)
{
}

void Rotate::execute(std::vector<std::string>&& params, std::ostringstream& result, bool ignoreConfirmation) const
{    
    const std::string loggerId = getParamValue(params, "--logger-id");
    if (loggerId.empty()) {
        result << "\nNo logger ID provided" << std::endl;
        return;
    }
    if (el::Loggers::getLogger(loggerId, false) == nullptr) {
        result << "Logger [" << loggerId << "] not yet registered" << std::endl;
        return;
    }
    if (hasParam(params, "--check-only")) {
        result << registry()->logRotator()->checkStatus(loggerId) << std::endl;
        return;
    }
    if (ignoreConfirmation || getConfirmation("This ignores the original rotation frequency and forces to run the rotation.\n"
                                              "This will change the rotation schedule for this logger.\n\n"
                                              "--check-only: To only check the next scheduled time\n"
                                              "--ignore-archive: To run the log rotation (if no --check-only provided) but do not archive and compress\n")) {
        if (registry()->logRotator()->isExecuting()) {
            result << "Log rotator already running, please try later\n";
            return;
        }
        result << "Rotating logs for [" << loggerId << "]\n";
        registry()->logRotator()->rotate(loggerId);
        if (!hasParam(params, "--ignore-archive")) {
            result << "Archiving logs for [" << loggerId << "]\n";
            registry()->logRotator()->archiveRotatedItems();
        } else {
            result << "Ignoring archive rotated logs for [" << loggerId << "]\n";
        }
    }
}