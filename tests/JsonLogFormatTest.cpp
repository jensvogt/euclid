// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE JsonLogFormatTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

// Boost includes
#include <boost/json.hpp>

// Euclid includes
#include <euclid/core/Configuration.h>
#include <euclid/core/LogStream.h>

using Euclid::Core::Configuration;
using Euclid::Core::LogStream;
namespace trivial = boost::log::trivial;

// euclid's logs are shipped by a collector that tails them, which means every line has to be one
// JSON object it can parse and index without being told a format. Two kinds of line go through
// this: euclid's own records, and the lines the manager reads back from a process it started.
//
// The second kind is the interesting one. An application that logs JSON has already decided what
// its record means, and wrapping that in a "message" string would leave every field it carries
// unqueryable - which is the whole reason for asking applications to log JSON. So its fields are
// spliced into the envelope instead, and euclid's own are written after them: what namespace a
// line came from is a question about euclid, and an application must not be able to answer it by
// logging a field of that name.

namespace {

    // The console sink writes to std::cout, so that is where a record is caught.
    struct CapturedLog {

        CapturedLog() : _saved(std::cout.rdbuf(_buffer.rdbuf())) {}
        ~CapturedLog() { std::cout.rdbuf(_saved); }

        [[nodiscard]]
        std::string text() const { return _buffer.str(); }

        // The last line written, parsed. Every line is one object, which is the contract with the
        // collector.
        [[nodiscard]]
        boost::json::object lastObject() const {

            // Split rather than sliced: the arithmetic for "the last line of a string that ends in
            // a newline" is one off-by-one away from dropping the closing brace, which reads as a
            // formatter that produces unparsable JSON.
            std::string line;
            std::istringstream lines(_buffer.str());
            for (std::string candidate; std::getline(lines, candidate);) {
                if (!candidate.empty()) line = candidate;
            }
            if (line.empty()) return {};

            boost::system::error_code ec;
            const auto parsed = boost::json::parse(line, ec);
            if (ec || !parsed.is_object()) return {};
            return parsed.as_object();
        }

    private:

        std::ostringstream _buffer;
        std::streambuf *_saved;
    };

    std::string textOf(const boost::json::object &object, const std::string &key) {
        const auto *value = object.if_contains(key);
        return value != nullptr && value->is_string() ? std::string(value->as_string()) : std::string{};
    }

    struct Fixture {
        Fixture() {
            Configuration::instance().set<std::string>("euclid.logging.format", "json");
            Configuration::instance().set<std::string>("euclid.region", "eu-central-1");
            LogStream::Initialize();
            LogStream::SetSeverity("trace");
            LogStream::SetProcessChannel("esm");
        }
    };

}// namespace

