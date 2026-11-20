// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE EdsDnsTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

// Boost includes
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

// Euclid includes
#include <Catalog.h>
#include <DnsListener.h>
#include <DnsMessage.h>
#include <Resolver.h>

using namespace Euclid::EDS;
namespace asio = boost::asio;

// The DNS side of EDS: the wire codec, the resolver's answers, how the zones are built from what is
// stored and what runs, and the listener's handling of a request - none of which needs a database.

namespace {

    std::vector<std::uint8_t> query(const std::string &name, const std::uint16_t type, const std::uint16_t id = 0x1234,
                                    const std::optional<std::uint16_t> edns = std::nullopt) {
        Dns::Message message;
        message.id = id;
        message.recursionDesired = true;
        message.questions.push_back({.name = name, .type = type, .klass = Dns::ClassIn});
        message.ednsUdpSize = edns;
        return Dns::Serialize(message);
    }

    std::vector<std::uint8_t> encode(const std::uint16_t type, const std::string &value) {
        std::string error;
        auto rdata = Dns::EncodeRdata(type, value, error);
        BOOST_TEST_REQUIRE(rdata.has_value(), error);
        return *rdata;
    }

    // example.com, with the shapes the resolver has to tell apart.
    ZoneSet exampleZones() {
        Zone zone;
        zone.name = "example.com";
        zone.ttl = 60;
        zone.serial = 42;
        zone.Add("www.example.com", Dns::Type::A, 300, encode(Dns::Type::A, "192.0.2.10"));
        zone.Add("www.example.com", Dns::Type::A, 120, encode(Dns::Type::A, "192.0.2.11"));
        zone.Add("alias.example.com", Dns::Type::CNAME, 300, encode(Dns::Type::CNAME, "www.example.com"), "www.example.com");
        zone.Add("outside.example.com", Dns::Type::CNAME, 300, encode(Dns::Type::CNAME, "www.example.org"), "www.example.org");
        zone.Add("loop1.example.com", Dns::Type::CNAME, 300, encode(Dns::Type::CNAME, "loop2.example.com"), "loop2.example.com");
        zone.Add("loop2.example.com", Dns::Type::CNAME, 300, encode(Dns::Type::CNAME, "loop1.example.com"), "loop1.example.com");
        zone.Add("*.dev.example.com", Dns::Type::A, 300, encode(Dns::Type::A, "192.0.2.99"));
        zone.Add("a.b.example.com", Dns::Type::TXT, 300, encode(Dns::Type::TXT, "deep"));
        zone.Add("example.com", Dns::Type::MX, 300, encode(Dns::Type::MX, "10 mail.example.com"));

        Zone sub;
        sub.name = "lab.example.com";
        sub.Add("host.lab.example.com", Dns::Type::A, 300, encode(Dns::Type::A, "198.51.100.1"));

        ZoneSet set;
        set.Add(std::move(zone));
        set.Add(std::move(sub));
        return set;
    }

    unsigned short freePort() {
        asio::io_context context;
        asio::ip::udp::socket probe(context, asio::ip::udp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
        return probe.local_endpoint().port();
    }

}// namespace

// ── Codec ────────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(QueryRoundTrips) {
    const auto wire = query("www.Example.com", Dns::Type::AAAA, 0xBEEF, 4096);
    const auto parsed = Dns::Parse(wire);
    BOOST_TEST_REQUIRE(parsed.has_value());
    BOOST_TEST(parsed->id == 0xBEEF);
    BOOST_TEST(parsed->recursionDesired);
    BOOST_TEST(!parsed->response);
    BOOST_TEST_REQUIRE(parsed->questions.size() == 1u);
    BOOST_TEST(parsed->questions[0].name == "www.Example.com");
    BOOST_TEST(parsed->questions[0].type == Dns::Type::AAAA);
    BOOST_TEST_REQUIRE(parsed->ednsUdpSize.has_value());
    // What we write in our own OPT record is our limit, not the one the message was built with.
    BOOST_TEST(*parsed->ednsUdpSize == Dns::EdnsUdpLimit);
}

