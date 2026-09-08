//
// Created by vogje01 on 9/8/26.
//

#ifndef EUCLID_CORE_TTL_CACHE_H
#define EUCLID_CORE_TTL_CACHE_H

// C++ includes
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace Euclid::Core {

    /**
     * @brief A small map that remembers an answer for a while, for lookups that are asked the same
     * question far more often than the answer changes.
     *
     * It exists for one situation in particular: authenticating a request. Resolving an access key
     * to its secret, and a user to their grants, are database reads that happen on every single
     * authenticated request - twice, since the gateway and the module each authenticate - while the
     * rows behind them change perhaps once a month. Without this, a busy installation spends most
     * of its request latency asking the database questions it already knows the answer to.
     *
     * <b>What the TTL costs.</b> A cached answer is a stale answer for up to the TTL, and these are
     * authorisation answers: for that long after an access key is deactivated or a grant is
     * revoked, a request presenting it can still be accepted. That is the whole of the trade, it is
     * why the TTL is measured in seconds rather than minutes, and it is why the value is
     * configurable - an installation that would rather pay the latency can set it to zero, which
     * turns the cache off entirely.
     *
     * <b>Misses are remembered too.</b> An unknown access key is a common thing to be asked about -
     * a stale client, a scan - and looking it up every time makes an unauthenticated caller more
     * expensive to serve than an authenticated one. The absence is cached like any other answer.
     *
     * <b>Size.</b> Bounded, and when the bound is reached the expired entries go first and the
     * whole map second. That is cruder than evicting least-recently-used, and deliberately so:
     * the population here is an installation's users and access keys, which is small enough that
     * the bound exists to stop unbounded growth from unknown keys rather than to manage a working
     * set that genuinely does not fit.
     *
     * Instances are safe to share between threads.
     */
    template<typename Key, typename Value>
    class TtlCache {

      public:

        /**
         * @brief Creates a cache holding answers for @p ttl.
         *
         * @param ttl how long an answer may be reused; zero disables caching entirely, so every
         * lookup goes to the loader
         * @param maxEntries the most entries to hold before the cache is emptied
         */
        explicit TtlCache(const std::chrono::milliseconds ttl, const std::size_t maxEntries = 4096)
            : _ttl(ttl), _maxEntries(maxEntries) {}

        /**
         * @brief The answer for @p key, from the cache when it is still fresh and from @p loader
         * otherwise.
         *
         * The loader runs outside the lock, so a slow database read does not hold up every other
         * thread's cache hits. Two threads asking for the same missing key at the same moment will
         * therefore both load it; they get the same answer and the second simply overwrites the
         * first, which is cheaper than making one of them wait for the other.
         *
         * @param key what to look up
         * @param loader produces the answer when there is no fresh one, including the answer "there
         * is none", which is remembered too
         * @return the answer, or empty if there is none
         */
        std::optional<Value> get(const Key &key, const std::function<std::optional<Value>(const Key &)> &loader) {

            if (_ttl.count() <= 0) {
                return loader(key);
            }

            const auto now = std::chrono::steady_clock::now();
            {
                std::lock_guard lock(_mutex);
                if (const auto entry = _entries.find(key); entry != _entries.end() && entry->second.expiresAt > now) {
                    return entry->second.value;
                }
            }

            std::optional<Value> loaded = loader(key);

            std::lock_guard lock(_mutex);
            if (_entries.size() >= _maxEntries) {
                evict(now);
            }
            _entries[key] = Entry{.value = loaded, .expiresAt = now + _ttl};
            return loaded;
        }

        /**
         * @brief Forgets everything, so the next lookup of anything reaches the database.
         *
         * For the caller that has just changed one of the rows behind the cache and would rather
         * not wait out the TTL to see it.
         */
        void clear() {
            std::lock_guard lock(_mutex);
            _entries.clear();
        }

      private:

        struct Entry {
            std::optional<Value> value;
            std::chrono::steady_clock::time_point expiresAt;
        };

        /**
         * @brief Makes room. Called with the lock held.
         */
        void evict(const std::chrono::steady_clock::time_point &now) {
            std::erase_if(_entries, [&now](const auto &entry) { return entry.second.expiresAt <= now; });
            if (_entries.size() >= _maxEntries) {
                _entries.clear();
            }
        }

        const std::chrono::milliseconds _ttl;
        const std::size_t _maxEntries;

        std::mutex _mutex;
        std::unordered_map<Key, Entry> _entries;
    };

}// namespace Euclid::Core

#endif// EUCLID_CORE_TTL_CACHE_H
