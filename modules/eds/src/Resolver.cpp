// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// C++ includes
#include <algorithm>
#include <ranges>

// Euclid includes
#include <Resolver.h>

namespace Euclid::EDS {

    namespace {

        // How many CNAMEs one lookup follows. Enough for any real chain, short of a loop.
        constexpr int kMaxChain = 8;

        // SOA timers. Nothing transfers these zones, so only minimum - the negative-caching TTL -
        // is read by anybody; the others are the RFC 1912 suggestions, there to be well-formed.
        constexpr std::uint32_t kRefresh = 3600;
        constexpr std::uint32_t kRetry = 600;
        constexpr std::uint32_t kExpire = 604800;

        std::string parentOf(const std::string &name) {
            const auto dot = name.find('.');
            return dot == std::string::npos ? std::string{} : name.substr(dot + 1);
        }

        void append(std::vector<Dns::ResourceRecord> &section, const std::string &owner, const RecordSet &set) {
            for (const auto &rdata: set.rdata) {
                section.push_back(Dns::ResourceRecord{.name = owner, .type = set.type, .klass = Dns::ClassIn, .ttl = set.ttl, .rdata = rdata});
            }
        }

        // The SOA a negative answer carries, with its TTL capped at the zone's negative TTL
        // (RFC 2308 §5) - which is how long a resolver caches "there is nothing here".
        void appendSoa(const Zone &zone, Answer &answer) {
            const auto apex = zone.names.find(zone.name);
            if (apex == zone.names.end()) return;
            const auto soa = apex->second.find(Dns::Type::SOA);
            if (soa == apex->second.end()) return;
            append(answer.authority, zone.name, soa->second);
        }

    }// namespace

    void Zone::Add(const std::string &owner, const std::uint16_t type, const std::uint32_t ttl, std::vector<std::uint8_t> rdata,
                   const std::string &target) {

        auto &set = names[owner][type];
        if (set.rdata.empty()) {
            set.type = type;
            set.ttl = ttl;
        } else {
            set.ttl = std::min(set.ttl, ttl);
        }
        // A duplicate value is one record, not two (RFC 2181 §5).
        if (std::ranges::find(set.rdata, rdata) == set.rdata.end()) set.rdata.push_back(std::move(rdata));
        if (!target.empty()) set.target = target;
    }

    void Zone::Finish() {

        auto &apex = names[name];
        if (!apex.contains(Dns::Type::NS)) {
            std::vector<std::uint8_t> ns;
            Dns::AppendName(ns, primaryNs);
            apex[Dns::Type::NS] = RecordSet{.type = Dns::Type::NS, .ttl = ttl, .rdata = {ns}, .target = {}};
        }
        const auto rname = hostmaster.empty() ? "hostmaster." + name : hostmaster;
        apex[Dns::Type::SOA] = RecordSet{.type = Dns::Type::SOA, .ttl = ttl,
                                         .rdata = {Dns::EncodeSoa(primaryNs, rname, serial, kRefresh, kRetry, kExpire, ttl)},
                                         .target = {}};

        // Every owner, and every name between it and the apex: "a.b.example.com" makes
        // "b.example.com" exist, records or not.
        _existing.clear();
        for (const auto &owner: names | std::views::keys) {
            for (auto current = owner; Dns::IsInZone(current, name); current = parentOf(current)) {
                if (!_existing.insert(current).second) break;
                if (current == name) break;
            }
        }
    }

    void ZoneSet::Add(Zone zone) {
        zone.Finish();
        _zones.push_back(std::move(zone));
        // Longest apex first, so Find() stops at the most specific zone.
        std::ranges::stable_sort(_zones, [](const Zone &a, const Zone &b) { return a.name.size() > b.name.size(); });
    }

    const Zone *ZoneSet::Find(const std::string &name) const {
        for (const auto &zone: _zones) {
            if (Dns::IsInZone(name, zone.name)) return &zone;
        }
        return nullptr;
    }

    std::optional<Answer> Resolve(const ZoneSet &zones, const std::string &name, const std::uint16_t type) {

        auto current = Dns::Normalize(name);
        if (zones.Find(current) == nullptr) return std::nullopt;

        // The first owner is spelled the way it was asked (RFC 4343 §4.1); the names a CNAME leads
        // to are spelled the way they were stored.
        auto owner = name;
        if (!owner.empty() && owner.back() == '.') owner.pop_back();

        Answer answer;
        for (int link = 0; link <= kMaxChain; ++link) {

            const auto *zone = zones.Find(current);
            // The chain left the zones here. What was found so far is the answer, and the client's
            // resolver follows the last CNAME itself.
            if (zone == nullptr) return answer;

            const std::map<std::uint16_t, RecordSet> *sets = nullptr;
            if (const auto it = zone->names.find(current); it != zone->names.end()) {
                sets = &it->second;
            } else if (zone->Exists(current)) {
                appendSoa(*zone, answer);
                return answer;
            } else {
                // The closest encloser: the nearest ancestor that exists. Only a wildcard directly
                // below it may answer (RFC 4592 §3.3.1); one further up is shadowed by it.
                auto encloser = parentOf(current);
                while (!zone->Exists(encloser)) encloser = parentOf(encloser);

                if (const auto wildcard = zone->names.find("*." + encloser); wildcard != zone->names.end()) {
                    sets = &wildcard->second;
                } else {
                    // RFC 6604 §3: after a CNAME the rcode is the last name's, so a chain ending
                    // nowhere is NXDOMAIN with the CNAMEs that led there still in the answer.
                    answer.rcode = Dns::Rcode::NxDomain;
                    appendSoa(*zone, answer);
                    return answer;
                }
            }

            if (type == Dns::Type::ANY) {
                for (const auto &set: *sets | std::views::values) append(answer.answers, owner, set);
                return answer;
            }
            if (const auto match = sets->find(type); match != sets->end()) {
                append(answer.answers, owner, match->second);
                return answer;
            }
            if (const auto alias = sets->find(Dns::Type::CNAME); alias != sets->end()) {
                append(answer.answers, owner, alias->second);
                current = alias->second.target;
                owner = current;
                continue;
            }

            appendSoa(*zone, answer);
            return answer;
        }

        // A chain longer than kMaxChain is a loop or as good as one.
        answer.rcode = Dns::Rcode::ServFail;
        return answer;
    }

}// namespace Euclid::EDS