BOOST_AUTO_TEST_CASE(MalformedMessagesAreRefused) {
    // Shorter than a header.
    BOOST_TEST(!Dns::Parse(std::vector<std::uint8_t>{0, 1, 2}).has_value());

    // A question whose name runs past the end.
    std::vector<std::uint8_t> truncated{0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 5, 'a', 'b'};
    BOOST_TEST(!Dns::Parse(truncated).has_value());

    // A compression pointer that points at itself.
    std::vector<std::uint8_t> loop{0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0xC0, 12, 0, 1, 0, 1};
    BOOST_TEST(!Dns::Parse(loop).has_value());

    // A 64-byte label.
    std::vector<std::uint8_t> longLabel{0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 64};
    longLabel.insert(longLabel.end(), 64, 'x');
    longLabel.insert(longLabel.end(), {0, 0, 1, 0, 1});
    BOOST_TEST(!Dns::Parse(longLabel).has_value());
}

BOOST_AUTO_TEST_CASE(OwnerNamesAreCompressed) {
    Dns::Message response;
    response.response = true;
    response.questions.push_back({.name = "www.example.com", .type = Dns::Type::A});
    for (int i = 0; i < 3; ++i) {
        response.answers.push_back({.name = "www.example.com", .type = Dns::Type::A, .ttl = 60, .rdata = {192, 0, 2, static_cast<std::uint8_t>(i)}});
    }
    const auto wire = Dns::Serialize(response);
    // Header 12, question 17 + 4, three answers of pointer 2 + 10 + 4.
    BOOST_TEST(wire.size() == 12u + 21u + 3u * 16u);

    const auto parsed = Dns::Parse(wire);
    BOOST_TEST_REQUIRE(parsed.has_value());
    BOOST_TEST_REQUIRE(parsed->answers.size() == 3u);
    BOOST_TEST(parsed->answers[2].name == "www.example.com");
}

BOOST_AUTO_TEST_CASE(OversizedResponsesAreTruncated) {
    Dns::Message response;
    response.response = true;
    response.questions.push_back({.name = "big.example.com", .type = Dns::Type::TXT});
    for (int i = 0; i < 10; ++i) {
        response.answers.push_back({.name = "big.example.com", .type = Dns::Type::TXT, .ttl = 60, .rdata = encode(Dns::Type::TXT, std::string(100, 'x'))});
    }
    const auto wire = Dns::Serialize(response, Dns::ClassicUdpLimit);
    BOOST_TEST(wire.size() <= Dns::ClassicUdpLimit);

    const auto parsed = Dns::Parse(wire);
    BOOST_TEST_REQUIRE(parsed.has_value());
    BOOST_TEST(parsed->truncated);
    BOOST_TEST(parsed->answers.empty());
    BOOST_TEST(parsed->questions.size() == 1u);
}

BOOST_AUTO_TEST_CASE(RecordDataRoundTrips) {
    const std::vector<std::pair<std::uint16_t, std::string> > cases{
            {Dns::Type::A, "192.0.2.1"},
            {Dns::Type::AAAA, "2001:db8::1"},
            {Dns::Type::CNAME, "target.example.com."},
            {Dns::Type::MX, "10 mail.example.com."},
            {Dns::Type::SRV, "10 5 8080 api.example.com."},
            {Dns::Type::TXT, "v=spf1 -all"},
    };
    for (const auto &[type, text]: cases) {
        BOOST_TEST_CONTEXT(Dns::TypeToString(type) << " " << text) {
            BOOST_TEST(Dns::RdataToString(type, encode(type, text)) == text);
        }
    }

    // A TXT value longer than one character-string comes back whole.
    const std::string longText(600, 'q');
    const auto rdata = encode(Dns::Type::TXT, longText);
    BOOST_TEST(rdata.size() == 600u + 3u);
    BOOST_TEST(Dns::RdataToString(Dns::Type::TXT, rdata) == longText);
}

BOOST_AUTO_TEST_CASE(BadValuesAreRefused) {
    std::string error;
    BOOST_TEST(!Dns::EncodeRdata(Dns::Type::A, "2001:db8::1", error).has_value());
    BOOST_TEST(!Dns::EncodeRdata(Dns::Type::AAAA, "192.0.2.1", error).has_value());
    BOOST_TEST(!Dns::EncodeRdata(Dns::Type::MX, "mail.example.com", error).has_value());
    BOOST_TEST(!Dns::EncodeRdata(Dns::Type::SRV, "10 5 70000 api.example.com", error).has_value());
    BOOST_TEST(!Dns::EncodeRdata(Dns::Type::CNAME, "", error).has_value());
    BOOST_TEST(!Dns::EncodeRdata(Dns::Type::SOA, "anything", error).has_value());
    BOOST_TEST(!Dns::EncodeRdata(Dns::Type::CNAME, std::string(64, 'a') + ".example.com", error).has_value());
}

