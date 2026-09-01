#pragma once

// C++ includes
#include <algorithm>
#include <cstddef>
#include <deque>
#include <string>
#include <utility>

namespace Euclid::main {

    /**
     * @brief The frames waiting to go out on one websocket connection, and what happens when a
     * client stops reading them.
     *
     * @par
     * A connection whose peer has stopped reading fills its write queue with everything the
     * gateway wanted to send it, and without a bound that queue grows until the process dies -
     * one stalled client taking the gateway with it. So the queue is bounded, and the interesting
     * question is what to give up when it is full.
     *
     * @par
     * Events are given up. A durable subscriber still has them in the store and can claim them,
     * and a live subscriber was never promised history - so dropping an event costs a client the
     * chance to be told about it, not the event itself. Responses are never given up: a client is
     * blocked waiting for each one, and dropping it would hang the call rather than slow it down.
     * A connection whose backlog is responses alone is therefore not slow but gone, and saying so
     * (Push() reports the overflow, and the session closes) is better than holding frames for
     * somebody who will never read them.
     *
     * @par
     * Dropped events are counted rather than forgotten, so the client can be told it missed some
     * once it starts reading again - see TakeDropped(). For a durable subscription that is a
     * precise instruction: the events are still there, come and claim them.
     *
     * @par
     * Not thread-safe by design: a session drives this from handlers on its own websocket
     * executor, never concurrently, which is also what lets Front() hand out a reference that
     * stays valid for the duration of an async_write.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class Outbox {

    public:

        /**
         * @brief What the caller should do after a Push().
         */
        enum class Result {
            /** @brief Nothing is being written; start the write loop. */
            Idle,
            /** @brief A write is already in flight; it will pick this frame up. */
            Writing,
            /** @brief Still over the bound with nothing left to drop - close the connection. */
            Overflow,
        };

        Outbox(const std::size_t maxFrames, const std::size_t maxBytes) : _maxFrames(maxFrames), _maxBytes(maxBytes) {}

        /**
         * @brief Queues one frame, dropping the oldest droppable events if that puts the queue
         * over its bounds.
         *
         * @param frame the serialized frame.
         * @param event whether it is a pushed event, i.e. whether it may be dropped.
         * @return what the caller should do next.
         */
        Result Push(std::string frame, const bool event) {

            _queuedBytes += frame.size();
            _frames.push_back(Entry{.text = std::move(frame), .event = event});

            if (over()) {
                // The frame at the front while a write is in flight is the one async_write is
                // reading from, so it is never a candidate - everything after it is.
                auto it = _frames.begin() + static_cast<std::ptrdiff_t>(_writing ? 1 : 0);
                while (it != _frames.end() && over()) {
                    if (!it->event) {
                        ++it;
                        continue;
                    }
                    _queuedBytes -= it->text.size();
                    it = _frames.erase(it);
                    ++_dropped;
                }
            }

            if (over()) return Result::Overflow;
            return _writing ? Result::Writing : Result::Idle;
        }

        /**
         * @brief The frame to write next. Valid until CompleteWrite(), which is what an
         * async_write's buffer needs.
         */
        [[nodiscard]] const std::string &Front() const { return _frames.front().text; }

        /**
         * @brief Marks a write as started, so the frame being written is not dropped underneath it.
         */
        void BeginWrite() { _writing = true; }

        /**
         * @brief Marks the front frame as written and removes it.
         */
        void CompleteWrite() {
            _writing = false;
            if (_frames.empty()) return;
            _queuedBytes -= _frames.front().text.size();
            _frames.pop_front();
        }

        /**
         * @brief Marks a failed write, leaving the queue alone - the session is closing anyway.
         */
        void FailWrite() { _writing = false; }

        [[nodiscard]] bool Empty() const { return _frames.empty(); }

        [[nodiscard]] std::size_t Size() const { return _frames.size(); }

        [[nodiscard]] std::size_t QueuedBytes() const { return _queuedBytes; }

        /**
         * @brief How many events were dropped since this was last asked, resetting the count.
         *
         * @return the number dropped, or 0 if none were.
         */
        long TakeDropped() { return std::exchange(_dropped, 0L); }

    private:

        struct Entry {
            std::string text;
            bool event = false;
        };

        [[nodiscard]] bool over() const { return _frames.size() > _maxFrames || _queuedBytes > _maxBytes; }

        std::size_t _maxFrames;
        std::size_t _maxBytes;
        std::deque<Entry> _frames;
        std::size_t _queuedBytes = 0;
        bool _writing = false;
        long _dropped = 0;
    };

}// namespace Euclid::main
