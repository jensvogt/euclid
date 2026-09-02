//
// Created by vogje01 on 9/2/26.
//

#pragma once

// C++ includes
#include <algorithm>
#include <atomic>

namespace Euclid::Core {

    /**
     * @brief Keeps long-polling requests from occupying every worker thread a module has.
     *
     * @par
     * A long poll is a request that deliberately does nothing for up to twenty seconds, waiting
     * for something to arrive. It is the right shape for "tell me when there is work" - an empty
     * answer that took twenty seconds beats a client asking four times a second - but it holds a
     * worker thread for its whole duration, and a module runs a small fixed pool of them.
     *
     * @par
     * With nothing to stop it, that pool fills. Two clients politely waiting for events on a
     * two-thread module leave nothing to answer anything else, and the next request queues behind
     * them for as long as their wait lasts. It does not fail, which is the difficult part: it sits
     * in the accept queue and is served the moment a poll returns, so what the caller sees is a
     * request that timed out against a module whose log shows it healthy and idle - and, twenty
     * seconds later, shows it handling that very request just after the caller gave up on it.
     *
     * @par
     * This bounds how many threads may be spent waiting, leaving at least one for requests that
     * have actual work to do. A poll that cannot get a slot does not queue and does not fail: it
     * answers immediately with whatever is there, which is what a long poll degrades to anyway,
     * and the client asks again. Subscribing, acknowledging and metrics therefore always find a
     * thread, however many clients are waiting.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class LongPollSlots {
    public:

        /**
         * @brief A slot held for as long as the object lives.
         *
         * @par
         * held() being false is not a failure - it means the caller should answer now rather than
         * wait - so this is deliberately not something that can be ignored into a leak: the count
         * is released by the destructor whichever way the handler returns.
         */
        class Slot {
        public:

            Slot(LongPollSlots *owner, const bool held) : _owner(owner), _held(held) {}

            Slot(const Slot &) = delete;
            Slot &operator=(const Slot &) = delete;

            ~Slot() {
                if (_held) _owner->_inUse.fetch_sub(1, std::memory_order_relaxed);
            }

            /**
             * @brief Whether this request may spend a thread waiting.
             */
            [[nodiscard]]
            bool held() const { return _held; }

        private:

            LongPollSlots *_owner;
            bool _held;
        };

        /**
         * @brief How many threads may be spent waiting at once.
         *
         * @param limit the cap; at least one, so a single-threaded module can still long poll -
         * there is nothing to protect there, since one request is all it can serve either way.
         */
        void limit(const int limit) { _limit.store(std::max(1, limit), std::memory_order_relaxed); }

        /**
         * @brief Takes a slot if one is free.
         *
         * @return a slot whose held() says whether the caller may wait.
         */
        [[nodiscard]]
        Slot acquire() {
            const int limit = _limit.load(std::memory_order_relaxed);
            int current = _inUse.load(std::memory_order_relaxed);
            while (current < limit) {
                if (_inUse.compare_exchange_weak(current, current + 1, std::memory_order_relaxed)) {
                    return {this, true};
                }
            }
            return {this, false};
        }

    private:

        std::atomic<int> _limit{1};
        std::atomic<int> _inUse{0};
    };

}// namespace Euclid::Core