BOOST_AUTO_TEST_CASE(TypesAndNames) {
    BOOST_TEST(Dns::TypeFromString("aaaa").value_or(0) == Dns::Type::AAAA);
    BOOST_TEST(Dns::TypeFromString("TYPE65").value_or(0) == 65);
    BOOST_TEST(!Dns::TypeFromString("BOGUS").has_value());
    BOOST_TEST(Dns::TypeToString(65) == "TYPE65");

    BOOST_TEST(Dns::Normalize("WWW.Example.COM.") == "www.example.com");
    BOOST_TEST(Dns::IsInZone("a.example.com", "example.com"));
    BOOST_TEST(Dns::IsInZone("example.com", "example.com"));
    BOOST_TEST(!Dns::IsInZone("badexample.com", "example.com"));
    BOOST_TEST(!Dns::IsValidName("a..b"));
    BOOST_TEST(!Dns::IsValidName("has space.example.com"));
    BOOST_TEST(Dns::IsValidName("_http._tcp.example.com"));
}

// ── Resolver ─────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(AnswersAnAddress) {
    const auto zones = exampleZones();
    const auto answer = Resolve(zones, "www.example.com", Dns::Type::A);
    BOOST_TEST_REQUIRE(answer.has_value());
    BOOST_TEST(answer->rcode == Dns::Rcode::NoError);
    BOOST_TEST_REQUIRE(answer->answers.size() == 2u);
    // One set, one TTL: the lower of the two values'.
    BOOST_TEST(answer->answers[0].ttl == 120u);
    BOOST_TEST(answer->answers[1].ttl == 120u);
}

BOOST_AUTO_TEST_CASE(KeepsTheQuestionsSpelling) {
    const auto zones = exampleZones();
    const auto answer = Resolve(zones, "WWW.Example.COM.", Dns::Type::A);
    BOOST_TEST_REQUIRE(answer.has_value());
    BOOST_TEST_REQUIRE(!answer->answers.empty());
    BOOST_TEST(answer->answers[0].name == "WWW.Example.COM");
}

BOOST_AUTO_TEST_CASE(NoDataCarriesTheSoa) {
    const auto zones = exampleZones();
    const auto answer = Resolve(zones, "www.example.com", Dns::Type::AAAA);
    BOOST_TEST_REQUIRE(answer.has_value());
    BOOST_TEST(answer->rcode == Dns::Rcode::NoError);
    BOOST_TEST(answer->answers.empty());
    BOOST_TEST_REQUIRE(answer->authority.size() == 1u);
    BOOST_TEST(answer->authority[0].type == Dns::Type::SOA);
    BOOST_TEST(answer->authority[0].name == "example.com");
    BOOST_TEST(Dns::RdataToString(Dns::Type::SOA, answer->authority[0].rdata) ==
               "localhost. hostmaster.example.com. 42 3600 600 604800 60");
}

BOOST_AUTO_TEST_CASE(MissingNameIsNxDomain) {
    const auto zones = exampleZones();
    const auto answer = Resolve(zones, "nope.example.com", Dns::Type::A);
    BOOST_TEST_REQUIRE(answer.has_value());
    BOOST_TEST(answer->rcode == Dns::Rcode::NxDomain);
    BOOST_TEST(answer->answers.empty());
    BOOST_TEST(answer->authority.size() == 1u);
}

BOOST_AUTO_TEST_CASE(EmptyNonTerminalIsNoData) {
    // b.example.com has no records, but a.b.example.com does - so it exists (RFC 8020).
    const auto zones = exampleZones();
    const auto answer = Resolve(zones, "b.example.com", Dns::Type::A);
    BOOST_TEST_REQUIRE(answer.has_value());
    BOOST_TEST(answer->rcode == Dns::Rcode::NoError);
    BOOST_TEST(answer->answers.empty());
}

BOOST_AUTO_TEST_CASE(FollowsACname) {
    const auto zones = exampleZones();
    const auto answer = Resolve(zones, "alias.example.com", Dns::Type::A);
    BOOST_TEST_REQUIRE(answer.has_value());
    BOOST_TEST_REQUIRE(answer->answers.size() == 3u);
    BOOST_TEST(answer->answers[0].type == Dns::Type::CNAME);
    BOOST_TEST(answer->answers[0].name == "alias.example.com");
    BOOST_TEST(answer->answers[1].name == "www.example.com");

    // Asked for the CNAME itself, the chain is not followed.
    const auto direct = Resolve(zones, "alias.example.com", Dns::Type::CNAME);
    BOOST_TEST_REQUIRE(direct.has_value());
    BOOST_TEST(direct->answers.size() == 1u);
}

