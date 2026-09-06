#define BOOST_TEST_MODULE RouteMatchTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <string>
#include <vector>

// Euclid includes
#include <RouteTable.h>

using Euclid::EAG::RouteTable;
using Route = Euclid::Database::Entity::EAG::Route;

// What the API gateway does with a request is decided entirely by these two functions: order()
// puts the routes in the sequence matchIn() scans, and matchIn() picks one. Everything else in the
// module is plumbing around that decision - so this is where the rules are worth pinning down.
//
// The rules, and why each exists:
//
//   - longest prefix wins, so a general route can be laid over an application and a narrower one
//     carved out of it later without either being rewritten;
//   - a prefix only matches whole segments, so "/api" never claims "/api-intern";
//   - a route naming no methods answers for all of them, so an existing route keeps working
//     unchanged when methods are added to the model;
//   - a path that matches with the wrong method does not end the scan, because two routes may
//     share a path and split it by method;
//   - only when nothing on the path takes the method is it refused, and then with the methods
//     that were on offer, so the gateway can say 405 and Allow rather than a misleading 404.

namespace {

    Route route(const std::string &routeId, const std::string &path, const std::string &applicationId,
                const std::vector<std::string> &methods = {}) {
        Route r;
        r.routeId = routeId;
        r.path = path;
        r.applicationId = applicationId;
        r.methods = methods;
        return r;
    }

    Route inNamespace(const std::string &routeId, const std::string &path, const std::string &applicationId,
                      const std::string &nameSpace) {
        Route r;
        r.routeId = routeId;
        r.path = path;
        r.applicationId = applicationId;
        r.nameSpace = nameSpace;
        return r;
    }

    // Routes as the table holds them: ordered once, exactly as refresh() does it.
    std::vector<Route> ordered(std::vector<Route> routes) {
        RouteTable::order(routes);
        return routes;
    }

}// namespace

BOOST_AUTO_TEST_CASE(MatchesAPathBeneathThePrefix) {
    const auto routes = ordered({route("parser", "/api/produkte", "parser")});

    const auto match = RouteTable::matchIn(routes, "/api/produkte/searchById", "GET", "");

    BOOST_REQUIRE(match.route.has_value());
    BOOST_TEST(match.route->applicationId == "parser");
}

BOOST_AUTO_TEST_CASE(MatchesThePrefixItself) {
    const auto routes = ordered({route("parser", "/api/produkte", "parser")});

    BOOST_REQUIRE(RouteTable::matchIn(routes, "/api/produkte", "GET", "").route.has_value());
}

BOOST_AUTO_TEST_CASE(MatchesAcrossAQueryString) {
    // The proxy strips the query before matching, but a path ending directly in "?" is accepted
    // too, so a caller cannot slip past a route by leaving out the trailing slash.
    const auto routes = ordered({route("parser", "/api/produkte", "parser")});

    BOOST_REQUIRE(RouteTable::matchIn(routes, "/api/produkte?id=1", "GET", "").route.has_value());
}

BOOST_AUTO_TEST_CASE(DoesNotClaimASimilarlyNamedPath) {
    const auto routes = ordered({route("parser", "/api/produkte", "parser")});

    const auto match = RouteTable::matchIn(routes, "/api/produkte-intern/x", "GET", "");

    BOOST_TEST(!match.route.has_value());
    BOOST_TEST(match.allowed.empty());
}

BOOST_AUTO_TEST_CASE(LongestPrefixWinsRegardlessOfInsertionOrder) {
    // Deliberately given in the unhelpful order: the general route first.
    const auto routes = ordered({route("general", "/api", "gateway-app"),
                                 route("specific", "/api/produkte", "parser")});

    const auto match = RouteTable::matchIn(routes, "/api/produkte/x", "GET", "");

    BOOST_REQUIRE(match.route.has_value());
    BOOST_TEST(match.route->routeId == "specific");
}

BOOST_AUTO_TEST_CASE(ARouteWithoutMethodsAnswersForEveryMethod) {
    const auto routes = ordered({route("parser", "/api/produkte", "parser")});

    for (const auto &method: {"GET", "POST", "PUT", "DELETE", "OPTIONS", "PATCH", "HEAD"}) {
        BOOST_TEST_CONTEXT(method) {
            BOOST_TEST(RouteTable::matchIn(routes, "/api/produkte", method, "").route.has_value());
        }
    }
}

BOOST_AUTO_TEST_CASE(AMethodTheRouteNamesIsMatched) {
    const auto routes = ordered({route("lesen", "/api/produkte", "parser", {"GET", "HEAD"})});

    BOOST_REQUIRE(RouteTable::matchIn(routes, "/api/produkte", "GET", "").route.has_value());
}

