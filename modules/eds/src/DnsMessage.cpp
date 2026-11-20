// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// C++ includes
#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <map>
#include <sstream>
#include <utility>

// Boost includes
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/address_v6.hpp>

// Euclid includes
#include <DnsMessage.h>

namespace Euclid::EDS::Dns {

    namespace {

        constexpr std::size_t kHeaderSize = 12;
        constexpr std::size_t kMaxLabel = 63;
        constexpr std::size_t kMaxName = 255;

        // A compression pointer may only point backwards, and a well-formed name follows at most a
        // handful of them. 64 is far past any real message and short of looping for ever.
        constexpr int kMaxPointerJumps = 64;

        std::uint16_t read16(const std::span<const std::uint8_t> wire, const std::size_t pos) {
            return static_cast<std::uint16_t>((wire[pos] << 8) | wire[pos + 1]);
        }

        std::uint32_t read32(const std::span<const std::uint8_t> wire, const std::size_t pos) {
            return (static_cast<std::uint32_t>(wire[pos]) << 24) | (static_cast<std::uint32_t>(wire[pos + 1]) << 16) |
                   (static_cast<std::uint32_t>(wire[pos + 2]) << 8) | static_cast<std::uint32_t>(wire[pos + 3]);
        }

        void put16(std::vector<std::uint8_t> &out, const std::uint16_t value) {
            out.push_back(static_cast<std::uint8_t>(value >> 8));
            out.push_back(static_cast<std::uint8_t>(value & 0xFF));
        }

        void put32(std::vector<std::uint8_t> &out, const std::uint32_t value) {
            out.push_back(static_cast<std::uint8_t>(value >> 24));
            out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
            out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
            out.push_back(static_cast<std::uint8_t>(value & 0xFF));
        }

        // Reads a possibly compressed name starting at pos, and moves pos past it - past the first
        // pointer, when there is one, since that is where the name ends in this part of the message.
        bool readName(const std::span<const std::uint8_t> wire, std::size_t &pos, std::string &out) {

            out.clear();
            std::size_t p = pos;
            bool jumped = false;
            int jumps = 0;
            std::size_t wireLength = 1;

            while (true) {
                if (p >= wire.size()) return false;
                const std::uint8_t length = wire[p];

                if ((length & 0xC0) == 0xC0) {
                    if (p + 1 >= wire.size()) return false;
                    const std::size_t target = (static_cast<std::size_t>(length & 0x3F) << 8) | wire[p + 1];
                    if (!jumped) pos = p + 2;
                    jumped = true;
                    if (++jumps > kMaxPointerJumps || target >= wire.size()) return false;
                    p = target;
                    continue;
                }
                // 0x40 and 0x80 are the extended label types RFC 6891 retired; nobody sends them.
                if ((length & 0xC0) != 0) return false;

                if (length == 0) {
                    if (!jumped) pos = p + 1;
                    return true;
                }
                if (p + 1 + length > wire.size()) return false;

                const std::string_view label(reinterpret_cast<const char *>(&wire[p + 1]), length);
                // A dot inside a label cannot be told apart from a label boundary once the name is
                // text. Legal on the wire, used by nobody, and refused rather than misread.
                if (label.find('.') != std::string_view::npos) return false;

                if (!out.empty()) out += '.';
                out.append(label);

                wireLength += length + 1;
                if (wireLength > kMaxName) return false;
                p += 1 + length;
            }
        }

        bool readRecord(const std::span<const std::uint8_t> wire, std::size_t &pos, ResourceRecord &record) {
            if (!readName(wire, pos, record.name)) return false;
            if (pos + 10 > wire.size()) return false;
            record.type = read16(wire, pos);
            record.klass = read16(wire, pos + 2);
            record.ttl = read32(wire, pos + 4);
            const std::size_t length = read16(wire, pos + 8);
            pos += 10;
            if (pos + length > wire.size()) return false;
            record.rdata.assign(wire.begin() + static_cast<std::ptrdiff_t>(pos), wire.begin() + static_cast<std::ptrdiff_t>(pos + length));
            pos += length;
            return true;
        }