BOOST_AUTO_TEST_CASE(ACnameOutOfTheZonesStopsThere) {
    const auto zones = exampleZones();
    const auto answer = Resolve(zones, "outside.example.com", Dns::Type::A);
    BOOST_TEST_REQUIRE(answer.has_value());
    BOOST_TEST(answer->rcode == Dns::Rcode::NoError);
    BOOST_TEST_REQUIRE(answer->answers.size() == 1u);
    BOOST_TEST(answer->answers[0].type == Dns::Type::CNAME);
}

BOOST_AUTO_TEST_CASE(ACnameLoopIsServFail) {
    const auto zones = exampleZones();
    const auto answer = Resolve(zones, "loop1.example.com", Dns::Type::A);
    BOOST_TEST_REQUIRE(answer.has_value());
    BOOST_TEST(answer->rcode == Dns::Rcode::ServFail);
}

BOOST_AUTO_TEST_CASE(WildcardsAnswerWithTheAskedName) {
    const auto zones = exampleZones();
    const auto answer = Resolve(zones, "feature-x.dev.example.com", Dns::Type::A);
    BOOST_TEST_REQUIRE(answer.has_value());
    BOOST_TEST_REQUIRE(answer->answers.size() == 1u);
    BOOST_TEST(answer->answers[0].name == "feature-x.dev.example.com");

    // Two labels below: the closest encloser is dev.example.com, whose wildcard answers.
    const auto deeper = Resolve(zones, "a.b.dev.example.com", Dns::Type::A);
    BOOST_TEST_REQUIRE(deeper.has_value());
    BOOST_TEST(deeper->answers.size() == 1u);

    // A wildcard does not answer for a name beside it.
    const auto beside = Resolve(zones, "x.example.com", Dns::Type::A);
    BOOST_TEST_REQUIRE(beside.has_value());
    BOOST_TEST(beside->rcode == Dns::Rcode::NxDomain);
}

BOOST_AUTO_TEST_CASE(TheApexHasSoaAndNs) {
    const auto zones = exampleZones();
    const auto soa = Resolve(zones, "example.com", Dns::Type::SOA);
    BOOST_TEST_REQUIRE(soa.has_value());
    BOOST_TEST(soa->answers.size() == 1u);

    const auto ns = Resolve(zones, "example.com", Dns::Type::NS);
    BOOST_TEST_REQUIRE(ns.has_value());
    BOOST_TEST_REQUIRE(ns->answers.size() == 1u);
    BOOST_TEST(Dns::RdataToString(Dns::Type::NS, ns->answers[0].rdata) == "localhost.");

    const auto any = Resolve(zones, "example.com", Dns::Type::ANY);
    BOOST_TEST_REQUIRE(any.has_value());
    BOOST_TEST(any->answers.size() == 3u);
}

BOOST_AUTO_TEST_CASE(TheMostSpecificZoneWins) {
    const auto zones = exampleZones();
    const auto host = Resolve(zones, "host.lab.example.com", Dns::Type::A);
    BOOST_TEST_REQUIRE(host.has_value());
    BOOST_TEST(host->answers.size() == 1u);

    // Missing in the subzone is NXDOMAIN with the subzone's SOA, not the parent's.
    const auto missing = Resolve(zones, "other.lab.example.com", Dns::Type::A);
    BOOST_TEST_REQUIRE(missing.has_value());
    BOOST_TEST(missing->rcode == Dns::Rcode::NxDomain);
    BOOST_TEST_REQUIRE(missing->authority.size() == 1u);
    BOOST_TEST(missing->authority[0].name == "lab.example.com");
}

BOOST_AUTO_TEST_CASE(NamesInNoZoneAreNotOurs) {
    const auto zones = exampleZones();
    BOOST_TEST(!Resolve(zones, "www.example.org", Dns::Type::A).has_value());
    BOOST_TEST(!Resolve(zones, "notexample.com", Dns::Type::A).has_value());
}

