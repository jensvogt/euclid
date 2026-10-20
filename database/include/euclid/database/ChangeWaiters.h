// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

// Mongodb includes
#include <bsoncxx/document/value.hpp>

namespace Euclid::Database {

    /**
     * @brief Long-poll waiters that are woken when there may be something for them, instead of
     * asking the database again on a timer.
     *
     * @par
     * A long poll used to be a loop: query, find nothing, sleep a fraction of a second, query
     * again. On an idle queue or subscriber that is every query of the wait answering nothing -
     * eighty for one twenty-second wait at 250 ms, per waiting client - and it is the larger part
     * of what a pool of consumers costs the database. Here a waiter sleeps on a condition variable
     * under a key (a queue ERN, a subscriber), and something that knows the key may have work
     * wakes it: a change stream on the collection, started with Watch(), for a write made by any
     * process, and Wake() for one made by this process.
     *
     * @par
     * A wake-up only ever means "look again". Whatever claims the work still does so atomically,
     * so two waiters woken by one insert cannot both get it, and a missed wake-up costs latency,
     * never correctness: the wait is capped at a fallback interval in any case - long while the
     * change stream is running, as short as the old poll while it is not.
     *
     * @par Usage
     * @code
     * auto waiter = waiters.Enroll(queueErn);
     * while (true) {
     *     const auto seen = waiter.Generation();     // before looking, not after
     *     if (auto work = claim(); !work.empty()) return work;
     *     if (now() >= deadline) return {};
     *     waiter.Wait(seen, deadline);
     * }
     * @endcode
     *
     * @author jensvogt47\@gmail.com
     */
    class ChangeWaiters {

        struct Entry {
            std::condition_variable cv;
            std::uint64_t generation = 0;
            int waiters = 0;
        };

    public:

        /**
         * @param name what these waiters are for, for the log, e.g. "EQS receive".
         * @param watchedFallback longest a wait lasts without a wake-up while the change stream is
         * running - a safety net for what the stream does not report.
         * @param polledFallback the same without a change stream, which is then the poll interval.
         */
        ChangeWaiters(std::string name, std::chrono::milliseconds watchedFallback, std::chrono::milliseconds polledFallback);

        ChangeWaiters(const ChangeWaiters &) = delete;

        ChangeWaiters &operator=(const ChangeWaiters &) = delete;

        /**
         * @brief One caller waiting under one key, for as long as the object lives.
         */
        class Waiter {
        public:

            Waiter(ChangeWaiters &owner, std::string key, std::shared_ptr<Entry> entry);

            Waiter(const Waiter &) = delete;

            Waiter &operator=(const Waiter &) = delete;

            ~Waiter();

            /**
             * @brief The current generation. Read it before looking for work: a wake-up that lands
             * between the look and the Wait() has moved it on, and the Wait() returns at once.
             */
            [[nodiscard]]
            std::uint64_t Generation() const;

            /**
             * @brief Blocks until woken past seen, until the deadline, or until the fallback
             * interval runs out, whichever is first.
             */
            void Wait(std::uint64_t seen, std::chrono::steady_clock::time_point deadline) const;

        private:

            ChangeWaiters &_owner;
            std::string _key;
            std::shared_ptr<Entry> _entry;
        };

        /**
         * @brief Registers a waiter under key. Call before the first look for work.
         */
        [[nodiscard]]
        Waiter Enroll(const std::string &key);

        /**
         * @brief Wakes everything waiting under key in this process. Cheap when nothing is.
         */
        void Wake(std::string_view key);

        /**
         * @brief Starts the change stream that wakes waiters for writes made by any process.
         *
         * @par
         * One detached thread, started once whatever number of times this is called. The stream
         * is filtered on the server by match and projected to keyField alone, so neither unrelated
         * writes nor document bodies cross the wire. A no-op on the in-memory and EMD backends,
         * which have no change stream, and given up for good on a standalone MongoDB, which has
         * no oplog to tail; waiters then wait at the polled fallback.
         *
         * @param collection collection to watch.
         * @param match $match stage over the change event, e.g. inserts whose fullDocument.status
         * is AVAILABLE.
         * @param keyField field of the inserted document holding the key, e.g. "queueErn".
         */
        void Watch(const std::string &collection, bsoncxx::document::value match, const std::string &keyField);

        /**
         * @brief Whether the change stream is currently open.
         */
        [[nodiscard]]
        bool Watching() const { return _watching.load(std::memory_order_relaxed); }

    private:

        void watchLoop(const std::string &collection, const bsoncxx::document::value &match, const std::string &keyField);

        void release(const std::string &key);

        std::string _name;
        std::chrono::milliseconds _watchedFallback;
        std::chrono::milliseconds _polledFallback;

        std::mutex _mutex;
        std::unordered_map<std::string, std::shared_ptr<Entry> > _entries;

        std::once_flag _watchOnce;
        std::atomic<bool> _watching{false};

        static constexpr auto kReconnectDelay = std::chrono::seconds(2);
        // "The $changeStream stage is only supported on replica sets or mongos".
        static constexpr int kChangeStreamUnsupported = 40573;
    };

}// namespace Euclid::Database
