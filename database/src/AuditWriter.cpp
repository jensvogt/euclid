// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// C++ includes
#include <algorithm>
#include <tuple>
#include <vector>

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

        // How many entries one insert carries. Large enough that the round trip is amortised to
        // nothing, small enough to stay well inside any server's document-per-batch limit and to
        // keep the queue's lock held only briefly while they are taken off it.
        constexpr size_t kWriteBatch = 500;

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

            std::vector<Entity::EAD::AuditEvent> batch;
            {
                std::unique_lock lock(_mutex);
                _wakeup.wait(lock, [this] { return _stopping || !_queued.empty(); });

                // Drains before stopping: the entries already accepted are ones a caller was told
                // nothing about, so dropping them on shutdown would lose commands that ran.
                if (_queued.empty()) return;

                // Everything waiting, up to a bound, rather than one entry.
                //
                // This is what the queue was for and what nothing used. One insert per entry made
                // this thread the ceiling on how fast a module could be audited: entries arrived
                // at about 1,035 a second against a writer nowhere near that, so Write() started
                // discarding the oldest to make room - 46,000 of them in one process. The queue
                // was not absorbing a burst, it was where the trail went to be lost.
                //
                // Bounded so that a process that has fallen a long way behind still writes
                // something promptly, and so one insert cannot grow past what a server will take.
                const auto take = std::min<size_t>(_queued.size(), kWriteBatch);
                batch.reserve(take);
                for (size_t i = 0; i < take; ++i) {
                    batch.push_back(std::move(_queued.front()));
                    _queued.pop_front();
                }
            }

            try {
                std::ignore = RepositoryFactory::instance().eadRepository()->createEvents(batch);

            } catch (const std::exception &e) {
                // Nothing above this catch: this is a thread's entry path, and an exception
                // escaping it calls std::terminate() and takes the module down - to fail at
                // writing lines nobody is waiting for.
                log_error << "Audit write failed, entries: " << batch.size() << ", error: " << e.what();
            }
        }
    }

}// namespace Euclid::Database
