// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

// Mongo includes
#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <mongocxx/options/index.hpp>

// Euclid includes
#include <EgressPermits.h>
#include <euclid/core/Configuration.h>
#include <euclid/core/DateTimeUtils.h>
#include <euclid/core/LogStream.h>
#include <euclid/database/Database.h>

namespace Euclid::EAP::Egress {

    /**
     * @file
     * @brief Where the egress state is kept: one MongoDB document per site.
     *
     * @par
     * In the database rather than in memory, because EAP runs as many as maxInstances processes and
     * every one of them must see the same bucket: kept in memory, each would hand out the full rate
     * and the site would see it times the pool size - the very failure permits exist to prevent.
     * Each decision reads the document, decides, and writes it back only if nobody else wrote it in
     * between (its version); otherwise it reads again. At the rate an outside site can be called at
     * all, collisions are rare and a retry costs a millisecond.
     */

    inline constexpr auto kEgressCollection = "eap_egress_site";

    // A request still colliding after this many rounds is answered "try again shortly" rather
    // than kept spinning on an EAP thread.
    inline constexpr int kEgressAttempts = 16;

    inline long long toMillis(const Clock::time_point t) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
    }

    inline Clock::time_point fromMillis(const long long ms) {
        return Clock::time_point{std::chrono::milliseconds{ms}};
    }

    inline std::string isoOrEmpty(const Clock::time_point t) {
        return t == Clock::time_point{} ? std::string() : Core::DateTimeUtils::ToISO8601(t);
    }

    // What a site gets when nobody has set a limit for it: euclid.modules.eap.egress.default.
    inline Limits defaultEgressLimits() {
        const auto &c = Core::Configuration::instance();
        Limits limits;
        limits.requestsPerSecond = c.getOr<double>("euclid.modules.eap.egress.default.requests-per-second", limits.requestsPerSecond);
        limits.burst = c.getOr<double>("euclid.modules.eap.egress.default.burst", limits.burst);
        limits.concurrent = c.getOr<long>("euclid.modules.eap.egress.default.concurrent", limits.concurrent);
        return limits;
    }

    // The site name a limit is stored under to apply to all of an application's sites: its own
    // default, in between a limit for one site and euclid.modules.eap.egress.default.
    inline constexpr auto kAllSites = "*";

    // One application's state for one site, as read. Everything is per application: two
    // applications calling the same site each have their own bucket, slots and limits - the
    // downloader that must go gently does not slow down the one that needs not.
    struct StoredSite {
        std::string application;  // the application's ERN - what keys the state
        std::string applicationId;// its id, for display
        std::string site;
        long long version{-1};// -1: no document yet
        SiteState state;
        std::optional<Limits> limits;// set with set-egress-limit; otherwise see limitsFor()
        Clock::time_point modified{};
    };

    inline double numberOr(const bsoncxx::document::view doc, const std::string_view key, const double fallback) {
        const auto e = doc[key];
        if (!e) return fallback;
        switch (e.type()) {
            case bsoncxx::type::k_double: return e.get_double().value;
            case bsoncxx::type::k_int32: return e.get_int32().value;
            case bsoncxx::type::k_int64: return static_cast<double>(e.get_int64().value);
            default: return fallback;
        }
    }

    inline long long int64Or(const bsoncxx::document::view doc, const std::string_view key, const long long fallback) {
        return static_cast<long long>(numberOr(doc, key, static_cast<double>(fallback)));
    }

    inline std::string stringOr(const bsoncxx::document::view doc, const std::string_view key) {
        const auto e = doc[key];
        return e && e.type() == bsoncxx::type::k_string ? std::string(e.get_string().value) : std::string();
    }

    inline StoredSite parseSite(const bsoncxx::document::view doc) {
        StoredSite s;
        s.application = stringOr(doc, "application");
        s.applicationId = stringOr(doc, "applicationId");
        s.site = stringOr(doc, "site");
        s.version = int64Or(doc, "version", 0);
        s.state.tokens = numberOr(doc, "tokens", 0.0);
        s.state.refilledAt = fromMillis(int64Or(doc, "refilledAt", 0));
        s.state.blockedUntil = fromMillis(int64Or(doc, "blockedUntil", 0));
        s.modified = fromMillis(int64Or(doc, "modified", 0));
        if (const auto e = doc["permits"]; e && e.type() == bsoncxx::type::k_array) {
            for (const auto &p: e.get_array().value) {
                if (p.type() != bsoncxx::type::k_document) continue;
                const auto pd = p.get_document().value;
                if (const auto id = pd["id"]; id && id.type() == bsoncxx::type::k_string) {
                    s.state.permits.push_back({.id = std::string(id.get_string().value),
                                               .expiresAt = fromMillis(int64Or(pd, "expiresAt", 0))});
                }
            }
        }
        if (const auto e = doc["limits"]; e && e.type() == bsoncxx::type::k_document) {
            const auto ld = e.get_document().value;
            const auto defaults = defaultEgressLimits();
            s.limits = Limits{.requestsPerSecond = numberOr(ld, "requestsPerSecond", defaults.requestsPerSecond),
                              .burst = numberOr(ld, "burst", defaults.burst),
                              .concurrent = int64Or(ld, "concurrent", defaults.concurrent)};
        }
        return s;
    }

    inline bsoncxx::document::value siteDocument(const StoredSite &s, const long long version, const Clock::time_point now) {
        using bsoncxx::builder::basic::kvp;
        using bsoncxx::builder::basic::make_document;
        bsoncxx::builder::basic::array permits;
        for (const auto &p: s.state.permits) {
            permits.append(make_document(kvp("id", p.id), kvp("expiresAt", static_cast<std::int64_t>(toMillis(p.expiresAt)))));
        }
        bsoncxx::builder::basic::document doc;
        doc.append(kvp("application", s.application),
                   kvp("applicationId", s.applicationId),
                   kvp("site", s.site),
                   kvp("version", static_cast<std::int64_t>(version)),
                   kvp("tokens", s.state.tokens),
                   kvp("refilledAt", static_cast<std::int64_t>(toMillis(s.state.refilledAt))),
                   kvp("blockedUntil", static_cast<std::int64_t>(toMillis(s.state.blockedUntil))),
                   kvp("permits", permits),
                   kvp("modified", static_cast<std::int64_t>(toMillis(now))));
        if (s.limits) {
            doc.append(kvp("limits", make_document(kvp("requestsPerSecond", s.limits->requestsPerSecond),
                                                   kvp("burst", s.limits->burst),
                                                   kvp("concurrent", static_cast<std::int64_t>(s.limits->concurrent)))));
        }
        return doc.extract();
    }

    inline bsoncxx::document::value siteKey(const std::string &application, const std::string &site) {
        using bsoncxx::builder::basic::kvp;
        return bsoncxx::builder::basic::make_document(kvp("application", application), kvp("site", site));
    }

    inline Database::Collection egressCollection() {
        // The unique index is what keeps two EAP instances meeting a new site at the same moment
        // from creating it twice. Made once per process; creating an index that exists is a no-op.
        static std::once_flag indexed;
        auto collection = Database::Database::instance().collection(kEgressCollection);
        std::call_once(indexed, [&] {
            try {
                // The index of the first, installation-wide version, unique on site alone - which
                // would refuse a second application's state for a site the first one already has.
                collection.drop_index("site_1");
                mongocxx::options::index options;
                options.unique(true);
                collection.create_index(bsoncxx::builder::basic::make_document(bsoncxx::builder::basic::kvp("application", 1),
                                                                               bsoncxx::builder::basic::kvp("site", 1)),
                                        options);
            } catch (const std::exception &e) {
                log_warning << "EAP egress: cannot create the unique index on " << kEgressCollection << ": " << e.what();
            }
        });
        return collection;
    }

    inline std::optional<StoredSite> loadSite(const Database::Collection &collection, const std::string &application, const std::string &site) {
        const auto doc = collection.find_one(siteKey(application, site));
        if (!doc) return std::nullopt;
        return parseSite(doc->view());
    }

    // What one application's state for one site is held to: its own limit for that site, else the
    // application's limit for all its sites, else euclid.modules.eap.egress.default.
    inline Limits limitsFor(const StoredSite &s) {
        if (s.limits) return *s.limits;
        if (s.site != kAllSites) {
            if (const auto all = loadSite(egressCollection(), s.application, kAllSites); all && all->limits) return *all->limits;
        }
        return defaultEgressLimits();
    }

    // Writes `s` if its document is still at the version it was read at. False if somebody
    // else got there first - the caller reads again and decides again.
    inline bool storeSite(const Database::Collection &collection, const StoredSite &s, const Clock::time_point now) {
        if (s.version < 0) {
            try {
                std::ignore = collection.insert_one(siteDocument(s, 0, now));
                return true;
            } catch (const std::exception &) {
                return false;// another instance created it a moment ago
            }
        }
        using bsoncxx::builder::basic::kvp;
        using bsoncxx::builder::basic::make_document;
        const auto outcome = collection.replace_one(make_document(kvp("application", s.application), kvp("site", s.site),
                                                                  kvp("version", static_cast<std::int64_t>(s.version))),
                                                    siteDocument(s, s.version + 1, now));
        return outcome.has_value() && outcome->matched_count() == 1;
    }

    // Reads one application's state for a site, lets `change` decide, writes the result - again
    // from the read if another instance wrote in between. `change` returns false to write nothing.
    //
    // @return true if the change was written (or none was needed), false if it kept colliding
    template<typename Change>
    bool updateSite(const std::string &application, const std::string &applicationId, const std::string &site, Change &&change) {
        const auto collection = egressCollection();
        for (int attempt = 0; attempt < kEgressAttempts; ++attempt) {
            auto stored = loadSite(collection, application, site)
                                  .value_or(StoredSite{.application = application, .applicationId = applicationId, .site = site});
            const auto now = Clock::now();
            if (!change(stored, now)) return true;
            if (storeSite(collection, stored, now)) return true;
            // Collided. A short, growing pause, so a crowd of instances spreads out.
            std::this_thread::sleep_for(std::chrono::milliseconds(1 + attempt * 2));
        }
        return false;
    }

}// namespace Euclid::EAP::Egress
