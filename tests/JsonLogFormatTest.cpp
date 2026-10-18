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
#include <euclid/core/ApplicationLaunch.h>
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

    // ── an application's JSON, translated ──────────────────────────────────
    //
    // Launch::EmitOutput is what the worker and the manager run every line of an application's
    // output through. A JSON line comes out as a record of euclid's own: its message, level and
    // moment as the record's, the rest as ECS fields - one schema whoever logged.
    //
    // The record is read after the capture has ended, not inside it: Boost.Test reports a failed
    // check on std::cout, which the capture has taken over - so a check inside it fails silently.

    boost::json::object emitted(const std::string &line, const std::string &fields = {}) {
        boost::json::object record;
        {
            const CapturedLog captured;
            Euclid::Core::Launch::EmitOutput("app.file-copy", line, false, fields);
            record = captured.lastObject();
        }
        return record;
    }

    BOOST_AUTO_TEST_CASE(ALogstashRecordBecomesARecordOfEuclidsOwn) {

        // What the PIM services' LogstashEncoder writes.
        const auto record = emitted(
                R"({"@timestamp":"2026-10-18T01:26:51.899898343+02:00","@version":"1","message":"Starting Application",)"
                R"("logger_name":"l.p.l.f.Application","thread_name":"main","level":"WARN","level_value":30000,)"
                R"("caller_class_name":"org.springframework.boot.StartupInfoLogger","caller_method_name":"logStarting",)"
                R"("caller_file_name":"StartupInfoLogger.java","caller_line_number":54,"batchSize":4096})",
                R"({"service.name":"file-copy","application.id":"file-copy","namespace":"development"})");

        BOOST_TEST(textOf(record, "message") == "Starting Application");
        // euclid's level spelling, so log.level:warning finds application and euclid records alike.
        BOOST_TEST(textOf(record, "log.level") == "warning");
        // The application's moment, in the collector's form.
        BOOST_TEST(textOf(record, "@timestamp") == "2026-10-17T23:26:51.899Z");
        // Under ECS names.
        BOOST_TEST(textOf(record, "log.logger") == "l.p.l.f.Application");
        BOOST_TEST(textOf(record, "process.thread.name") == "main");
        BOOST_TEST(textOf(record, "log.origin.function") == "org.springframework.boot.StartupInfoLogger.logStarting");
        BOOST_TEST(textOf(record, "log.origin.file.name") == "StartupInfoLogger.java");
        BOOST_TEST(record.contains("log.origin.file.line"));
        // Its own field, under labels; the encoder's bookkeeping, nowhere.
        BOOST_TEST(record.contains("labels.batchSize"));
        BOOST_TEST(!record.contains("batchSize"));
        BOOST_TEST(!record.contains("level"));
        BOOST_TEST(!record.contains("@version"));
        BOOST_TEST(!record.contains("level_value"));
        // And euclid's account of where it came from.
        BOOST_TEST(textOf(record, "channel") == "app.file-copy");
        BOOST_TEST(textOf(record, "application.id") == "file-copy");
    }

    // logback-spring.xml in the PIM services adds "service" as a custom field; euclid adds
    // "service.name". At the top level OpenSearch had to store "service" as a value and an object at
    // once, and refused every record. Under labels it cannot collide with anything.
    BOOST_AUTO_TEST_CASE(AnApplicationsOwnFieldsGoUnderLabels) {

        const auto record = emitted(R"({"message":"x","service":"file-copy","kubernetes.namespace_name":"pim-dev"})",
                                    R"({"service.name":"file-copy"})");

        BOOST_TEST(!record.contains("service"));
        BOOST_TEST(textOf(record, "labels.service") == "file-copy");
        BOOST_TEST(textOf(record, "labels.kubernetes_namespace_name") == "pim-dev");
        BOOST_TEST(textOf(record, "service.name") == "file-copy");
    }

    // ECS, nested the way Spring Boot's "ecs" format writes some of it: already euclid's vocabulary,
    // so kept where it is.
    BOOST_AUTO_TEST_CASE(AnEcsRecordKeepsItsEcsFields) {

        const auto record = emitted(
                R"({"@timestamp":"2026-10-18T10:00:00.000Z","log":{"level":"ERROR","logger":"d.l.Parser"},)"
                R"("process":{"thread":{"name":"worker-1"}},"message":"parse failed",)"
                R"("error":{"type":"java.io.IOException","stack_trace":"java.io.IOException: closed\n\tat d.l.Parser.run"},)"
                R"("ecs":{"version":"8.11"}})");

        BOOST_TEST(textOf(record, "log.level") == "error");
        BOOST_TEST(textOf(record, "log.logger") == "d.l.Parser");
        BOOST_TEST(textOf(record, "process.thread.name") == "worker-1");
        BOOST_TEST(textOf(record, "error.type") == "java.io.IOException");
        BOOST_TEST(textOf(record, "error.stack_trace").starts_with("java.io.IOException: closed"));
        BOOST_TEST(!record.contains("ecs.version"));
        BOOST_TEST(textOf(record, "@timestamp") == "2026-10-18T10:00:00.000Z");
    }

    // Python's logging with a JSON formatter.
    BOOST_AUTO_TEST_CASE(APythonRecordIsReadToo) {

        const auto record = emitted(R"({"asctime":"2026-10-18 01:26:51,123","levelname":"INFO","name":"importer","message":"done"})");

        BOOST_TEST(textOf(record, "message") == "done");
        BOOST_TEST(textOf(record, "log.level") == "info");
        BOOST_TEST(textOf(record, "@timestamp") == "2026-10-18T01:26:51.123Z");
        // "name" is too common a word to take for the logger; it stays the application's own.
        BOOST_TEST(textOf(record, "labels.name") == "importer");
    }

    // Logback's encoder writes local time with an offset and nanoseconds. Fluent Bit's parser takes
    // one format and warned on every line that had another - so the application's moment is kept,
    // in euclid's form.
    BOOST_AUTO_TEST_CASE(AnApplicationsTimestampIsBroughtToUtcMilliseconds) {

        const auto stampOf = [](const std::string &stamp) {
            return textOf(emitted(R"({"@timestamp":")" + stamp + R"(","message":"x"})"), "@timestamp");
        };

        BOOST_TEST(stampOf("2026-10-18T01:26:51.9+0200") == "2026-10-17T23:26:51.900Z");
        BOOST_TEST(stampOf("2026-10-18T01:26:51-05:30") == "2026-10-18T06:56:51.000Z");
        BOOST_TEST(stampOf("2026-09-20T12:00:00.000Z") == "2026-09-20T12:00:00.000Z");
        // Across a month boundary, which is where hand-done date arithmetic goes wrong.
        BOOST_TEST(stampOf("2026-03-01T00:30:00.000+01:00") == "2026-02-28T23:30:00.000Z");
    }

    // A timestamp nothing can read gives way to the moment euclid read the line - and stays, under
    // labels, for whoever wants to know what the application wrote.
    BOOST_AUTO_TEST_CASE(AnUnreadableTimestampBecomesEuclids) {

        const auto record = emitted(R"({"@timestamp":"yesterday","message":"x"})");

        const auto stamp = textOf(record, "@timestamp");
        BOOST_TEST(stamp.size() == 24U);
        BOOST_TEST(stamp.back() == 'Z');
        BOOST_TEST(textOf(record, "labels.@timestamp") == "yesterday");
    }

    // An application is not allowed to say it is somewhere it is not.
    BOOST_AUTO_TEST_CASE(AnApplicationCannotOverwriteEuclidsOwnFields) {

        const auto record = emitted(R"({"message":"hi","namespace":"production","channel":"app.something-else","service.name":"evil"})",
                                    R"({"namespace":"development","service.name":"file-copy"})");

        BOOST_TEST(textOf(record, "namespace") == "development");
        BOOST_TEST(textOf(record, "channel") == "app.file-copy");
        BOOST_TEST(textOf(record, "service.name") == "file-copy");
        // What it said is kept, where it cannot be mistaken for euclid's.
        BOOST_TEST(textOf(record, "labels.namespace") == "production");
    }

    // A JSON line without a message is still a line somebody wanted written.
    BOOST_AUTO_TEST_CASE(AJsonLineWithoutAMessageKeepsTheLine) {

        const auto record = emitted(R"({"event":"tick","count":3})");
        BOOST_TEST(textOf(record, "message") == R"({"event":"tick","count":3})");
        BOOST_TEST(textOf(record, "log.level") == "info");
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
