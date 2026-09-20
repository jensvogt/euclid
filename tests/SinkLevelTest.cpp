// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE SinkLevelTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

// Euclid includes
#include <euclid/core/Configuration.h>
#include <euclid/core/LogStream.h>

using Euclid::Core::Configuration;
using Euclid::Core::LogStream;

// Once the log file is being shipped somewhere searchable, the two outputs want different things:
// a person watching a console wants warnings, and an index wants everything worth filtering on
// later. So a sink may narrow what the core's channel filter allows - but only narrow it, and
// only when somebody has written the threshold down.
//
// The last part is the whole reason this is configurable rather than fixed. The console once
// carried a hardcoded filter at "info" that silently discarded everything a level of "debug"
// asked for, and the fix at the time was to delete it. A threshold nobody can see is the problem;
// a threshold in the configuration is not.

namespace {

    struct Captured {
        Captured() : _saved(std::cout.rdbuf(_buffer.rdbuf())) {}
        ~Captured() { std::cout.rdbuf(_saved); }

        [[nodiscard]]
        std::string text() const { return _buffer.str(); }

    private:
        std::ostringstream _buffer;
        std::streambuf *_saved;
    };

    std::string fileContents(const std::filesystem::path &dir) {
        std::string all;
        for (const auto &entry: std::filesystem::directory_iterator(dir)) {
            std::ifstream in(entry.path());
            std::ostringstream buffer;
            buffer << in.rdbuf();
            all += buffer.str();
        }
        return all;
    }

    // A directory of its own per run, so what is read back is what this test wrote.
    std::filesystem::path freshLogDir() {
        const auto dir = std::filesystem::temp_directory_path() / ("euclid-sink-level-" + std::to_string(::getpid()));
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        return dir;
    }

}// namespace

BOOST_AUTO_TEST_CASE(AConsoleAtWarningLeavesTheFileAtInfo) {

    const auto dir = freshLogDir();

    // The floor: what is produced at all. Neither sink can ask for more than this.
    Configuration::instance().set<std::string>("euclid.logging.level", "info");
    Configuration::instance().set<std::string>("euclid.logging.console-level", "warning");
    Configuration::instance().set<std::string>("euclid.logging.file-level", "info");

    LogStream::RemoveConsoleLogs();
    LogStream::RemoveFile();
    LogStream::Initialize();
    LogStream::SetSeverity("info");
    LogStream::SetProcessChannel("esm");

    std::string console;
    {
        const Captured captured;
        LogStream::AddFile(dir.string(), "euclid", 1024L * 1024L, 5);

        log_info << "an ordinary thing happened";
        log_error << "something went wrong";

        console = captured.text();
    }
    LogStream::RemoveFile();

    const auto file = fileContents(dir);

    // The file keeps both...
    BOOST_TEST(file.find("an ordinary thing happened") != std::string::npos);
    BOOST_TEST(file.find("something went wrong") != std::string::npos);

    // ...and the console shows only what somebody watching it needs to see.
    BOOST_TEST(console.find("an ordinary thing happened") == std::string::npos);
    BOOST_TEST(console.find("something went wrong") != std::string::npos);

    std::filesystem::remove_all(dir);
}

BOOST_AUTO_TEST_CASE(NoThresholdMeansNoFilterAtAll) {

    const auto dir = freshLogDir();

    // Neither key set, which is every installation that predates them: both sinks then get
    // exactly what the core's channel filter allows, as they always did.
    Configuration::instance().set<std::string>("euclid.logging.console-level", "");
    Configuration::instance().set<std::string>("euclid.logging.file-level", "");

    LogStream::RemoveConsoleLogs();
    LogStream::RemoveFile();
    LogStream::Initialize();
    LogStream::SetSeverity("debug");
    LogStream::SetProcessChannel("esm");

    std::string console;
    {
        const Captured captured;
        LogStream::AddFile(dir.string(), "euclid", 1024L * 1024L, 5);
        log_debug << "a debug line nobody asked to hide";
        console = captured.text();
    }
    LogStream::RemoveFile();

    BOOST_TEST(console.find("a debug line nobody asked to hide") != std::string::npos);
    BOOST_TEST(fileContents(dir).find("a debug line nobody asked to hide") != std::string::npos);

    std::filesystem::remove_all(dir);
}

