#define BOOST_TEST_MODULE WorkerThreadsConfigTest
#include <boost/test/unit_test.hpp>

// Euclid includes
#include <euclid/core/Configuration.h>
#include <euclid/core/HttpActionServer.h>

using Euclid::Core::Configuration;
using Euclid::Core::HttpActionServer;

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