        std::vector<std::string> labelsOf(const std::string_view name) {
            std::vector<std::string> labels;
            if (name.empty()) return labels;
            std::size_t start = 0;
            while (true) {
                const auto dot = name.find('.', start);
                labels.emplace_back(name.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start));
                if (dot == std::string_view::npos) break;
                start = dot + 1;
            }
            return labels;
        }

        // Writes messages with owner names compressed against the names already written.
        class Writer {

        public:
            void name(const std::string_view name) {

                const auto labels = labelsOf(name);
                for (std::size_t i = 0; i < labels.size(); ++i) {

                    std::string suffix;
                    for (std::size_t j = i; j < labels.size(); ++j) {
                        if (!suffix.empty()) suffix += '.';
                        suffix += labels[j];
                    }

                    // Matched as written, not case-folded: a pointer to the question's spelling
                    // would change the case of an owner name the client compares byte for byte.
                    if (const auto it = _offsets.find(suffix); it != _offsets.end()) {
                        put16(_out, static_cast<std::uint16_t>(0xC000 | it->second));
                        return;
                    }
                    if (_out.size() < 0x4000) _offsets.emplace(suffix, static_cast<std::uint16_t>(_out.size()));

                    _out.push_back(static_cast<std::uint8_t>(labels[i].size()));
                    _out.insert(_out.end(), labels[i].begin(), labels[i].end());
                }
                _out.push_back(0);
            }

            void record(const ResourceRecord &record) {
                name(record.name);
                put16(_out, record.type);
                put16(_out, record.klass);
                put32(_out, record.ttl);
                put16(_out, static_cast<std::uint16_t>(record.rdata.size()));
                _out.insert(_out.end(), record.rdata.begin(), record.rdata.end());
            }

            std::vector<std::uint8_t> &bytes() { return _out; }

        private:
            std::vector<std::uint8_t> _out;
            std::map<std::string, std::uint16_t> _offsets;
        };

        std::vector<std::uint8_t> write(const Message &message, const bool sections, const bool truncated) {

            Writer writer;
            auto &out = writer.bytes();

            put16(out, message.id);
            std::uint16_t flags = 0;
            if (message.response) flags |= 0x8000;
            flags |= static_cast<std::uint16_t>((message.opcode & 0x0F) << 11);
            if (message.authoritative) flags |= 0x0400;
            if (truncated) flags |= 0x0200;
            if (message.recursionDesired) flags |= 0x0100;
            if (message.recursionAvailable) flags |= 0x0080;
            flags |= static_cast<std::uint16_t>(message.rcode & 0x0F);
            put16(out, flags);

            const bool edns = message.ednsUdpSize.has_value();
            put16(out, static_cast<std::uint16_t>(message.questions.size()));
            put16(out, static_cast<std::uint16_t>(sections ? message.answers.size() : 0));
            put16(out, static_cast<std::uint16_t>(sections ? message.authority.size() : 0));
            put16(out, static_cast<std::uint16_t>((sections ? message.additional.size() : 0) + (edns ? 1 : 0)));

            for (const auto &question: message.questions) {
                writer.name(question.name);
                put16(out, question.type);
                put16(out, question.klass);
            }
            if (sections) {
                for (const auto &record: message.answers) writer.record(record);
                for (const auto &record: message.authority) writer.record(record);
                for (const auto &record: message.additional) writer.record(record);
            }
            if (edns) {
                // Root owner, our payload size in the class field, extended rcode/version/flags 0.
                out.push_back(0);
                put16(out, Type::OPT);
                put16(out, static_cast<std::uint16_t>(EdnsUdpLimit));
                put32(out, 0);
                put16(out, 0);
            }
            return std::move(out);
        }

        std::string trim(const std::string_view text) {
            const auto first = text.find_first_not_of(" \t\r\n");
            if (first == std::string_view::npos) return {};
            const auto last = text.find_last_not_of(" \t\r\n");
            return std::string(text.substr(first, last - first + 1));
        }