// ── Building the zones ───────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(LabelsFromNames) {
    BOOST_TEST(ToLabel("Image_Processing") == "image-processing");
    BOOST_TEST(ToLabel("--x--") == "x");
    BOOST_TEST(ToLabel("___").empty());
    BOOST_TEST(ToLabel(std::string(64, 'a')).empty());
}

BOOST_AUTO_TEST_CASE(StoredZonesAndServiceNames) {

    Euclid::Database::Entity::EDS::HostedZone stored;
    stored.name = "corp.example";
    stored.ttl = 120;
    stored.modified = std::chrono::system_clock::time_point{std::chrono::seconds{1700000000}};

    Euclid::Database::Entity::EDS::Record good;
    good.zoneName = "corp.example";
    good.name = "api.corp.example";
    good.type = "A";
    good.values = {"10.1.2.3"};

    Euclid::Database::Entity::EDS::Record bad = good;
    bad.name = "broken.corp.example";
    bad.values = {"not-an-address"};

    ServiceNames services;
    services.nodes.push_back({.name = "vogje01-nuc", .address = "192.168.178.20"});
    services.instances.push_back({.application = "image-processing", .address = "192.168.178.20", .port = 9004, .node = "vogje01-nuc"});
    services.instances.push_back({.application = "image-processing", .address = "192.168.178.10", .port = 10004, .node = ""});

    CatalogOptions options;
    options.managerAddress = "192.168.178.10";

    std::vector<std::string> problems;
    const auto zones = BuildZoneSet({stored}, {good, bad}, services, options, problems);

    BOOST_TEST(problems.size() == 1u);

    const auto api = Resolve(zones, "api.corp.example", Dns::Type::A);
    BOOST_TEST_REQUIRE(api.has_value());
    BOOST_TEST_REQUIRE(api->answers.size() == 1u);
    BOOST_TEST(api->answers[0].ttl == 120u);
    BOOST_TEST(Dns::RdataToString(Dns::Type::A, api->answers[0].rdata) == "10.1.2.3");

    const auto app = Resolve(zones, "image-processing.app.euclid.internal", Dns::Type::A);
    BOOST_TEST_REQUIRE(app.has_value());
    BOOST_TEST(app->answers.size() == 2u);
    BOOST_TEST(app->answers[0].ttl == 5u);

    const auto srv = Resolve(zones, "_http._tcp.image-processing.app.euclid.internal", Dns::Type::SRV);
    BOOST_TEST_REQUIRE(srv.has_value());
    BOOST_TEST_REQUIRE(srv->answers.size() == 2u);
    std::set<std::string> targets;
    for (const auto &record: srv->answers) targets.insert(Dns::RdataToString(Dns::Type::SRV, record.rdata));
    BOOST_TEST(targets.contains("0 1 9004 vogje01-nuc.node.euclid.internal."));
    BOOST_TEST(targets.contains("0 1 10004 manager.euclid.internal."));

    const auto node = Resolve(zones, "vogje01-nuc.node.euclid.internal", Dns::Type::A);
    BOOST_TEST_REQUIRE(node.has_value());
    BOOST_TEST(node->answers.size() == 1u);

    const auto missing = Resolve(zones, "nothing.app.euclid.internal", Dns::Type::A);
    BOOST_TEST_REQUIRE(missing.has_value());
    BOOST_TEST(missing->rcode == Dns::Rcode::NxDomain);
}

BOOST_AUTO_TEST_CASE(ServiceNamesCanBeSwitchedOff) {
    CatalogOptions options;
    options.serviceDomain.clear();
    std::vector<std::string> problems;
    const auto zones = BuildZoneSet({}, {}, {}, options, problems);
    BOOST_TEST(zones.Zones().empty());
}

// ── Listener ─────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(RecursionIsLimitedToTheAllowedNetworks) {
    const std::vector<std::string> networks{"127.0.0.0/8", "10.0.0.0/8", "fc00::/7", "not a network"};
    BOOST_TEST(InNetworks(asio::ip::make_address("127.0.0.1"), networks));
    BOOST_TEST(InNetworks(asio::ip::make_address("10.200.3.4"), networks));
    BOOST_TEST(InNetworks(asio::ip::make_address("::ffff:10.1.1.1"), networks));
    BOOST_TEST(InNetworks(asio::ip::make_address("fd12::1"), networks));
    BOOST_TEST(!InNetworks(asio::ip::make_address("8.8.8.8"), networks));
    BOOST_TEST(!InNetworks(asio::ip::make_address("2001:db8::1"), networks));
}

