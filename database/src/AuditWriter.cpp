// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Euclid includes
#include <euclid/core/Configuration.h>
#include <euclid/core/LogStream.h>
#include <euclid/database/AuditWriter.h>
#include <euclid/database/RepositoryFactory.h>

namespace Euclid::Database {

    namespace {

        // How many entries may wait to be written before the oldest is dropped. Generous enough to
        // ride out a slow moment, small enough that it cannot become the reason a module runs out
        // of memory.
        long queueLimit() {
            return std::max<long>(100, Core::Configuration::instance().getOr<long>("euclid.modules.ead.queue-limit", 10000));
        }

    }// namespace

    AuditWriter &AuditWriter::instance() {
        static AuditWriter writer;
        return writer;
    }

    AuditWriter::AuditWriter() : _worker([this] { Run(); }) {}

    AuditWriter::~AuditWriter() {
        {
            std::lock_guard lock(_mutex);
            _stopping = true;
        }
        _wakeup.notify_all();
        if (_worker.joinable()) _worker.join();
    }

    void AuditWriter::Write(const Entity::EAD::AuditEvent &event) {

        {
            std::lock_guard lock(_mutex);

            if (static_cast<long>(_queued.size()) >= queueLimit()) {
                // The oldest goes rather than the newest: what just happened is more likely to be
                // what somebody is about to look for. Logged every time the count crosses a power
                // of ten, so a persistent problem is visible without a log line per dropped entry.
                _queued.pop_front();
                ++_dropped;
                if (_dropped == 1 || _dropped % 1000 == 0) {
                    log_warning << "Audit queue full, entries dropped: " << _dropped
                                << " - the trail has gaps; see euclid.modules.ead.queue-limit";
                }
            }

            _queued.push_back(event);
        }
        _wakeup.notify_one();
    }

    void AuditWriter::Run() {

        for (;;) {

            Entity::EAD::AuditEvent event;
            {
                std::unique_lock lock(_mutex);
                _wakeup.wait(lock, [this] { return _stopping || !_queued.empty(); });

                // Drains before stopping: the entries already accepted are ones a caller was told
                // nothing about, so dropping them on shutdown would lose commands that ran.
                if (_queued.empty()) return;

                event = std::move(_queued.front());
                _queued.pop_front();
            }

            try {
                std::ignore = RepositoryFactory::instance().eadRepository()->createEvent(event);

            } catch (const std::exception &e) {
                // Nothing above this catch: this is a thread's entry path, and an exception
                // escaping it calls std::terminate() and takes the module down - to fail at
                // writing a line nobody is waiting for.
                log_error << "Audit write failed, module: " << event.moduleName
                          << ", command: " << event.command << ", error: " << e.what();
            }
        }
    }

}// namespace Euclid::Database
