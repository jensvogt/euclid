// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE WorkerThreadsConfigTest
#include <boost/test/unit_test.hpp>

// Euclid includes
#include <euclid/core/Configuration.h>
#include <euclid/core/HttpActionServer.h>
#include <euclid/core/UnixSocketServer.h>

using Euclid::Core::Configuration;
using Euclid::Core::HttpActionServer;
using Euclid::Core::UnixSocketServer;

// How many worker threads a module runs decides how many requests it can be in the middle of at
// once, and getting it wrong does not announce itself: a module with too few threads does not
// fail, its callers just wait behind whatever is already running. So the figure is configurable
// per module, and what it resolves to has to be predictable.

BOOST_AUTO_TEST_CASE(AModuleThatConfiguresNothingKeepsItsOwnDefault) {
    // The ordinary case - euclid.json says nothing about threads, and the module's compiled-in
    // figure stands.
    BOOST_TEST(HttpActionServer::ConfiguredWorkerThreads("nosuchmodule", 8) == 8);
}

BOOST_AUTO_TEST_CASE(AConfiguredCountIsUsed) {
    Configuration::instance().set<long>("euclid.modules.ees.threads", 24);

    BOOST_TEST(HttpActionServer::ConfiguredWorkerThreads("ees", 8) == 24);
}

BOOST_AUTO_TEST_CASE(EachModuleReadsItsOwnSetting) {
    Configuration::instance().set<long>("euclid.modules.ees.threads", 24);
    Configuration::instance().set<long>("euclid.modules.eqs.threads", 12);

    BOOST_TEST(HttpActionServer::ConfiguredWorkerThreads("ees", 8) == 24);
    BOOST_TEST(HttpActionServer::ConfiguredWorkerThreads("eqs", 8) == 12);
}

BOOST_AUTO_TEST_CASE(ZeroThreadsWouldAnswerNothingAndIsRefused) {
    // A module with no thread accepts connections and never reads them, which looks exactly like
    // a module that is hanging. Better to run with one and say so than to start something that
    // cannot work.
    Configuration::instance().set<long>("euclid.modules.emo.threads", 0);
    BOOST_TEST(HttpActionServer::ConfiguredWorkerThreads("emo", 2) == 1);

    Configuration::instance().set<long>("euclid.modules.emo.threads", -4);
    BOOST_TEST(HttpActionServer::ConfiguredWorkerThreads("emo", 2) == 1);
}

BOOST_AUTO_TEST_CASE(AnAbsurdCountIsCappedRatherThanAttempted) {
    // One typo away from a number of OS threads that will not be created. Failing to start is a
    // worse answer to a misconfigured thread count than running with a sane one.
    Configuration::instance().set<long>("euclid.modules.esm.threads", 1000000);

    BOOST_TEST(HttpActionServer::ConfiguredWorkerThreads("esm", 2) == 256);
}

BOOST_AUTO_TEST_CASE(TheClampGuardsCallersThatNeverAskedTheConfigurationReader) {
    // ConfiguredWorkerThreads is not the only way a count reaches a server: the gateway reads
    // euclid.gateway.http.max-thread itself and hands the number straight to a constructor. That
    // count sizes a std::vector of workers, and a negative int is not a small size there - it is
    // a very large unsigned one, so the reserve() throws and the gateway reports "failed to
    // start" without a word about the typo behind it. The clamp lives low enough to cover both
    // routes in.
    BOOST_TEST(UnixSocketServer::ClampWorkerThreads(-1) == 1);
    BOOST_TEST(UnixSocketServer::ClampWorkerThreads(0) == 1);
    BOOST_TEST(UnixSocketServer::ClampWorkerThreads(8) == 8);
    BOOST_TEST(UnixSocketServer::ClampWorkerThreads(256) == 256);
    BOOST_TEST(UnixSocketServer::ClampWorkerThreads(1000000) == 256);

    // A count that does not fit an int at all, which is what a pasted-in millisecond timestamp
    // looks like when it lands on the wrong configuration key. The parameter is a 64-bit type
    // rather than a long, so this arrives as what it is: a long is 32 bits on Windows, and the
    // value this exists to catch was being truncated on its way in - to 0, and then clamped up to
    // 1, so the guard reported a thread rather than the ceiling.
    BOOST_TEST(UnixSocketServer::ClampWorkerThreads(4294967296LL) == 256);
    BOOST_TEST(UnixSocketServer::ClampWorkerThreads(1758800000000LL) == 256);
}

// The same figure, arriving the way it really would: through the configuration file.
//
// A JSON number is 64-bit whatever the platform, and reading one into a long truncates it where a
// long is 32 bits. Truncating is worse than refusing here, because the low 32 bits of a large
// number is usually a plausible-looking small one - 4294967296 reads as 0, and 1,758,800,000,000
// as 1,051,373,568 or, one digit along, as something that could pass for a deliberate setting.
BOOST_AUTO_TEST_CASE(AConfiguredCountTooLargeForALongIsNotTruncatedIntoLookingReasonable) {

    Configuration::instance().set<long long>("euclid.modules.ees.threads", 4294967296LL);

    // Whatever a long is here, it does not come back small. On Linux this is the number itself; on
    // Windows it is LONG_MAX, because the reader clamps rather than wraps. Both are answers the
    // clamp below can act on; a wrapped 0 is not.
    BOOST_TEST(Configuration::instance().getOr<long>("euclid.modules.ees.threads", 0L) > 1000000L);

    // And so the count a server actually runs with is the ceiling, on both platforms.
    BOOST_TEST(HttpActionServer::ConfiguredWorkerThreads("ees", 8) == 256);
}

// Where 64 bits are the point - a size in bytes rather than a count of something small - the value
// is asked for as a long long and arrives whole on every platform.
BOOST_AUTO_TEST_CASE(ASizeInBytesIsReadAsSixtyFourBitsWherever) {

    constexpr long long twelveGigabytes = 12LL * 1024 * 1024 * 1024;
    Configuration::instance().set<long long>("euclid.gateway.http.max-body", twelveGigabytes);

    BOOST_TEST(Configuration::instance().getOr<long long>("euclid.gateway.http.max-body", 0LL) == twelveGigabytes);
}
