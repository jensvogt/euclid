// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE ApplicationLaunchTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <tuple>
#include <vector>

// Euclid includes
#include <euclid/core/ApplicationLaunch.h>
#include <euclid/core/CryptoUtils.h>
#include <euclid/core/DateTimeUtils.h>

namespace Launch = Euclid::Core::Launch;
using namespace std::chrono_literals;

// What starting an application takes, on whichever host starts it. The manager and euclid-wrk both
// call these and nothing else, so every case here holds on both - which is what step 13.1 of
// docs/worker-nodes.md exists for.

namespace {

    // A host with nothing configured: every interpreter is its default.
    std::string unconfigured(const std::string &, const std::string &fallback) { return fallback; }

    auto configured(std::map<std::string, std::string> interpreters) {
        return [interpreters = std::move(interpreters)](const std::string &key, const std::string &fallback) {
            const auto found = interpreters.find(key);
            return found != interpreters.end() ? found->second : fallback;
        };
    }

    // A scratch directory per test, removed on the way out.
    struct Scratch {
        std::filesystem::path path;

        Scratch() : path(std::filesystem::temp_directory_path() / ("euclid-launch-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
            std::filesystem::create_directories(path);
        }

        ~Scratch() {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }

        [[nodiscard]] std::filesystem::path write(const std::string &name, const std::string &content) const {
            const auto file = path / name;
            std::ofstream(file, std::ios::binary) << content;
            return file;
        }
    };

}// namespace

// ── The command line ────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(EachRuntimeStartsWithItsInterpreter) {

    BOOST_TEST(Launch::CommandLine("JAVA", "", "a.jar", {}, unconfigured) == (std::vector<std::string>{"java", "-jar", "a.jar"}));
    BOOST_TEST(Launch::CommandLine("JAVA21", "", "a.jar", {}, unconfigured) == (std::vector<std::string>{"java21", "-jar", "a.jar"}));
    BOOST_TEST(Launch::CommandLine("JAVA25", "", "a.jar", {}, unconfigured) == (std::vector<std::string>{"java25", "-jar", "a.jar"}));
    // The failure the worker had: everything that was not BINARY was handed to java -jar.
    BOOST_TEST(Launch::CommandLine("PYTHON", "", "app.py", {}, unconfigured) == (std::vector<std::string>{"python3", "app.py"}));
    BOOST_TEST(Launch::CommandLine("NODEJS", "", "app.js", {}, unconfigured) == (std::vector<std::string>{"node", "app.js"}));
    BOOST_TEST(Launch::CommandLine("BINARY", "", "./server", {}, unconfigured) == (std::vector<std::string>{"./server"}));
}

BOOST_AUTO_TEST_CASE(TheInterpreterIsWhereTheHostSaysItIs) {

    const auto interpreters = configured({{"java21", "/usr/lib/jvm/java-21/bin/java"}, {"python", "/opt/python/bin/python3"}});

    BOOST_TEST(Launch::CommandLine("JAVA21", "", "a.jar", {}, interpreters) == (std::vector<std::string>{"/usr/lib/jvm/java-21/bin/java", "-jar", "a.jar"}));
    BOOST_TEST(Launch::CommandLine("PYTHON", "", "a.py", {}, interpreters) == (std::vector<std::string>{"/opt/python/bin/python3", "a.py"}));
    // Not configured, so the name nothing ships - which fails to exec with the version in it.
    BOOST_TEST(Launch::CommandLine("JAVA25", "", "a.jar", {}, interpreters).front() == "java25");
}

BOOST_AUTO_TEST_CASE(OnlyTheInterpreterIsAskedFor) {

    // -jar is how a jar is started; what the host is asked is which java.
    std::vector<std::string> asked;
    const auto recording = [&asked](const std::string &key, const std::string &fallback) {
        asked.push_back(key);
        return fallback;
    };

    std::ignore = Launch::CommandLine("JAVA21", "", "a.jar", {}, recording);
    std::ignore = Launch::CommandLine("BINARY", "", "./server", {}, recording);
    std::ignore = Launch::CommandLine("JAVA", "/opt/wrapper.sh", "a.jar", {}, recording);

    BOOST_TEST(asked == (std::vector<std::string>{"java21"}));
}

BOOST_AUTO_TEST_CASE(AnApplicationsOwnCommandOverridesItsRuntime) {

    BOOST_TEST(Launch::CommandLine("JAVA", "/opt/wrapper.sh", "a.jar", {"--port", "8080"}, unconfigured)
               == (std::vector<std::string>{"/opt/wrapper.sh", "a.jar", "--port", "8080"}));
}

