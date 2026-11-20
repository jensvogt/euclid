// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Euclid::EDS::Dns {

    /**
     * @brief The record types EDS knows by name.
     *
     * @par
     * A type number not in this list is still carried through a query and answered with "no such
     * data" - DNS says an unknown type is a question like any other - it just cannot be stored.
     */
    namespace Type {
        constexpr std::uint16_t A = 1;
        constexpr std::uint16_t NS = 2;
        constexpr std::uint16_t CNAME = 5;
        constexpr std::uint16_t SOA = 6;
        constexpr std::uint16_t PTR = 12;
        constexpr std::uint16_t MX = 15;
        constexpr std::uint16_t TXT = 16;
        constexpr std::uint16_t AAAA = 28;
        constexpr std::uint16_t SRV = 33;
        constexpr std::uint16_t OPT = 41;
        constexpr std::uint16_t ANY = 255;
    }// namespace Type

    /**
     * @brief Response codes (RFC 1035 §4.1.1).
     */
    namespace Rcode {
        constexpr std::uint8_t NoError = 0;
        constexpr std::uint8_t FormErr = 1;
        constexpr std::uint8_t ServFail = 2;
        constexpr std::uint8_t NxDomain = 3;
        constexpr std::uint8_t NotImp = 4;
        constexpr std::uint8_t Refused = 5;
    }// namespace Rcode

    /**
     * @brief Class IN, the only one served.
     */
    constexpr std::uint16_t ClassIn = 1;

    /**
     * @brief What a client without EDNS0 may be sent over UDP (RFC 1035 §4.2.1).
     */
    constexpr std::size_t ClassicUdpLimit = 512;

    /**
     * @brief The UDP payload size EDS advertises and honours at most.
     *
     * @par
     * 1232 rather than the 4096 EDNS0 allows: the DNS flag day 2020 figure, the largest payload
     * that survives a 1280-byte IPv6 path without fragmenting. A fragmented UDP answer is one that
     * a firewall somewhere drops, and the client then waits for a timeout instead of retrying over
     * TCP, which a truncated answer would have told it to do straight away.
     */
    constexpr std::size_t EdnsUdpLimit = 1232;

    /**
     * @brief The question section's one entry.
     */
    struct Question {
        /**
         * @brief The name as the client spelled it, without the trailing dot. Echoed back exactly,
         * case included (RFC 4343) - matching is done on Normalize() of it.
         */
        std::string name;
        std::uint16_t type{};
        std::uint16_t klass{ClassIn};
    };

    /**
     * @brief One resource record, with its data already in wire form.
     */
    struct ResourceRecord {
        std::string name;
        std::uint16_t type{};
        std::uint16_t klass{ClassIn};
        std::uint32_t ttl{};
        std::vector<std::uint8_t> rdata;
    };

    /**
     * @brief A DNS message, as much of one as EDS reads and writes.
     */
    struct Message {
        std::uint16_t id{};
        bool response{};
        std::uint8_t opcode{};
        bool authoritative{};
        bool truncated{};
        bool recursionDesired{};
        bool recursionAvailable{};
        std::uint8_t rcode{};
        std::vector<Question> questions;
        std::vector<ResourceRecord> answers;
        std::vector<ResourceRecord> authority;
        std::vector<ResourceRecord> additional;

        /**
         * @brief The UDP payload size from the requester's OPT record, if it sent one.
         *
         * @par
         * On a response this says whether to write an OPT record back: one is written exactly when
         * this is set, which is the rule RFC 6891 §7 states - a responder does not send EDNS to a
         * requester that did not.
         */
        std::optional<std::uint16_t> ednsUdpSize;
    };

    /**
     * @brief Reads a message off the wire.
     *
     * @par
     * Header, questions, and the records after them. Records are parsed rather than skipped,
     * because the OPT record sits at the end of the additional section and is the only way to
     * learn the client's payload size. Their data is kept as raw bytes; names inside it are not
     * decompressed, since nothing here needs to read them.
     *
     * @return the message, or std::nullopt when it is not a well-formed DNS message - a label
     * longer than 63, a name longer than 255, a compression loop, or a section that runs past the
     * end.
     */
    std::optional<Message> Parse(std::span<const std::uint8_t> wire);

    /**
     * @brief Writes a message, truncating it to fit.
     *
     * @par
     * Owner names are compressed against each other and the question. Names inside record data
     * are written whole: compressing them is allowed only for the types RFC 1035 lists, and the
     * data was encoded once, when the record was stored, without knowing what message it would be
     * part of.
     *
     * @param message what to write.
     * @param limit the most bytes the result may have. When the whole message does not fit, the
     * answer, authority and additional sections are dropped and TC is set - RFC 2181 §9's rule
     * that a truncated response is one the client must re-ask over TCP, not a partial answer to
     * use. The OPT record is kept.
     */
    std::vector<std::uint8_t> Serialize(const Message &message, std::size_t limit = 65535);

    /**
     * @brief Lowercases a name and drops a trailing dot - the form every comparison is made in.
     */
    std::string Normalize(std::string_view name);

    /**
     * @brief Whether a name can be written on the wire: labels of 1..63 bytes, 255 bytes in all.
     *
     * @par
     * Any byte is allowed in a label - DNS itself does not restrict them, and "_http._tcp" is a
     * legitimate owner - except a dot, which would split it, and whitespace, which is always a
     * typo. A leading "*." label is a wildcard and passes like any other.
     */
    bool IsValidName(std::string_view name);

    /**
     * @brief Whether name is zone itself or a name below it. Both already normalized.
     */
    bool IsInZone(std::string_view name, std::string_view zone);

    /**
     * @brief Writes a name in wire form, uncompressed. The empty string is the root.
     */
    void AppendName(std::vector<std::uint8_t> &out, std::string_view name);

    /**
     * @brief The type number for a mnemonic, "A", "aaaa", "SRV"... or a "TYPE<n>" spelling.
     */
    std::optional<std::uint16_t> TypeFromString(std::string_view name);

    /**
     * @brief The mnemonic for a type number, or "TYPE<n>" for one without one.
     */
    std::string TypeToString(std::uint16_t type);

    /**
     * @brief Whether records of this type may be stored through the API.
     *
     * @par
     * SOA is not: a zone's SOA is made from the zone, so it cannot disagree with it. OPT and ANY
     * are not record types at all but parts of the protocol.
     */
    bool IsStorableType(std::uint16_t type);

    /**
     * @brief Encodes one value of a record in wire form, from the text an operator writes.
     *
     * @par
     * The presentation formats are the zone-file ones, without the quoting:
     * - A: "192.0.2.1"
     * - AAAA: "2001:db8::1"
     * - CNAME, NS, PTR: "target.example.com"
     * - MX: "10 mail.example.com"
     * - SRV: "10 5 8080 host.example.com" (priority, weight, port, target)
     * - TXT: any text; split into 255-byte strings when longer
     *
     * @param type the record type.
     * @param value the value in presentation form.
     * @param error filled in with why, when the value cannot be encoded.
     * @return the record data, or std::nullopt.
     */
    std::optional<std::vector<std::uint8_t> > EncodeRdata(std::uint16_t type, const std::string &value, std::string &error);

    /**
     * @brief Record data back in presentation form - the inverse of EncodeRdata(), and SOA as
     * "<mname> <rname> <serial> <refresh> <retry> <expire> <minimum>".
     *
     * @par
     * For data EDS encoded itself, so names in it are never compressed. Anything it cannot read is
     * shown as RFC 3597's "\# <length> <hex>".
     */
    std::string RdataToString(std::uint16_t type, std::span<const std::uint8_t> rdata);

    /**
     * @brief The SOA record data for a zone (RFC 1035 §3.3.13).
     */
    std::vector<std::uint8_t> EncodeSoa(std::string_view primaryNs, std::string_view hostmaster, std::uint32_t serial,
                                        std::uint32_t refresh, std::uint32_t retry, std::uint32_t expire, std::uint32_t minimum);

    /**
     * @brief A response to a request that carries nothing but an rcode: the request's ID,
     * question and RD flag, with QR set.
     */
    Message ErrorResponse(const Message &request, std::uint8_t rcode);

}// namespace Euclid::EDS::Dns