BOOST_AUTO_TEST_CASE(RequestHandling) {
    CatalogOptions catalogOptions;
    catalogOptions.serviceDomain.clear();
    const Catalog catalog(catalogOptions);// not started: no zones
    const DnsListener listener(catalog, ListenerOptions{});
    const auto client = asio::ip::make_address("127.0.0.1");

    // A name in no zone, with no forwarders: refused, and recursion not offered.
    const auto refused = Dns::Parse(listener.Answer(query("www.example.org", Dns::Type::A), client, false));
    BOOST_TEST_REQUIRE(refused.has_value());
    BOOST_TEST(refused->response);
    BOOST_TEST(refused->id == 0x1234);
    BOOST_TEST(refused->rcode == Dns::Rcode::Refused);
    BOOST_TEST(!refused->recursionAvailable);
    BOOST_TEST(refused->questions.size() == 1u);

    // A header followed by nothing parseable: FORMERR, same ID.
    std::vector<std::uint8_t> garbage{0xAB, 0xCD, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF};
    const auto formErr = Dns::Parse(listener.Answer(garbage, client, false));
    BOOST_TEST_REQUIRE(formErr.has_value());
    BOOST_TEST(formErr->id == 0xABCD);
    BOOST_TEST(formErr->rcode == Dns::Rcode::FormErr);

    // A response is never answered.
    auto response = query("www.example.org", Dns::Type::A);
    response[2] |= 0x80;
    BOOST_TEST(listener.Answer(response, client, false).empty());

    // Zone transfers are refused.
    const auto axfr = Dns::Parse(listener.Answer(query("example.com", 252), client, true));
    BOOST_TEST_REQUIRE(axfr.has_value());
    BOOST_TEST(axfr->rcode == Dns::Rcode::Refused);

    // EDNS in, EDNS out.
    const auto edns = Dns::Parse(listener.Answer(query("www.example.org", Dns::Type::A, 7, 4096), client, false));
    BOOST_TEST_REQUIRE(edns.has_value());
    BOOST_TEST(edns->ednsUdpSize.has_value());
}

BOOST_AUTO_TEST_CASE(AnswersOverUdpAndTcp) {
    CatalogOptions catalogOptions;
    catalogOptions.serviceDomain.clear();
    const Catalog catalog(catalogOptions);

    ListenerOptions options;
    options.address = "127.0.0.1";
    options.port = freePort();
    DnsListener listener(catalog, options);
    listener.Start();
    BOOST_TEST_REQUIRE(listener.Serving());

    asio::io_context context;
    const auto request = query("www.example.org", Dns::Type::A, 0x4242);

    // UDP
    asio::ip::udp::socket udp(context, asio::ip::udp::v4());
    udp.send_to(asio::buffer(request), asio::ip::udp::endpoint(asio::ip::make_address("127.0.0.1"), options.port));
    std::array<std::uint8_t, 512> reply{};
    asio::ip::udp::endpoint from;
    const auto size = udp.receive_from(asio::buffer(reply), from);
    const auto udpAnswer = Dns::Parse(std::span(reply.data(), size));
    BOOST_TEST_REQUIRE(udpAnswer.has_value());
    BOOST_TEST(udpAnswer->id == 0x4242);
    BOOST_TEST(udpAnswer->rcode == Dns::Rcode::Refused);

    // TCP: two queries on one connection, each length-prefixed.
    asio::ip::tcp::socket tcp(context);
    tcp.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), options.port));
    for (const std::uint16_t id: {std::uint16_t{1}, std::uint16_t{2}}) {
        const auto message = query("www.example.org", Dns::Type::A, id);
        std::vector<std::uint8_t> framed{static_cast<std::uint8_t>(message.size() >> 8), static_cast<std::uint8_t>(message.size() & 0xFF)};
        framed.insert(framed.end(), message.begin(), message.end());
        asio::write(tcp, asio::buffer(framed));

        std::array<std::uint8_t, 2> length{};
        asio::read(tcp, asio::buffer(length));
        std::vector<std::uint8_t> body((length[0] << 8) | length[1]);
        asio::read(tcp, asio::buffer(body));
        const auto tcpAnswer = Dns::Parse(body);
        BOOST_TEST_REQUIRE(tcpAnswer.has_value());
        BOOST_TEST(tcpAnswer->id == id);
    }

    listener.Stop();
    BOOST_TEST(!listener.Serving());
}
