#define BOOST_TEST_MODULE HttpActionServerScopeTest
#include <boost/test/unit_test.hpp>

// Euclid includes
#include <euclid/core/Configuration.h>
#include <euclid/core/HttpActionServer.h>
#include <euclid/core/JwtUtils.h>

using Euclid::Core::Configuration;
using Euclid::Core::HttpActionServer;
using Euclid::Core::JwtUtils;

// Covers the three states CheckScope() (inside HttpActionServer::Authenticate()) can be in:
//  1. Neither ScopeLookup nor GrantLookup wired - falls back to the static euclid.account-ids/
//     euclid.namespaces config lists (empty/unconfigured = no restriction), exactly as it behaved
//     before account/namespace management had a database behind it. This is what keeps modules
//     with no database access (e.g. ftp) working unchanged.
//  2. ScopeLookup wired - the account/namespace must exist in the database, regardless of the
//     static config lists.
//  3. GrantLookup wired - the authenticated user must additionally hold a grant for the requested
//     account/namespace.
// Each test case resets the lookups it needs at its own start rather than relying on test
// execution order, since HttpActionServer::Set*Lookup() sets process-wide static state.

namespace {

    constexpr auto kJwtSecret = "test-jwt-secret-at-least-32-bytes-long-for-hs256";

    boost::beast::http::request<boost::beast::http::string_body> buildRequest(const std::string &token, const std::string &accountId, const std::string &ns) {
        namespace http = boost::beast::http;
        http::request<http::string_body> req(http::verb::post, "/", 11);
        req.set(http::field::authorization, "Bearer " + token);
        if (!accountId.empty()) req.set("x-euclid-account-id", accountId);
        if (!ns.empty()) req.set("x-euclid-namespace", ns);
        req.prepare_payload();
        return req;
    }

    struct JwtSecretFixture {
        JwtSecretFixture() {
            Configuration::instance().set<std::string>("euclid.modules.eam.jwt-secret", kJwtSecret);
        }
    };

}// namespace

BOOST_TEST_GLOBAL_FIXTURE(JwtSecretFixture);

BOOST_AUTO_TEST_CASE(NoLookupsWiredFallsBackToStaticConfig) {
    HttpActionServer::SetScopeLookup({});
    HttpActionServer::SetGrantLookup({});

    const auto token = JwtUtils::CreateToken("alice", kJwtSecret);
    const auto req = buildRequest(token, "acct1", "dev");

    const auto auth = HttpActionServer::Authenticate(req);
    BOOST_TEST_REQUIRE(auth.subject.has_value());
    BOOST_TEST(*auth.subject == "alice");
}

BOOST_AUTO_TEST_CASE(ScopeLookupWiredDeniesUnknownAccountOrNamespace) {
    HttpActionServer::SetScopeLookup([](const std::string &, const std::string &) { return false; });
    HttpActionServer::SetGrantLookup({});

    const auto token = JwtUtils::CreateToken("alice", kJwtSecret);
    const auto req = buildRequest(token, "acct1", "dev");

    const auto auth = HttpActionServer::Authenticate(req);
    BOOST_TEST(!auth.subject.has_value());
    BOOST_TEST(!auth.denialReason.empty());

    // Once the account/namespace exist (per the lookup), the request goes through again.
    HttpActionServer::SetScopeLookup([](const std::string &accountId, const std::string &ns) { return accountId == "acct1" && ns == "dev"; });
    const auto auth2 = HttpActionServer::Authenticate(req);
    BOOST_TEST_REQUIRE(auth2.subject.has_value());
    BOOST_TEST(*auth2.subject == "alice");
}

BOOST_AUTO_TEST_CASE(GrantLookupWiredDeniesWithoutMatchingGrant) {
    HttpActionServer::SetScopeLookup([](const std::string &, const std::string &) { return true; });
    HttpActionServer::SetGrantLookup([](const std::string &, const std::string &, const std::string &) { return false; });

    const auto token = JwtUtils::CreateToken("alice", kJwtSecret);
    const auto req = buildRequest(token, "acct1", "dev");

    const auto auth = HttpActionServer::Authenticate(req);
    BOOST_TEST(!auth.subject.has_value());
    BOOST_TEST(!auth.denialReason.empty());

    // A matching grant lets the same request through.
    HttpActionServer::SetGrantLookup([](const std::string &userId, const std::string &accountId, const std::string &ns) {
        return userId == "alice" && accountId == "acct1" && ns == "dev";
    });
    const auto auth2 = HttpActionServer::Authenticate(req);
    BOOST_TEST_REQUIRE(auth2.subject.has_value());
    BOOST_TEST(*auth2.subject == "alice");
}

BOOST_AUTO_TEST_CASE(RequestsWithNoAccountIdBypassGrantCheck) {
    // Account-agnostic actions (e.g. login, get-metrics) carry no x-euclid-account-id and must
    // stay reachable even when GrantLookup would otherwise deny everything.
    HttpActionServer::SetScopeLookup({});
    HttpActionServer::SetGrantLookup([](const std::string &, const std::string &, const std::string &) { return false; });

    const auto token = JwtUtils::CreateToken("alice", kJwtSecret);
    const auto req = buildRequest(token, "", "");

    const auto auth = HttpActionServer::Authenticate(req);
    BOOST_TEST_REQUIRE(auth.subject.has_value());
    BOOST_TEST(*auth.subject == "alice");

    HttpActionServer::SetScopeLookup({});
    HttpActionServer::SetGrantLookup({});
}
