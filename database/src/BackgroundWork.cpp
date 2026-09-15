// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// C++ includes
#include <atomic>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <utility>

// Euclid includes
#include <euclid/core/LogStream.h>
#include <euclid/database/BackgroundWork.h>
#include <euclid/database/RepositoryFactory.h>

namespace Euclid::Database {

    namespace {

        // Outstanding background tasks in this process, whatever kind they are. One counter rather
        // than one per command, because the manager's question is "is this instance busy with
        // something I cannot see", and two purges and a resend answer it the same way.
        std::atomic<long> outstanding{0};

        // Tells the manager the current figure. Called whenever it moves, so a scale-down decision
        // one second later reads something current rather than a five-minute-old average.
        void report(const std::string &moduleName) {
            try {
                RepositoryFactory::instance().emmRepository()->reportBackgroundTasks(
                        moduleName, InstanceName(), outstanding.load());
            } catch (const std::exception &e) {
                // Advisory. The work carries on either way, and for a purge it is the job document
                // rather than this figure that makes it survive being stopped.
                log_debug << "Could not report background work, module: " << moduleName << ", error: " << e.what();
            }
        }

    }// namespace

    const std::string &InstanceName() {
        static const std::string kName = [] {
            if (const char *id = std::getenv("EUCLID_INSTANCE_ID"); id != nullptr && *id != '\0') return std::string(id);
            return "pid-" + std::to_string(static_cast<long>(::getpid()));
        }();
        return kName;
    }

    std::shared_ptr<BackgroundWork> BackgroundWork::Begin(std::string moduleName) {
        // make_shared cannot reach a private constructor, and the constructor is private so that
        // the only way to hold one of these is the shared handle the thread captures.
        return std::shared_ptr<BackgroundWork>(new BackgroundWork(std::move(moduleName)));
    }

    BackgroundWork::BackgroundWork(std::string moduleName) : _moduleName(std::move(moduleName)) {
        ++outstanding;
        report(_moduleName);
    }

    BackgroundWork::~BackgroundWork() {
        --outstanding;
        report(_moduleName);
    }

}// namespace Euclid::Database