BOOST_AUTO_TEST_CASE(ArgumentsComeLast) {

    BOOST_TEST(Launch::CommandLine("BINARY", "", "./server", {"--verbose"}, unconfigured) == (std::vector<std::string>{"./server", "--verbose"}));
    BOOST_TEST(Launch::CommandLine("JAVA", "", "a.jar", {"-x"}, unconfigured) == (std::vector<std::string>{"java", "-jar", "a.jar", "-x"}));
}

BOOST_AUTO_TEST_CASE(AnUnknownRuntimeIsTheArtifactItself) {

    BOOST_TEST(Launch::CommandLine("", "", "./thing", {}, unconfigured) == (std::vector<std::string>{"./thing"}));
    BOOST_TEST(Launch::RuntimeKey("BINARY").empty());
    BOOST_TEST(Launch::RuntimeKey("NODEJS") == "nodejs");
}

// ── The HTTP port ───────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(NoRangeMeansNoPort) {

    BOOST_TEST(Launch::PickHttpPort({}, "i-1", 0, 0) == 0);
    BOOST_TEST(Launch::PickHttpPort({}, "i-1", 9000, 8999) == 0);
}

BOOST_AUTO_TEST_CASE(EachSlotGetsTheFirstFreePortInTheRange) {

    BOOST_TEST(Launch::PickHttpPort({}, "i-1", 9000, 9009) == 9000);
    BOOST_TEST(Launch::PickHttpPort({{"i-1", 9000}, {"i-2", 9001}}, "i-3", 9000, 9009) == 9002);
    // A gap left by a slot that went away is reused.
    BOOST_TEST(Launch::PickHttpPort({{"i-2", 9001}}, "i-3", 9000, 9009) == 9000);
}

BOOST_AUTO_TEST_CASE(ASlotKeepsItsPortAcrossARestart) {

    BOOST_TEST(Launch::PickHttpPort({{"i-1", 9005}}, "i-1", 9000, 9009) == 9005);
}

BOOST_AUTO_TEST_CASE(AFullRangeGivesNothingRatherThanAPortOutsideIt) {

    BOOST_TEST(Launch::PickHttpPort({{"i-1", 9000}, {"i-2", 9001}}, "i-3", 9000, 9001) == 0);
}

// ── The output ──────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(AnApplicationsOutputIsOnItsOwnChannel) {

    BOOST_TEST(Launch::OutputChannel("orders-000000000000-production") == "app.orders-000000000000-production");

    const auto fields = boost::json::parse(Launch::OutputFields("orders", "production", "000000000000")).as_object();
    BOOST_TEST(fields.at("service.name").as_string() == "orders");
    BOOST_TEST(fields.at("namespace").as_string() == "production");
    BOOST_TEST(fields.at("account.id").as_string() == "000000000000");
    BOOST_TEST(!boost::json::parse(Launch::OutputFields("orders", "", "")).as_object().contains("namespace"));
}

BOOST_AUTO_TEST_CASE(AJsonLinesOwnLevelIsTakenAtItsWord) {

    using boost::log::trivial::severity_level;
    BOOST_TEST(Launch::OutputSeverity(R"({"level":"WARN","message":"x"})", false) == severity_level::warning);
    BOOST_TEST(Launch::OutputSeverity(R"({"level":"debug"})", true) == severity_level::debug);
    BOOST_TEST(Launch::OutputSeverity(R"({"level":"SEVERE"})", false) == severity_level::error);
}

BOOST_AUTO_TEST_CASE(OtherwiseThePipeDecides) {

    using boost::log::trivial::severity_level;
    BOOST_TEST(Launch::OutputSeverity("plain text", false) == severity_level::info);
    BOOST_TEST(Launch::OutputSeverity("plain text", true) == severity_level::error);
    BOOST_TEST(Launch::OutputSeverity("{not json", false) == severity_level::info);
    BOOST_TEST(Launch::OutputSeverity(R"({"level":"chatty"})", true) == severity_level::error);
}

BOOST_AUTO_TEST_CASE(ControlBytesNeverReachTheJournalRaw) {

    BOOST_TEST(Launch::SanitizeOutput("a\x1b[31mb") == "a\\x1b[31mb");
    BOOST_TEST(Launch::SanitizeOutput("tab\tkept") == "tab\tkept");
    BOOST_TEST(Launch::SanitizeOutput("caf\xc3\xa9") == "caf\xc3\xa9");
}

// ── The artifact ────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(AnArtifactIsCurrentWhenItsContentMatches) {

    const Scratch scratch;
    const auto file = scratch.write("app.jar", "build 1");
    const auto md5 = Euclid::Core::CryptoUtils::md5SumFile(file.string());

    BOOST_TEST(Launch::ArtifactIsCurrent(file, md5, 7));
    // A rebuild of exactly the same size is still a different build.
    BOOST_TEST(!Launch::ArtifactIsCurrent(file, Euclid::Core::CryptoUtils::md5SumFile(scratch.write("other", "build 2").string()), 7));
}

