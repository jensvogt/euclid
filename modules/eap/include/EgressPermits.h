// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace Euclid::EAP::Egress {

    /**
     * @file
     * @brief How fast euclid's applications may call one outside site, all of them together.
     *
     * @par
     * A site that defends itself against denial of service counts requests and open connections
     * per source address - and every instance of every application on every node leaves euclid
     * through the same one. A limit kept inside an application is a limit per instance, which the
     * autoscaler multiplies exactly when there is the most to fetch. So the limit is kept here, once
     * per site for the whole installation, and an application asks for a permit before each request.
     *
     * @par
     * A permit is two things: a token from the site's bucket (the rate) and a slot among the
     * requests to it in flight (the concurrency). Slots are leased, so an instance killed in the
     * middle of a download gives its slot back when the lease runs out rather than never.
     *
     * @par
     * Everything here is pure - state in, decision and new state out - so that it can be decided
     * once and stored by whichever EAP instance took the request; see EapServer for the storage.
     */

    using Clock = std::chrono::system_clock;

    /**
     * @brief What a site is allowed.
     */
    struct Limits {

        /**
         * @brief Requests per second, averaged; 0 or less for no rate limit.
         */
        double requestsPerSecond{2.0};

        /**
         * @brief How many requests may go in a burst, before the rate applies. At least 1.
         */
        double burst{2.0};

        /**
         * @brief Requests to the site in flight at once; 0 or less for no limit.
         */
        long concurrent{4};
    };

    /**
     * @brief A slot one request holds while it runs.
     */
    struct Permit {
        std::string id;
        Clock::time_point expiresAt;
    };

    /**
     * @brief Everything stored about one site between two requests for it.
     */
    struct SiteState {

        /**
         * @brief Tokens in the bucket as of refilledAt.
         */
        double tokens{0.0};

        /**
         * @brief When tokens was last brought up to date. The epoch for a site never asked for,
         * which fills the bucket on the first request.
         */
        Clock::time_point refilledAt{};

        /**
         * @brief Until when the site has asked to be left alone - a 429 or 503 an application
         * reported. Nothing is granted before then.
         */
        Clock::time_point blockedUntil{};

        /**
         * @brief The slots held, expired ones included until the next decision drops them.
         */
        std::vector<Permit> permits;
    };

    /**
     * @brief Why a permit was not granted.
     */
    enum class Refusal { None, Blocked, Concurrency, Rate };

    inline std::string RefusalToString(const Refusal refusal) {
        switch (refusal) {
            case Refusal::Blocked: return "blocked";
            case Refusal::Concurrency: return "concurrency";
            case Refusal::Rate: return "rate";
            case Refusal::None:
            default: return "";
        }
    }

    /**
     * @brief The answer to one request for a permit.
     */
    struct Decision {

        /**
         * @brief True if the caller may go ahead.
         */
        bool granted{};

        /**
         * @brief Why not, if not.
         */
        Refusal refusal{Refusal::None};

        /**
         * @brief How long to wait before asking again, if not. A hint: the slot or token may go to
         * somebody else in the meantime.
         */
        std::chrono::milliseconds retryAfter{0};

        /**
         * @brief The slot granted, if granted.
         */
        std::optional<Permit> permit;
    };

    /**
     * @brief The longest a caller is told to wait while every slot is taken.
     *
     * @par
     * Not when the earliest lease expires, which is usually much later than the request holding it
     * finishes and releases it. Asking again within a second costs one small request; waiting out a
     * fifteen-minute lease for a download that ended a moment later costs fifteen minutes.
     */
    inline constexpr std::chrono::milliseconds kConcurrencyRetry{1000};

    /**
     * @brief Brings the bucket up to `now`, and drops slots whose lease has run out.
     */
    inline void Refresh(SiteState &state, const Limits &limits, const Clock::time_point now) {

        const double burst = std::max(1.0, limits.burst);
        if (state.refilledAt == Clock::time_point{}) {
            state.tokens = burst;
        } else if (now > state.refilledAt && limits.requestsPerSecond > 0) {
            const double elapsed = std::chrono::duration<double>(now - state.refilledAt).count();
            state.tokens = std::min(burst, state.tokens + elapsed * limits.requestsPerSecond);
        }
        // A limit lowered since: the bucket holds no more than the new burst.
        state.tokens = std::min(state.tokens, burst);
        state.refilledAt = std::max(state.refilledAt, now);

        std::erase_if(state.permits, [now](const Permit &p) { return p.expiresAt <= now; });
    }

    /**
     * @brief Decides one request for a permit, and changes the state accordingly.
     *
     * @par
     * Checked in the order the reasons last: a site that asked to be left alone, then a full set of
     * slots, then an empty bucket. A token is taken only if the permit is granted, so a caller kept
     * waiting for a slot does not use up the rate meanwhile.
     *
     * @param state    the site's state, updated in place
     * @param limits   what the site is allowed
     * @param now      the time to decide at
     * @param permitId the id to give the permit, if one is granted
     * @param lease    how long the slot is held unless released first
     */
    inline Decision Acquire(SiteState &state, const Limits &limits, const Clock::time_point now,
                            const std::string &permitId, const std::chrono::seconds lease) {

        using std::chrono::ceil;
        using std::chrono::milliseconds;

        Refresh(state, limits, now);

        if (now < state.blockedUntil) {
            return {.granted = false, .refusal = Refusal::Blocked, .retryAfter = ceil<milliseconds>(state.blockedUntil - now)};
        }

        if (limits.concurrent > 0 && static_cast<long>(state.permits.size()) >= limits.concurrent) {
            auto wait = kConcurrencyRetry;
            for (const auto &p: state.permits) wait = std::min(wait, ceil<milliseconds>(p.expiresAt - now));
            return {.granted = false, .refusal = Refusal::Concurrency, .retryAfter = std::max(milliseconds{1}, wait)};
        }

        if (limits.requestsPerSecond > 0) {
            if (state.tokens < 1.0) {
                const auto wait = std::chrono::duration<double>((1.0 - state.tokens) / limits.requestsPerSecond);
                return {.granted = false, .refusal = Refusal::Rate,
                        .retryAfter = std::max(milliseconds{1}, ceil<milliseconds>(wait))};
            }
            state.tokens -= 1.0;
        }

        Permit permit{.id = permitId, .expiresAt = now + lease};
        state.permits.push_back(permit);
        return {.granted = true, .permit = std::move(permit)};
    }

    /**
     * @brief Gives a slot back before its lease runs out.
     *
     * @return true if it was held; false if it had expired or never existed - which is not an
     * error, the slot is free either way
     */
    inline bool Release(SiteState &state, const std::string &permitId) {
        return std::erase_if(state.permits, [&](const Permit &p) { return p.id == permitId; }) > 0;
    }

    /**
     * @brief The longest a site can be blocked on one report.
     *
     * @par
     * Retry-After is the site's to set, and a value of days - meant, or a bug - would otherwise stop
     * every application calling it without anyone having decided that. An hour, and a later report
     * blocks it again if it is still refusing.
     */
    inline constexpr std::chrono::seconds kMaxBlock{3600};

    /**
     * @brief Records that the site refused a request and asked to be left alone for a while.
     *
     * @par
     * Only ever moves blockedUntil later: two instances reporting the same refusal, one with a
     * shorter Retry-After, must not shorten what the other was told.
     *
     * @return the time the site is now blocked until
     */
    inline Clock::time_point Block(SiteState &state, const Clock::time_point now, const std::chrono::seconds retryAfter) {
        const auto until = now + std::clamp(retryAfter, std::chrono::seconds{1}, kMaxBlock);
        state.blockedUntil = std::max(state.blockedUntil, until);
        return state.blockedUntil;
    }

    /**
     * @brief The name a site is stored under: its host, lower-cased, without a trailing dot.
     *
     * @par
     * The host and not the URL or the host and port: a site's defence counts per address it is
     * reached at, and two ports of one host are the same machine counting.
     *
     * @return the name, or an empty string if `site` cannot be one
     */
    inline std::string NormalizeSite(std::string site) {
        std::ranges::transform(site, site.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
        while (!site.empty() && site.back() == '.') site.pop_back();
        if (site.empty() || site.size() > 253) return {};
        for (const char c: site) {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == ':' || c == '[' || c == ']';
            if (!ok) return {};
        }
        return site;
    }

}// namespace Euclid::EAP::Egress