// A sink narrows, it does not widen: asking a sink for "debug" when the floor is "warning" gets
// warnings, because the record was never produced.
BOOST_AUTO_TEST_CASE(ASinkCannotAskForMoreThanTheFloorProduces) {

    const auto dir = freshLogDir();

    Configuration::instance().set<std::string>("euclid.logging.console-level", "");
    Configuration::instance().set<std::string>("euclid.logging.file-level", "debug");

    LogStream::RemoveConsoleLogs();
    LogStream::RemoveFile();
    LogStream::Initialize();
    LogStream::SetSeverity("warning");
    LogStream::SetProcessChannel("esm");

    {
        const Captured captured;
        LogStream::AddFile(dir.string(), "euclid", 1024L * 1024L, 5);
        log_info << "below the floor";
        log_error << "above it";
    }
    LogStream::RemoveFile();

    const auto file = fileContents(dir);
    BOOST_TEST(file.find("below the floor") == std::string::npos);
    BOOST_TEST(file.find("above it") != std::string::npos);

    std::filesystem::remove_all(dir);
}

// "off" silences one output without touching the other - which is how a service that logs to a
// file and is watched through it stops writing to a console nobody reads.
BOOST_AUTO_TEST_CASE(ASinkCanBeTurnedOffOnItsOwn) {

    const auto dir = freshLogDir();

    Configuration::instance().set<std::string>("euclid.logging.console-level", "off");
    Configuration::instance().set<std::string>("euclid.logging.file-level", "");

    LogStream::RemoveConsoleLogs();
    LogStream::RemoveFile();
    LogStream::Initialize();
    LogStream::SetSeverity("info");
    LogStream::SetProcessChannel("esm");

    std::string console;
    {
        const Captured captured;
        LogStream::AddFile(dir.string(), "euclid", 1024L * 1024L, 5);
        log_error << "kept in the file, not on the console";
        console = captured.text();
    }
    LogStream::RemoveFile();

    BOOST_TEST(console.find("kept in the file") == std::string::npos);
    BOOST_TEST(fileContents(dir).find("kept in the file") != std::string::npos);

    std::filesystem::remove_all(dir);
}

// ── Format, decided the same way ────────────────────────────────────────────
//
// The same argument as the levels, for the same reason: a console is read by a person and a file
// that a collector tails is read by a machine, and insisting both be the same shape means one of
// them is wrong for whoever is reading it.

BOOST_AUTO_TEST_CASE(APlainConsoleCanSitOverAJsonFile) {

    const auto dir = freshLogDir();

    Configuration::instance().set<std::string>("euclid.logging.console-level", "");
    Configuration::instance().set<std::string>("euclid.logging.file-level", "");
    Configuration::instance().set<std::string>("euclid.logging.format", "text");
    Configuration::instance().set<std::string>("euclid.logging.console-format", "text");
    Configuration::instance().set<std::string>("euclid.logging.file-format", "json");

    LogStream::RemoveConsoleLogs();
    LogStream::RemoveFile();
    LogStream::Initialize();
    LogStream::SetSeverity("info");
    LogStream::SetProcessChannel("esm");

    std::string console;
    {
        const Captured captured;
        LogStream::AddFile(dir.string(), "euclid", 1024L * 1024L, 5);
        log_error << "one record, two shapes";
        console = captured.text();
    }
    LogStream::RemoveFile();

    const auto file = fileContents(dir);

    // The console line is for a person: no braces, and the message where the eye expects it.
    BOOST_TEST(console.find("one record, two shapes") != std::string::npos);
    BOOST_TEST(console.find("[error]") != std::string::npos);
    BOOST_TEST(console.find("\"log.level\"") == std::string::npos);

    // The file line is for the collector: one object, with the fields the console leaves implicit.
    BOOST_TEST(file.find("\"log.level\":\"error\"") != std::string::npos);
    BOOST_TEST(file.find("\"channel\":\"esm\"") != std::string::npos);
    BOOST_TEST(file.find("one record, two shapes") != std::string::npos);

    std::filesystem::remove_all(dir);
}

// Neither key set is every installation that predates them: both sinks follow
// euclid.logging.format, exactly as they did when it was one decision for the whole process.
BOOST_AUTO_TEST_CASE(WithoutPerSinkFormatsBothFollowTheGlobalOne) {

    const auto dir = freshLogDir();

    Configuration::instance().set<std::string>("euclid.logging.console-format", "");
    Configuration::instance().set<std::string>("euclid.logging.file-format", "");
    Configuration::instance().set<std::string>("euclid.logging.format", "json");

    LogStream::RemoveConsoleLogs();
    LogStream::RemoveFile();
    LogStream::Initialize();
    LogStream::SetSeverity("info");
    LogStream::SetProcessChannel("esm");

    std::string console;
    {
        const Captured captured;
        LogStream::AddFile(dir.string(), "euclid", 1024L * 1024L, 5);
        log_error << "both in json";
        console = captured.text();
    }
    LogStream::RemoveFile();

    BOOST_TEST(console.find("\"log.level\":\"error\"") != std::string::npos);
    BOOST_TEST(fileContents(dir).find("\"log.level\":\"error\"") != std::string::npos);

    std::filesystem::remove_all(dir);
}