BOOST_AUTO_TEST_CASE(SizeIsTheFallbackAndNothingKnownMeansFetch) {

    const Scratch scratch;
    const auto file = scratch.write("app.jar", "build 1");

    BOOST_TEST(Launch::ArtifactIsCurrent(file, "", 7));
    BOOST_TEST(!Launch::ArtifactIsCurrent(file, "", 8));
    BOOST_TEST(!Launch::ArtifactIsCurrent(file, "", 0));
    BOOST_TEST(!Launch::ArtifactIsCurrent(scratch.path / "missing.jar", "", 7));
}

#ifndef _WIN32
BOOST_AUTO_TEST_CASE(ABinaryIsMadeExecutableAndNothingElseIs) {

    const Scratch scratch;
    const auto binary = scratch.write("server", "#!/bin/sh\n");
    const auto jar = scratch.write("app.jar", "jar");
    const auto wrapped = scratch.write("tool", "#!/bin/sh\n");

    Launch::PrepareArtifact(binary, "BINARY", "");
    Launch::PrepareArtifact(jar, "JAVA", "");
    Launch::PrepareArtifact(wrapped, "BINARY", "/opt/wrapper.sh");

    const auto executable = [](const std::filesystem::path &path) {
        return (std::filesystem::status(path).permissions() & std::filesystem::perms::owner_exec) != std::filesystem::perms::none;
    };
    BOOST_TEST(executable(binary));
    BOOST_TEST(!executable(jar));
    BOOST_TEST(!executable(wrapped));
}
#endif

// ── The credentials file ────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(CredentialsAreWrittenWholeAndReadBack) {

    const Scratch scratch;
    const auto path = scratch.path / "demo" / Launch::CredentialsFileName;
    const auto expiresAt = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now() + 1h);

    BOOST_TEST_REQUIRE(Launch::WriteCredentials(path, boost::json::object{{"token", "t"}, {"expiresAt", Euclid::Core::DateTimeUtils::ToISO8601(expiresAt)}}));
    BOOST_TEST(!std::filesystem::exists(path.string() + ".new"));

    const auto read = Launch::ReadCredentialsExpiry(path);
    BOOST_TEST_REQUIRE(read.has_value());
    BOOST_TEST(std::chrono::abs(*read - expiresAt) < 1s);

#ifndef _WIN32
    // The token is the application's identity: nobody but its owner reads it.
    const auto mode = std::filesystem::status(path).permissions();
    BOOST_TEST((mode & (std::filesystem::perms::group_all | std::filesystem::perms::others_all)) == std::filesystem::perms::none);
#endif
}

BOOST_AUTO_TEST_CASE(MissingOrUnreadableCredentialsHaveNoExpiry) {

    const Scratch scratch;
    BOOST_TEST(!Launch::ReadCredentialsExpiry(scratch.path / "missing").has_value());
    BOOST_TEST(!Launch::ReadCredentialsExpiry(scratch.write("garbage", "not json")).has_value());
    BOOST_TEST(!Launch::CredentialsExpiry(boost::json::object{{"token", "t"}}).has_value());
}

// The expiry, and every lease deadline a worker acts on, is read back through FromISO8601. It used
// to read the string as local time and add an hour - right in CET winter only - and on Windows
// read nothing at all.
BOOST_AUTO_TEST_CASE(ATimestampRoundTripsInUtcWhateverTheHostsZone) {

    using Euclid::Core::DateTimeUtils;
    const auto now = std::chrono::system_clock::now();
    BOOST_TEST(std::chrono::abs(DateTimeUtils::FromISO8601(DateTimeUtils::ToISO8601(now)) - now) < 1us);

    const auto noon = std::chrono::sys_days{std::chrono::year{2026} / 10 / 5} + 12h;
    BOOST_TEST((DateTimeUtils::FromISO8601("2026-10-05T12:00:00Z") == noon));
    BOOST_TEST((DateTimeUtils::FromISO8601("2026-10-05T12:00:00.250Z") == noon + 250ms));
    BOOST_TEST((DateTimeUtils::FromISO8601("2026-10-05T14:00:00+02:00") == noon));
    BOOST_TEST((DateTimeUtils::FromISO8601("2026-10-05T07:30:00-04:30") == noon));
    BOOST_TEST((DateTimeUtils::FromISO8601("not a date") == std::chrono::system_clock::time_point{}));
}

BOOST_AUTO_TEST_CASE(CredentialsAreReplacedHalfwayThroughTheirLife) {

    const auto issued = std::chrono::system_clock::now();
    BOOST_TEST((Launch::RefreshAt(issued + 1h, issued) == issued + 30min));
    // Already expired: replace them now.
    BOOST_TEST((Launch::RefreshAt(issued - 1s, issued) == issued));
}
