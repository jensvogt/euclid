// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// C++ includes
#include <algorithm>
#include <thread>
#include <utility>

// Mongodb includes
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <mongocxx/change_stream.hpp>
#include <mongocxx/exception/exception.hpp>
#include <mongocxx/options/change_stream.hpp>
#include <mongocxx/pipeline.hpp>

// Euclid includes
#include <euclid/core/LogStream.h>
#include <euclid/database/ChangeWaiters.h>
#include <euclid/database/Database.h>

namespace Euclid::Database {

    using namespace bsoncxx::builder::basic;

    ChangeWaiters::ChangeWaiters(std::string name, const std::chrono::milliseconds watchedFallback, const std::chrono::milliseconds polledFallback)
        : _name(std::move(name)), _watchedFallback(watchedFallback), _polledFallback(polledFallback) {}

    // ── Waiter ───────────────────────────────────────────────────────────────

    ChangeWaiters::Waiter::Waiter(ChangeWaiters &owner, std::string key, std::shared_ptr<Entry> entry)
        : _owner(owner), _key(std::move(key)), _entry(std::move(entry)) {}

    ChangeWaiters::Waiter::~Waiter() {
        _owner.release(_key);
    }

    std::uint64_t ChangeWaiters::Waiter::Generation() const {
        std::lock_guard lock(_owner._mutex);
        return _entry->generation;
    }

    void ChangeWaiters::Waiter::Wait(const std::uint64_t seen, const std::chrono::steady_clock::time_point deadline) const {
        const auto fallback = _owner.Watching() ? _owner._watchedFallback : _owner._polledFallback;
        const auto until = std::min(deadline, std::chrono::steady_clock::now() + fallback);

        std::unique_lock lock(_owner._mutex);
        _entry->cv.wait_until(lock, until, [&] { return _entry->generation != seen; });
    }

    // ── ChangeWaiters ────────────────────────────────────────────────────────

    ChangeWaiters::Waiter ChangeWaiters::Enroll(const std::string &key) {
        std::lock_guard lock(_mutex);
        auto &entry = _entries[key];
        if (!entry) entry = std::make_shared<Entry>();
        ++entry->waiters;
        return {*this, key, entry};
    }

    void ChangeWaiters::release(const std::string &key) {
        // A key nobody is waiting on is forgotten, so the map does not grow with every queue or
        // subscriber that was ever polled.
        std::lock_guard lock(_mutex);
        if (const auto it = _entries.find(key); it != _entries.end() && --it->second->waiters == 0) {
            _entries.erase(it);
        }
    }

    void ChangeWaiters::Wake(const std::string_view key) {
        std::lock_guard lock(_mutex);
        if (const auto it = _entries.find(std::string(key)); it != _entries.end()) {
            ++it->second->generation;
            it->second->cv.notify_all();
        }
    }

    void ChangeWaiters::Watch(const std::string &collection, bsoncxx::document::value match, const std::string &keyField) {
        std::call_once(_watchOnce, [this, &collection, &match, &keyField] {
            if (Database::instance().inMemory()) {
                log_info << _name << " polling only, no change stream on the in-memory backend";
                return;
            }
            std::thread([this, collection, match = std::move(match), keyField] { watchLoop(collection, match, keyField); }).detach();
        });
    }

    void ChangeWaiters::watchLoop(const std::string &collection, const bsoncxx::document::value &match, const std::string &keyField) {

        const auto keyPath = "fullDocument." + keyField;

        while (true) {
            try {
                const auto entry = Database::instance().client();
                auto watched = (*entry)[Database::instance().databaseName()][collection];

                mongocxx::pipeline pipeline;
                pipeline.match(match.view());
                pipeline.project(make_document(kvp(keyPath, 1)));

                mongocxx::options::change_stream options;
                options.max_await_time(std::chrono::seconds(30));

                auto stream = watched.watch(pipeline, options);
                _watching.store(true, std::memory_order_relaxed);
                log_info << _name << " change stream opened, collection: " << collection;

                // An empty begin() is a quiet max_await_time, not a dead stream: the same stream is
                // asked again rather than torn down. Only an exception ends it.
                for (auto it = stream.begin();; it = stream.begin()) {
                    for (; it != stream.end(); ++it) {
                        const auto full = (*it)["fullDocument"];
                        if (!full || full.type() != bsoncxx::type::k_document) continue;
                        if (const auto key = full.get_document().value[keyField]; key && key.type() == bsoncxx::type::k_string) {
                            Wake(std::string(key.get_string().value));
                        }
                    }
                }

            } catch (const mongocxx::exception &e) {

                // Back to polling while there is no stream, so nothing waits on a wake-up that
                // cannot come.
                _watching.store(false, std::memory_order_relaxed);
                if (e.code().value() == kChangeStreamUnsupported) {
                    log_warning << _name << " polling only, this MongoDB deployment is a standalone and has no oplog to tail;"
                                            " start it as a replica set to get immediate delivery";
                    return;
                }
                log_error << _name << " change stream failed, error: " << e.what() << ", reconnecting";

            } catch (const std::exception &e) {
                _watching.store(false, std::memory_order_relaxed);
                log_error << _name << " change stream failed, error: " << e.what() << ", reconnecting";
            }

            std::this_thread::sleep_for(kReconnectDelay);
        }
    }

}// namespace Euclid::Database
