// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

// Euclid includes
#include <DnsMessage.h>

namespace Euclid::EDS {

    /**
     * @brief Every record of one type at one name, in wire form.
     */
    struct RecordSet {
        std::uint16_t type{};
        std::uint32_t ttl{};
        std::vector<std::vector<std::uint8_t> > rdata;

        /**
         * @brief For a CNAME, where it points, normalized - what a lookup follows. Empty otherwise.
         */
        std::string target;
    };

    /**
     * @brief One zone EDS answers for, as the resolver reads it.
     *
     * @par
     * Built once per refresh from what is stored, and never changed after Finish(): a lookup reads
     * a zone from several threads at once with no lock, which is only safe because nothing writes.
     */
    struct Zone {

        /**
         * @brief The apex, normalized: "example.com".
         */
        std::string name;

        /**
         * @brief TTL for records that name none, and the negative-caching TTL.
         */
        std::uint32_t ttl{300};

        /**
         * @brief The SOA serial. Seconds since the epoch of the zone's last change, so that it
         * moves forward whenever the zone does - which is all a serial is for.
         */
        std::uint32_t serial{1};

        /**
         * @brief SOA MNAME, and the NS record the apex gets when it was given none.
         */
        std::string primaryNs{"localhost"};

        /**
         * @brief SOA RNAME, in its domain form: "hostmaster.example.com".
         */
        std::string hostmaster;

        /**
         * @brief Owner name (normalized) -> type -> records.
         */
        std::map<std::string, std::map<std::uint16_t, RecordSet> > names;

        /**
         * @brief Adds one value. A second value of the same type at the same name joins the set;
         * its TTL is the lowest of theirs, since a set has one TTL (RFC 2181 §5.2).
         */
        void Add(const std::string &owner, std::uint16_t type, std::uint32_t recordTtl, std::vector<std::uint8_t> rdata,
                 const std::string &target = {});

        /**
         * @brief Completes the zone: synthesizes the apex SOA, and an NS when none was stored,
         * and records which names exist. Called once, after the last Add().
         */
        void Finish();

        /**
         * @brief Whether a name exists in the zone - has records, or has a name with records
         * below it (an "empty non-terminal", RFC 8020: answered NODATA, not NXDOMAIN).
         */
        [[nodiscard]] bool Exists(const std::string &owner) const { return _existing.contains(owner); }

    private:
        std::set<std::string> _existing;
    };

    /**
     * @brief Every zone EDS is authoritative for.
     */
    class ZoneSet {

    public:
        /**
         * @brief Adds a zone; Finish() is called on it here.
         */
        void Add(Zone zone);

        /**
         * @brief The zone a name belongs to: the one with the longest apex the name is in, so a
         * delegated "dev.example.com" wins over "example.com" for names below it.
         *
         * @param name normalized.
         */
        [[nodiscard]] const Zone *Find(const std::string &name) const;

        [[nodiscard]] const std::vector<Zone> &Zones() const { return _zones; }

    private:
        std::vector<Zone> _zones;
    };

    /**
     * @brief The answer to one question from the stored zones.
     */
    struct Answer {
        std::uint8_t rcode{Dns::Rcode::NoError};
        std::vector<Dns::ResourceRecord> answers;
        std::vector<Dns::ResourceRecord> authority;
    };

    /**
     * @brief Answers a question from the zones.
     *
     * @par
     * - A name with records of the type asked for is answered with them.
     * - A name with a CNAME is answered with it, and the lookup continues at its target for as
     *   long as the target is in a zone here - up to eight links, which stops a loop.
     * - A name that exists without that type is NODATA: NOERROR, no answer, SOA in authority.
     * - A name that does not exist is matched against a wildcard at its closest encloser
     *   (RFC 4592), and is NXDOMAIN with SOA in authority when there is none.
     *
     * @param zones the zones.
     * @param name the name as asked; answers carry this spelling as their owner.
     * @param type the type asked for; ANY answers every set at the name.
     * @return the answer, or std::nullopt when the name is in none of the zones - which the caller
     * forwards or refuses.
     */
    std::optional<Answer> Resolve(const ZoneSet &zones, const std::string &name, std::uint16_t type);

}// namespace Euclid::EDS