BOOST_AUTO_TEST_CASE(AMethodTheRouteDoesNotNameReportsWhatWouldHaveWorked) {
    const auto routes = ordered({route("lesen", "/api/produkte", "parser", {"GET", "HEAD"})});

    const auto match = RouteTable::matchIn(routes, "/api/produkte", "DELETE", "");

    // Not a 404: the path exists, and the caller is told how to ask for it properly.
    BOOST_TEST(!match.route.has_value());
    BOOST_TEST(match.allowed == (std::set<std::string>{"GET", "HEAD"}));
}

BOOST_AUTO_TEST_CASE(TwoRoutesSplitOnePathByMethod) {
    const auto routes = ordered({route("lesen", "/api/produkte", "parser", {"GET", "HEAD"}),
                                 route("pflege", "/api/produkte", "pim-writer", {"POST", "PUT", "DELETE"})});

    const auto read = RouteTable::matchIn(routes, "/api/produkte", "GET", "");
    const auto write = RouteTable::matchIn(routes, "/api/produkte", "POST", "");

    BOOST_REQUIRE(read.route.has_value());
    BOOST_REQUIRE(write.route.has_value());
    BOOST_TEST(read.route->applicationId == "parser");
    BOOST_TEST(write.route->applicationId == "pim-writer");
}

BOOST_AUTO_TEST_CASE(AMethodNeitherRouteTakesCollectsBothOffers) {
    const auto routes = ordered({route("lesen", "/api/produkte", "parser", {"GET"}),
                                 route("pflege", "/api/produkte", "pim-writer", {"POST"})});

    const auto match = RouteTable::matchIn(routes, "/api/produkte", "DELETE", "");

    BOOST_TEST(!match.route.has_value());
    BOOST_TEST(match.allowed == (std::set<std::string>{"GET", "POST"}));
}

BOOST_AUTO_TEST_CASE(AWrongMethodFallsThroughToABroaderRoute) {
    // The narrow route claims only GET on its subtree, so a POST beneath it is not refused - it
    // belongs to the route that covers everything else, which is the whole reason the scan
    // continues rather than stopping at the first path match.
    const auto routes = ordered({route("general", "/api", "gateway-app"),
                                 route("lesen", "/api/produkte", "parser", {"GET"})});

    const auto match = RouteTable::matchIn(routes, "/api/produkte/x", "POST", "");

    BOOST_REQUIRE(match.route.has_value());
    BOOST_TEST(match.route->routeId == "general");
}

BOOST_AUTO_TEST_CASE(NoRouteAtAllIsNotAMethodProblem) {
    const auto routes = ordered({route("parser", "/api/produkte", "parser", {"GET"})});

    const auto match = RouteTable::matchIn(routes, "/somewhere/else", "GET", "");

    BOOST_TEST(!match.route.has_value());
    // Empty is what tells the proxy to answer 404 rather than 405.
    BOOST_TEST(match.allowed.empty());
}

BOOST_AUTO_TEST_CASE(AnEmptyTableMatchesNothing) {
    BOOST_TEST(!RouteTable::matchIn({}, "/api/produkte", "GET", "").route.has_value());
}

// One installation can serve several environments by giving each its own port. The namespace comes
// from the port a request arrived on, so the same path means different things on different
// listeners - which is what keeps the environment out of everybody's URLs.

BOOST_AUTO_TEST_CASE(AListenerSeesOnlyItsOwnNamespace) {
    const auto routes = ordered({inNamespace("dev", "/api/produkte", "parser-dev", "development"),
                                 inNamespace("int", "/api/produkte", "parser-int", "integration")});

    const auto onDev = RouteTable::matchIn(routes, "/api/produkte", "GET", "development");
    const auto onInt = RouteTable::matchIn(routes, "/api/produkte", "GET", "integration");

    BOOST_REQUIRE(onDev.route.has_value());
    BOOST_REQUIRE(onInt.route.has_value());
    BOOST_TEST(onDev.route->applicationId == "parser-dev");
    BOOST_TEST(onInt.route->applicationId == "parser-int");
}

BOOST_AUTO_TEST_CASE(AListenerDoesNotSeeAnotherNamespacesRoute) {
    const auto routes = ordered({inNamespace("dev", "/api/produkte", "parser-dev", "development")});

    BOOST_TEST(!RouteTable::matchIn(routes, "/api/produkte", "GET", "production").route.has_value());
}

BOOST_AUTO_TEST_CASE(ARouteNamingNoNamespaceIsServedByEveryListener) {
    // What every route created before listeners existed looks like, so they keep working.
    const auto routes = ordered({route("shared", "/api/produkte", "parser")});

    BOOST_TEST(RouteTable::matchIn(routes, "/api/produkte", "GET", "development").route.has_value());
    BOOST_TEST(RouteTable::matchIn(routes, "/api/produkte", "GET", "production").route.has_value());
}

BOOST_AUTO_TEST_CASE(AnUnscopedListenerSeesEveryNamespace) {
    // A single-listener installation, which is what a production euclid usually is.
    const auto routes = ordered({inNamespace("dev", "/api/produkte", "parser-dev", "development")});

    BOOST_TEST(RouteTable::matchIn(routes, "/api/produkte", "GET", "").route.has_value());
}