        template<class T>
        bool parseNumber(const std::string &text, T &value, const T max) {
            unsigned long long parsed = 0;
            const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), parsed);
            if (ec != std::errc{} || ptr != text.data() + text.size() || parsed > max) return false;
            value = static_cast<T>(parsed);
            return true;
        }

        // A domain name inside record data: validated and written whole. "." is the root, which
        // is what a null MX ("0 .") names.
        bool encodeTarget(const std::string &text, std::vector<std::uint8_t> &out, std::string &error) {
            const auto name = Normalize(trim(text));
            if (name.empty() && trim(text) != ".") {
                error = "a domain name is required";
                return false;
            }
            if (!IsValidName(name)) {
                error = "not a valid domain name: " + text;
                return false;
            }
            AppendName(out, name);
            return true;
        }

        constexpr std::array<std::pair<std::string_view, std::uint16_t>, 11> kTypes{{
                {"A", Type::A},
                {"NS", Type::NS},
                {"CNAME", Type::CNAME},
                {"SOA", Type::SOA},
                {"PTR", Type::PTR},
                {"MX", Type::MX},
                {"TXT", Type::TXT},
                {"AAAA", Type::AAAA},
                {"SRV", Type::SRV},
                {"OPT", Type::OPT},
                {"ANY", Type::ANY},
        }};

    }// namespace

    std::optional<Message> Parse(const std::span<const std::uint8_t> wire) {

        if (wire.size() < kHeaderSize) return std::nullopt;

        Message message;
        message.id = read16(wire, 0);
        const auto flags = read16(wire, 2);
        message.response = (flags & 0x8000) != 0;
        message.opcode = static_cast<std::uint8_t>((flags >> 11) & 0x0F);
        message.authoritative = (flags & 0x0400) != 0;
        message.truncated = (flags & 0x0200) != 0;
        message.recursionDesired = (flags & 0x0100) != 0;
        message.recursionAvailable = (flags & 0x0080) != 0;
        message.rcode = static_cast<std::uint8_t>(flags & 0x0F);

        const auto questions = read16(wire, 4);
        const auto answers = read16(wire, 6);
        const auto authority = read16(wire, 8);
        const auto additional = read16(wire, 10);

        std::size_t pos = kHeaderSize;
        for (std::uint16_t i = 0; i < questions; ++i) {
            Question question;
            if (!readName(wire, pos, question.name)) return std::nullopt;
            if (pos + 4 > wire.size()) return std::nullopt;
            question.type = read16(wire, pos);
            question.klass = read16(wire, pos + 2);
            pos += 4;
            message.questions.push_back(std::move(question));
        }

        const auto readSection = [&](const std::uint16_t count, std::vector<ResourceRecord> &section) {
            for (std::uint16_t i = 0; i < count; ++i) {
                ResourceRecord record;
                if (!readRecord(wire, pos, record)) return false;
                if (record.type == Type::OPT) {
                    // RFC 6891 §6.2.5: a size below 512 is treated as 512.
                    message.ednsUdpSize = std::max<std::uint16_t>(static_cast<std::uint16_t>(ClassicUdpLimit), record.klass);
                    continue;
                }
                section.push_back(std::move(record));
            }
            return true;
        };
        if (!readSection(answers, message.answers)) return std::nullopt;
        if (!readSection(authority, message.authority)) return std::nullopt;
        if (!readSection(additional, message.additional)) return std::nullopt;

        return message;
    }

    std::vector<std::uint8_t> Serialize(const Message &message, const std::size_t limit) {
        auto full = write(message, true, message.truncated);
        if (full.size() <= limit) return full;
        return write(message, false, true);
    }

    std::string Normalize(const std::string_view name) {
        std::string normalized(name);
        if (!normalized.empty() && normalized.back() == '.') normalized.pop_back();
        std::ranges::transform(normalized, normalized.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return normalized;
    }

    bool IsValidName(const std::string_view name) {
        if (name.empty()) return true;
        std::size_t wireLength = 1;
        for (const auto &label: labelsOf(name)) {
            if (label.empty() || label.size() > kMaxLabel) return false;
            if (std::ranges::any_of(label, [](const unsigned char c) { return std::isspace(c) != 0 || c < 0x20; })) return false;
            wireLength += label.size() + 1;
        }
        return wireLength <= kMaxName;
    }

    bool IsInZone(const std::string_view name, const std::string_view zone) {
        if (zone.empty()) return true;
        if (name == zone) return true;
        return name.size() > zone.size() && name.ends_with(zone) && name[name.size() - zone.size() - 1] == '.';
    }

    void AppendName(std::vector<std::uint8_t> &out, const std::string_view name) {
        for (const auto &label: labelsOf(name)) {
            out.push_back(static_cast<std::uint8_t>(label.size()));
            out.insert(out.end(), label.begin(), label.end());
        }
        out.push_back(0);
    }

    std::optional<std::uint16_t> TypeFromString(const std::string_view name) {

        std::string upper(name);
        std::ranges::transform(upper, upper.begin(), [](const unsigned char c) { return static_cast<char>(std::toupper(c)); });

        for (const auto &[mnemonic, number]: kTypes) {
            if (upper == mnemonic) return number;
        }
        // RFC 3597's spelling for a type without a mnemonic.
        if (upper.starts_with("TYPE") && upper.size() > 4) {
            std::uint16_t number = 0;
            if (parseNumber<std::uint16_t>(upper.substr(4), number, 65535)) return number;
        }
        return std::nullopt;
    }

    std::string TypeToString(const std::uint16_t type) {
        for (const auto &[mnemonic, number]: kTypes) {
            if (type == number) return std::string(mnemonic);
        }
        return "TYPE" + std::to_string(type);
    }

    bool IsStorableType(const std::uint16_t type) {
        switch (type) {
            case Type::A:
            case Type::AAAA:
            case Type::CNAME:
            case Type::NS:
            case Type::PTR:
            case Type::MX:
            case Type::TXT:
            case Type::SRV:
                return true;
            default:
                return false;
        }
    }

    std::optional<std::vector<std::uint8_t> > EncodeRdata(const std::uint16_t type, const std::string &value, std::string &error) {

        std::vector<std::uint8_t> out;
        boost::system::error_code ec;

        switch (type) {

            case Type::A: {
                const auto address = boost::asio::ip::make_address_v4(trim(value), ec);
                if (ec) {
                    error = "not an IPv4 address: " + value;
                    return std::nullopt;
                }
                const auto bytes = address.to_bytes();
                out.assign(bytes.begin(), bytes.end());
                return out;
            }

            case Type::AAAA: {
                const auto address = boost::asio::ip::make_address_v6(trim(value), ec);
                if (ec) {
                    error = "not an IPv6 address: " + value;
                    return std::nullopt;
                }
                const auto bytes = address.to_bytes();
                out.assign(bytes.begin(), bytes.end());
                return out;
            }

            case Type::CNAME:
            case Type::NS:
            case Type::PTR:
                if (!encodeTarget(value, out, error)) return std::nullopt;
                return out;

            case Type::MX: {
                std::istringstream in(value);
                std::string preference, exchange, rest;
                in >> preference >> exchange >> rest;
                std::uint16_t number = 0;
                if (exchange.empty() || !rest.empty() || !parseNumber<std::uint16_t>(preference, number, 65535)) {
                    error = "an MX value is \"<preference> <host>\", e.g. \"10 mail.example.com\"";
                    return std::nullopt;
                }
                put16(out, number);
                if (!encodeTarget(exchange, out, error)) return std::nullopt;
                return out;
            }

            case Type::SRV: {
                std::istringstream in(value);
                std::string priority, weight, port, target, rest;
                in >> priority >> weight >> port >> target >> rest;
                std::uint16_t p = 0, w = 0, n = 0;
                if (target.empty() || !rest.empty() || !parseNumber<std::uint16_t>(priority, p, 65535) ||
                    !parseNumber<std::uint16_t>(weight, w, 65535) || !parseNumber<std::uint16_t>(port, n, 65535)) {
                    error = "an SRV value is \"<priority> <weight> <port> <host>\", e.g. \"10 5 8080 api.example.com\"";
                    return std::nullopt;
                }
                put16(out, p);
                put16(out, w);
                put16(out, n);
                if (!encodeTarget(target, out, error)) return std::nullopt;
                return out;
            }

            case Type::TXT: {
                // One character-string holds 255 bytes; a longer value is several, which a reader
                // joins back together (RFC 7208 §3.3 does exactly that for SPF).
                if (value.size() > 4000) {
                    error = "a TXT value may be at most 4000 bytes";
                    return std::nullopt;
                }
                if (value.empty()) {
                    out.push_back(0);
                    return out;
                }
                for (std::size_t start = 0; start < value.size(); start += 255) {
                    const auto chunk = std::string_view(value).substr(start, 255);
                    out.push_back(static_cast<std::uint8_t>(chunk.size()));
                    out.insert(out.end(), chunk.begin(), chunk.end());
                }
                return out;
            }

            default:
                error = "records of type " + TypeToString(type) + " cannot be stored";
                return std::nullopt;
        }
    }

    std::string RdataToString(const std::uint16_t type, const std::span<const std::uint8_t> rdata) {

        const auto opaque = [&] {
            static constexpr char kHex[] = "0123456789abcdef";
            std::string text = "\\# " + std::to_string(rdata.size()) + " ";
            for (const auto byte: rdata) {
                text += kHex[byte >> 4];
                text += kHex[byte & 0x0F];
            }
            return text;
        };

        std::size_t pos = 0;
        std::string name;
        const auto readTarget = [&](std::string &out) { return readName(rdata, pos, out); };

        switch (type) {
            case Type::A:
                if (rdata.size() != 4) return opaque();
                return boost::asio::ip::address_v4({rdata[0], rdata[1], rdata[2], rdata[3]}).to_string();

            case Type::AAAA: {
                if (rdata.size() != 16) return opaque();
                boost::asio::ip::address_v6::bytes_type bytes{};
                std::ranges::copy(rdata, bytes.begin());
                return boost::asio::ip::address_v6(bytes).to_string();
            }

            case Type::CNAME:
            case Type::NS:
            case Type::PTR:
                if (!readTarget(name)) return opaque();
                return name + ".";

            case Type::MX:
                if (rdata.size() < 3) return opaque();
                pos = 2;
                if (!readTarget(name)) return opaque();
                return std::to_string(read16(rdata, 0)) + " " + name + ".";

            case Type::SRV:
                if (rdata.size() < 7) return opaque();
                pos = 6;
                if (!readTarget(name)) return opaque();
                return std::to_string(read16(rdata, 0)) + " " + std::to_string(read16(rdata, 2)) + " " +
                       std::to_string(read16(rdata, 4)) + " " + name + ".";

            case Type::TXT: {
                std::string text;
                while (pos < rdata.size()) {
                    const std::size_t length = rdata[pos];
                    if (pos + 1 + length > rdata.size()) return opaque();
                    text.append(reinterpret_cast<const char *>(&rdata[pos + 1]), length);
                    pos += 1 + length;
                }
                return text;
            }

            case Type::SOA: {
                std::string mname, rname;
                if (!readTarget(mname) || !readTarget(rname) || pos + 20 != rdata.size()) return opaque();
                std::string text = mname + ". " + rname + ".";
                for (int i = 0; i < 5; ++i) text += " " + std::to_string(read32(rdata, pos + static_cast<std::size_t>(i) * 4));
                return text;
            }

            default:
                return opaque();
        }
    }

    std::vector<std::uint8_t> EncodeSoa(const std::string_view primaryNs, const std::string_view hostmaster, const std::uint32_t serial,
                                        const std::uint32_t refresh, const std::uint32_t retry, const std::uint32_t expire,
                                        const std::uint32_t minimum) {
        std::vector<std::uint8_t> out;
        AppendName(out, primaryNs);
        AppendName(out, hostmaster);
        put32(out, serial);
        put32(out, refresh);
        put32(out, retry);
        put32(out, expire);
        put32(out, minimum);
        return out;
    }

    Message ErrorResponse(const Message &request, const std::uint8_t rcode) {
        Message response;
        response.id = request.id;
        response.response = true;
        response.opcode = request.opcode;
        response.recursionDesired = request.recursionDesired;
        response.rcode = rcode;
        response.questions = request.questions;
        if (request.ednsUdpSize.has_value()) response.ednsUdpSize = EdnsUdpLimit;
        return response;
    }

}// namespace Euclid::EDS::Dns
