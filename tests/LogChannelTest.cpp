#define BOOST_TEST_MODULE LogChannelTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <sstream>
#include <string>

// Euclid includes
#include <euclid/core/Configuration.h>
#include <euclid/core/LogStream.h>

using Euclid::Core::Configuration;
using Euclid::Core::LogStream;
namespace trivial = boost::log::trivial;

// Every record carries the channel of whatever produced it: a module logs on its own name, and
// output the manager reads back from a process it started logs on that process's ("app.parser").
// What has to hold is how a level is found for a channel - its own if it has one, otherwise the
// nearest enclosing one, otherwise the process-wide level - because that is what lets one noisy
// application be turned off with a single line without silencing anything else.

BOOST_AUTO_TEST_SUITE(LogChannelTest)

    BOOST_AUTO_TEST_CASE(AChannelWithNothingSaidAboutItFollowsTheProcessLevel) {

        LogStream::SetSeverity("warning");
        LogStream::ClearChannelSeverity("esm");

        const auto level = LogStream::SeverityFor("esm");
        BOOST_REQUIRE(level.has_value());
        BOOST_CHECK_EQUAL(*level, trivial::warning);
    }

    BOOST_AUTO_TEST_CASE(AChannelWithItsOwnLevelIgnoresTheProcessLevel) {

        LogStream::SetSeverity("warning");
        BOOST_CHECK(LogStream::SetChannelSeverity("esm", "debug"));

        const auto level = LogStream::SeverityFor("esm");
        BOOST_REQUIRE(level.has_value());
        BOOST_CHECK_EQUAL(*level, trivial::debug);

        // And says nothing about anybody else.
        BOOST_CHECK_EQUAL(*LogStream::SeverityFor("eqs"), trivial::warning);
        LogStream::ClearChannelSeverity("esm");
    }

    BOOST_AUTO_TEST_CASE(ALevelSetOnAPrefixCoversEverythingUnderIt) {

        LogStream::SetSeverity("info");
        BOOST_CHECK(LogStream::SetChannelSeverity("app", "error"));

        // One line turns down every application euclid runs, whatever they are called - which is
        // the point, since an operator should not have to name them one by one.
        BOOST_CHECK_EQUAL(*LogStream::SeverityFor("app.parser"), trivial::error);
        BOOST_CHECK_EQUAL(*LogStream::SeverityFor("app.pim-writer"), trivial::error);

        // The nearest enclosing channel wins, so one application can be handled differently.
        BOOST_CHECK(LogStream::SetChannelSeverity("app.parser", "trace"));
        BOOST_CHECK_EQUAL(*LogStream::SeverityFor("app.parser"), trivial::trace);
        BOOST_CHECK_EQUAL(*LogStream::SeverityFor("app.pim-writer"), trivial::error);

        LogStream::ClearChannelSeverity("app");
        LogStream::ClearChannelSeverity("app.parser");
    }

    BOOST_AUTO_TEST_CASE(OffSilencesAChannelWhateverTheSeverity) {

        LogStream::SetSeverity("trace");
        BOOST_CHECK(LogStream::SetChannelSeverity("app.parser", "off"));

        // Nothing, rather than a level nothing can be below: a channel that is off records not
        // even a fatal.
        BOOST_CHECK(!LogStream::SeverityFor("app.parser").has_value());
        BOOST_CHECK(LogStream::SeverityFor("app.other").has_value());

        LogStream::ClearChannelSeverity("app.parser");
        BOOST_CHECK(LogStream::SeverityFor("app.parser").has_value());
    }

    BOOST_AUTO_TEST_CASE(AnUnreadableLevelChangesNothing) {

        LogStream::SetSeverity("info");
        BOOST_CHECK(LogStream::SetChannelSeverity("eqs", "warning"));

        // Refused rather than applied as some default: "warnign" meaning "info" would be a channel
        // logging more than its author asked for, and one meaning "off" would be silence nobody
        // asked for at all.
        BOOST_CHECK(!LogStream::SetChannelSeverity("eqs", "warnign"));
        BOOST_CHECK_EQUAL(*LogStream::SeverityFor("eqs"), trivial::warning);

        LogStream::ClearChannelSeverity("eqs");
    }

    BOOST_AUTO_TEST_CASE(TheProcessLevelIsKeptWhenTheNewOneIsNotALevel) {

        LogStream::SetSeverity("warning");
        LogStream::SetSeverity("wrning");

        BOOST_CHECK_EQUAL(LogStream::GetSeverity(), "warning");
        BOOST_CHECK_EQUAL(*LogStream::SeverityFor("anything"), trivial::warning);
    }

    BOOST_AUTO_TEST_CASE(ConfigurationDecidesTheLevelsAndAReloadTakesThemBack) {

        auto &configuration = Configuration::instance();
        configuration.set<std::string>("euclid.logging.level", "info");
        configuration.set<std::string>("euclid.logging.channels.app", "off");
        configuration.set<std::string>("euclid.logging.channels.esm", "debug");

        LogStream::ApplyConfiguration();
        BOOST_CHECK(!LogStream::SeverityFor("app.parser").has_value());
        BOOST_CHECK_EQUAL(*LogStream::SeverityFor("esm"), trivial::debug);
        BOOST_CHECK_EQUAL(*LogStream::SeverityFor("eqs"), trivial::info);

        // Applying a configuration that no longer mentions a channel puts it back under the
        // process-wide level, rather than leaving yesterday's override in force with nothing in
        // the file to explain it.
        configuration.set<std::string>("euclid.logging.channels.app", "info");
        configuration.set<std::string>("euclid.logging.channels.esm", "info");
        LogStream::ApplyConfiguration();
        BOOST_CHECK_EQUAL(*LogStream::SeverityFor("esm"), trivial::info);
        BOOST_CHECK(LogStream::SeverityFor("app.parser").has_value());
    }

    BOOST_AUTO_TEST_CASE(TheOutputOfWhatEuclidRunsStaysVisibleWhenTheLevelIsTurnedDown) {

        auto &configuration = Configuration::instance();
        configuration.set<std::string>("euclid.logging.level", "error");
        LogStream::ApplyConfiguration();

        // It used to bypass the log core entirely and was printed whatever the level was, so a
        // level of "error" must not be what silences an application's own logging - only saying so
        // does.
        BOOST_CHECK_EQUAL(*LogStream::SeverityFor("app.parser"), trivial::info);
        BOOST_CHECK_EQUAL(*LogStream::SeverityFor("module.esm"), trivial::info);

        // A channel no test above has configured, since the configuration is a singleton and
        // whatever an earlier case put in it is still there.
        BOOST_CHECK_EQUAL(*LogStream::SeverityFor("ens"), trivial::error);
    }

    BOOST_AUTO_TEST_CASE(TheProcessLogsOnItsOwnChannel) {

        BOOST_CHECK_EQUAL(LogStream::ProcessChannel(), LogStream::kDefaultChannel);

        LogStream::SetProcessChannel("eag");
        BOOST_CHECK_EQUAL(LogStream::ProcessChannel(), "eag");
    }

    BOOST_AUTO_TEST_CASE(ALevelThatIsStoredRatherThanAppliedIsReadBackCanonically) {

        // What an application's row carries, so that "none" and "off" do not end up looking like
        // two different settings to whatever later compares them - see the EAP set-log-level
        // action and the manager's reconcile.
        BOOST_CHECK_EQUAL(LogStream::CanonicalLevel("off").value_or("?"), "off");
        BOOST_CHECK_EQUAL(LogStream::CanonicalLevel("none").value_or("?"), "off");
        BOOST_CHECK_EQUAL(LogStream::CanonicalLevel("silent").value_or("?"), "off");
        BOOST_CHECK_EQUAL(LogStream::CanonicalLevel("warning").value_or("?"), "warning");
        BOOST_CHECK_EQUAL(LogStream::CanonicalLevel("trace").value_or("?"), "trace");

        BOOST_CHECK(!LogStream::CanonicalLevel("warnign").has_value());
        BOOST_CHECK(!LogStream::CanonicalLevel("").has_value());
    }

    // Everything above is about what a level resolves to; this is about what actually comes out,
    // which is the part anybody notices. The console sink writes to std::cout, so std::cout is
    // where the test reads it back from.
    BOOST_AUTO_TEST_CASE(WhatIsWrittenIsWhatTheChannelLevelsAllow) {

        std::ostringstream captured;
        auto *previous = std::cout.rdbuf(captured.rdbuf());

        LogStream::Initialize();
        LogStream::SetProcessChannel("esm");
        LogStream::SetSeverity("info");
        BOOST_REQUIRE(LogStream::SetChannelSeverity("app.parser", "off"));
        BOOST_REQUIRE(LogStream::SetChannelSeverity("app.pim", "info"));

        log_info << "from the module itself";
        LogStream::LogVerbatim("app.parser", boost::log::trivial::error, "the silenced application");
        LogStream::LogVerbatim("app.pim", boost::log::trivial::info, "2026-09-07 the application's own line");

        std::cout.rdbuf(previous);
        const auto output = captured.str();

        BOOST_CHECK(output.contains("from the module itself"));
        BOOST_CHECK(!output.contains("the silenced application"));

        // Verbatim: the application's line as it wrote it, with no second timestamp, severity or
        // source location of ours in front of it.
        BOOST_CHECK(output.contains("\n2026-09-07 the application's own line\n")
                    || output.starts_with("2026-09-07 the application's own line\n"));

        LogStream::ClearChannelSeverity("app.parser");
        LogStream::ClearChannelSeverity("app.pim");
    }

BOOST_AUTO_TEST_SUITE_END()