BOOST_FIXTURE_TEST_SUITE(JsonLogFormatTest, Fixture)

    // ── euclid's own records ────────────────────────────────────────────────

    BOOST_AUTO_TEST_CASE(EveryLineIsOneJsonObject) {

        const CapturedLog captured;
        log_error << "something went wrong";

        const auto record = captured.lastObject();
        BOOST_REQUIRE(!record.empty());
        BOOST_TEST(textOf(record, "message") == "something went wrong");
        BOOST_TEST(textOf(record, "log.level") == "error");
        BOOST_TEST(textOf(record, "channel") == "esm");
        BOOST_TEST(textOf(record, "service.name") == "esm");
    }

    // Without these a line in a shared index says nothing about where it came from, which is the
    // whole point of one index holding a whole installation.
    BOOST_AUTO_TEST_CASE(ARecordSaysWhereItCameFrom) {

        const CapturedLog captured;
        log_info << "hello";

        const auto record = captured.lastObject();
        BOOST_TEST(!textOf(record, "@timestamp").empty());
        BOOST_TEST(!textOf(record, "host.name").empty());
        BOOST_TEST(textOf(record, "region") == "eu-central-1");
        BOOST_TEST(record.contains("process.pid"));
        BOOST_TEST(record.contains("log.origin.function"));
        BOOST_TEST(record.contains("log.origin.line"));
    }

    // A timestamp OpenSearch reads as a date without being configured to.
    BOOST_AUTO_TEST_CASE(TheTimestampIsIso8601WithAZone) {

        const CapturedLog captured;
        log_info << "hello";

        const auto stamp = textOf(captured.lastObject(), "@timestamp");
        BOOST_REQUIRE(stamp.size() == 24U);
        BOOST_TEST(stamp[10] == 'T');
        BOOST_TEST(stamp.back() == 'Z');
    }

    // The test above checks that the timestamp *claims* UTC. It does not check that it is UTC, and
    // for a long time it was not: Boost's add_common_attributes() registers "TimeStamp" from a
    // local time generator, that ptime went out with a "Z" on the end, and OpenSearch was told
    // 22:00 UTC when it was 20:00 UTC. Kibana then rendered the instant in the browser's timezone
    // and added the offset a second time, so every record read two hours into the future.
    //
    // The zone is forced rather than inherited, because a machine sitting in UTC cannot tell the
    // two apart - which is the other half of why this went unnoticed.
    BOOST_AUTO_TEST_CASE(TheTimestampIsActuallyUtcAndNotLocalTimeWearingAZ) {

        // POSIX TZ rather than the IANA name "Europe/Berlin": glibc understands both, and the CRT
        // understands only this form - it would take an IANA name, fail to make sense of it, fall
        // back to UTC, and leave the test passing while testing nothing at all. Central European
        // Time either way, so the offset is an hour or two and never zero.
        static constexpr auto kZone = "CET-1CEST";

        struct ForcedZone {
            explicit ForcedZone(const char *zone) : _had(std::getenv("TZ") != nullptr) {
                if (_had) _saved = std::getenv("TZ");
                set("TZ", zone);
            }
            ~ForcedZone() { set("TZ", _had ? _saved.c_str() : ""); }

            static void set(const char *name, const char *value) {
#ifdef _WIN32
                // _putenv_s with an empty value is how the CRT removes a variable; there is no
                // unsetenv.
                _putenv_s(name, value);
                _tzset();
#else
                if (value != nullptr && *value != '\0') setenv(name, value, 1);
                else unsetenv(name);
                tzset();
#endif
            }

            bool _had;
            std::string _saved;
        };

        const ForcedZone zone(kZone);
        BOOST_TEST_REQUIRE(std::string(std::getenv("TZ")) == kZone);

        const auto before = std::time(nullptr);
        std::string stamp;
        {
            const CapturedLog captured;
            log_info << "what time is it";
            stamp = textOf(captured.lastObject(), "@timestamp");
        }
        BOOST_TEST_REQUIRE(stamp.size() == 24U);

        // Read back as UTC, which is what the trailing "Z" promises.
        std::tm parsed{};
        std::istringstream in(stamp.substr(0, 19));
        in >> std::get_time(&parsed, "%Y-%m-%dT%H:%M:%S");
        BOOST_TEST_REQUIRE(!in.fail());

        // timegm() is POSIX; the CRT spells the same thing _mkgmtime. Not mktime, which would read
        // the fields as local time and undo exactly what this is measuring.
#ifdef _WIN32
        const auto reported = _mkgmtime(&parsed);
#else
        const auto reported = timegm(&parsed);
#endif

        // Generous, because the point is not clock precision: a record stamped with local time in
        // this zone is 3600 or 7200 seconds out, and nothing here runs for a minute.
        const auto drift = std::abs(static_cast<long>(reported - before));
        BOOST_TEST_CONTEXT("@timestamp=" << stamp << " drift=" << drift << "s") {
            BOOST_TEST(drift < 60);
        }
    }

    // A quote or a newline in a message must not end the object early, or the collector reads one
    // record as two and neither parses.
    BOOST_AUTO_TEST_CASE(AMessageWithQuotesStaysOneParsableObject) {

        const CapturedLog captured;
        log_error << R"(he said "no" and left)";

        const auto record = captured.lastObject();
        BOOST_REQUIRE(!record.empty());
        BOOST_TEST(textOf(record, "message") == R"(he said "no" and left)");
    }

    // ── lines read back from an application ─────────────────────────────────

    BOOST_AUTO_TEST_CASE(AnApplicationsJsonIsSplicedRatherThanWrapped) {

        const CapturedLog captured;
        LogStream::LogVerbatim("app.parser", trivial::info,
                               R"({"@timestamp":"2026-09-20T12:00:00.000Z","level":"WARN","message":"slow batch",)"
                               R"("logger_name":"de.libri.Parser","thread_name":"main","batchSize":4096})",
                               R"({"service.name":"parser","application.id":"parser","namespace":"development"})");

        const auto record = captured.lastObject();
        BOOST_REQUIRE(!record.empty());

        // The application's own fields, queryable rather than buried in a string.
        BOOST_TEST(textOf(record, "message") == "slow batch");
        BOOST_TEST(textOf(record, "logger_name") == "de.libri.Parser");
        BOOST_TEST(textOf(record, "thread_name") == "main");
        BOOST_TEST(record.contains("batchSize"));

        // Its own timestamp, not the moment euclid happened to read the line.
        BOOST_TEST(textOf(record, "@timestamp") == "2026-09-20T12:00:00.000Z");

        // Logback calls it "level"; everything reading this calls it "log.level".
        BOOST_TEST(textOf(record, "log.level") == "WARN");
        BOOST_TEST(!record.contains("level"));

        // And euclid's own account of where it came from.
        BOOST_TEST(textOf(record, "channel") == "app.parser");
        BOOST_TEST(textOf(record, "namespace") == "development");
        BOOST_TEST(textOf(record, "application.id") == "parser");
    }

    // An application is not allowed to say it is somewhere it is not.
    BOOST_AUTO_TEST_CASE(AnApplicationCannotOverwriteEuclidsOwnFields) {

        const CapturedLog captured;
        LogStream::LogVerbatim("app.parser", trivial::info,
                               R"({"message":"hi","namespace":"production","channel":"app.something-else"})",
                               R"({"namespace":"development"})");

        const auto record = captured.lastObject();
        BOOST_TEST(textOf(record, "namespace") == "development");
        BOOST_TEST(textOf(record, "channel") == "app.parser");
    }

    // A program that logs plain text is the ordinary case and must keep working: its line becomes
    // the message, and euclid supplies everything around it.
    BOOST_AUTO_TEST_CASE(APlainTextLineBecomesTheMessage) {

        const CapturedLog captured;
        LogStream::LogVerbatim("app.legacy", trivial::error, "Segmentation fault",
                               R"({"application.id":"legacy"})");

        const auto record = captured.lastObject();
        BOOST_TEST(textOf(record, "message") == "Segmentation fault");
        BOOST_TEST(textOf(record, "log.level") == "error");
        BOOST_TEST(textOf(record, "application.id") == "legacy");
    }

    // Something that starts like JSON and is not - a stack trace, a truncated line - must not
    // throw away the line or produce half an object.
    BOOST_AUTO_TEST_CASE(AMalformedJsonLineIsKeptAsText) {

        const CapturedLog captured;
        LogStream::LogVerbatim("app.parser", trivial::error, R"({"message":"unterminated)", {});

        const auto record = captured.lastObject();
        BOOST_TEST(textOf(record, "message") == R"({"message":"unterminated)");
    }

    // Where in euclid a record was written means nothing for somebody else's program, so it is
    // left off rather than pointing at the pipe that read it.
    BOOST_AUTO_TEST_CASE(AnApplicationLineCarriesNoEuclidSourceLocation) {

        const CapturedLog captured;
        LogStream::LogVerbatim("app.parser", trivial::info, "started", {});

        const auto record = captured.lastObject();
        BOOST_TEST(!record.contains("log.origin.function"));
        BOOST_TEST(!record.contains("log.origin.line"));
    }

BOOST_AUTO_TEST_SUITE_END()
