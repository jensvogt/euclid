// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE ApplicationLogFileTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

// Euclid includes
#include <euclid/core/ApplicationLaunch.h>
#include <euclid/core/Configuration.h>
#include <euclid/core/LogStream.h>

using Euclid::Core::Configuration;
using Euclid::Core::LogStream;

// euclid.logging.application-files: each application's output in a file of its own,
// <dir>/<applicationId>.log, besides wherever it already went. Driven through Launch::EmitOutput,
// which is where both the worker and the manager hand an application's lines to the log.

namespace {

    std::filesystem::path freshDir(const std::string &name) {
        const auto dir = std::filesystem::temp_directory_path() / ("euclid-app-log-" + name + "-" + std::to_string(std::random_device{}()));
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        return dir;
    }

    std::string contents(const std::filesystem::path &file) {
        std::ifstream in(file);
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    void emit(const std::string &applicationId, const std::string &line) {
        Euclid::Core::Launch::EmitOutput("app." + applicationId, line, false, "");
    }

    struct Fixture {
        Fixture() {
            Configuration::instance().set<std::string>("euclid.logging.level", "info");
            Configuration::instance().set<std::string>("euclid.logging.file-format", "text");
            LogStream::Initialize();
        }
        ~Fixture() { LogStream::DisableApplicationFiles(); }
    };

}// namespace

BOOST_AUTO_TEST_CASE(OnlyAnApplicationsChannelNamesAFile) {
    BOOST_TEST(LogStream::ApplicationFileName("app.transformation") == "transformation");
    BOOST_TEST(LogStream::ApplicationFileName("app.katalogdaten-rule") == "katalogdaten-rule");
    BOOST_TEST(LogStream::ApplicationFileName("wrk").empty());
    BOOST_TEST(LogStream::ApplicationFileName("module.esm").empty());
    BOOST_TEST(LogStream::ApplicationFileName("app.").empty());
}

BOOST_AUTO_TEST_CASE(AnApplicationIdCannotNameAPath) {
    BOOST_TEST(LogStream::ApplicationFileName("app.../../etc/passwd") == ".._.._etc_passwd");
    BOOST_TEST(LogStream::ApplicationFileName("app.a/b") == "a_b");
    BOOST_TEST(LogStream::ApplicationFileName("app.a\\b") == "a_b");
    BOOST_TEST(LogStream::ApplicationFileName("app..").empty());
    BOOST_TEST(LogStream::ApplicationFileName("app...").empty());
}

BOOST_FIXTURE_TEST_CASE(EachApplicationWritesItsOwnFile, Fixture) {
    const auto dir = freshDir("own");
    LogStream::EnableApplicationFiles(dir.string(), 10 * 1024 * 1024, 3);

    emit("transformation", "transformed record 4711");
    emit("parsing", "parsed file onix.xml");
    log_info << "a line of the process's own";

    const auto transformation = contents(dir / "transformation.log");
    const auto parsing = contents(dir / "parsing.log");
    BOOST_TEST(transformation.find("transformed record 4711") != std::string::npos);
    BOOST_TEST(transformation.find("parsed file onix.xml") == std::string::npos);
    BOOST_TEST(transformation.find("a line of the process's own") == std::string::npos);
    BOOST_TEST(parsing.find("parsed file onix.xml") != std::string::npos);
    BOOST_TEST(parsing.find("transformed record 4711") == std::string::npos);
}

BOOST_FIXTURE_TEST_CASE(NothingIsWrittenUntilEnabled, Fixture) {
    const auto dir = freshDir("off");
    emit("transformation", "before enabling");
    BOOST_TEST(!std::filesystem::exists(dir / "transformation.log"));
}

BOOST_FIXTURE_TEST_CASE(AChannelThatIsOffWritesNothingToItsFile, Fixture) {
    const auto dir = freshDir("silenced");
    LogStream::EnableApplicationFiles(dir.string(), 10 * 1024 * 1024, 3);
    BOOST_REQUIRE(LogStream::SetChannelSeverity("app.noisy", "off"));

    emit("noisy", "should not appear");
    BOOST_TEST(contents(dir / "noisy.log").find("should not appear") == std::string::npos);
    LogStream::ClearChannelSeverity("app.noisy");
}

BOOST_FIXTURE_TEST_CASE(RotatedFilesStayInTheApplicationsOwnDirectory, Fixture) {
    const auto dir = freshDir("rotate");
    // A file of the process's own in the same directory, which rotation must leave alone.
    std::ofstream(dir / "euclid-wrk.log") << "the worker's own\n";

    LogStream::EnableApplicationFiles(dir.string(), 512, 2);
    const std::string padding(100, 'x');
    for (int i = 0; i < 200; ++i) emit("busy", "line " + std::to_string(i) + " " + padding);

    BOOST_TEST(std::filesystem::exists(dir / "busy.log"));
    BOOST_TEST(std::filesystem::exists(dir / "euclid-wrk.log"));

    long rotated = 0;
    for (const auto &entry: std::filesystem::directory_iterator(dir / "busy")) {
        BOOST_TEST(entry.path().filename().string().starts_with("busy_"));
        ++rotated;
    }
    // Rotated many times over, and only the configured number kept.
    BOOST_TEST(rotated >= 1);
    BOOST_TEST(rotated <= 2);

    // Nothing of the application's was rotated into the shared directory.
    for (const auto &entry: std::filesystem::directory_iterator(dir)) {
        BOOST_TEST(!entry.path().filename().string().starts_with("busy_"));
    }
}
