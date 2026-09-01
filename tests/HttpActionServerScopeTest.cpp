#define BOOST_TEST_MODULE HttpActionServerScopeTest
#include <boost/test/unit_test.hpp>

// Euclid includes
#include <euclid/core/Configuration.h>
#include <euclid/core/HttpActionServer.h>
#include <euclid/core/HttpSignature.h>
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

    boost::beast::http::request<boost::beast::http::string_body> buildRequest(const std::string &token, const std::string &accountId, const std::string &ns, const std::string &region = "") {
        namespace http = boost::beast::http;
        http::request<http::string_body> req(http::verb::post, "/", 11);
        req.set(http::field::authorization, "Bearer " + token);
        if (!accountId.empty()) req.set("x-euclid-account-id", accountId);
        if (!ns.empty()) req.set("x-euclid-namespace", ns);
        if (!region.empty()) req.set("x-euclid-region", region);
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

BOOST_AUTO_TEST_CASE(Rfc9421SignedRequestAuthenticatesAsTheKeyOwner) {
    // The third way in, alongside a bearer token and a SigV4 signature: an RFC 9421 signature
    // carries no Authorization header at all, so Authenticate() has to notice it on its own.
    HttpActionServer::SetScopeLookup({});
    HttpActionServer::SetGrantLookup({});
    HttpActionServer::SetAccessKeyLookup([](const std::string &accessKeyId) -> std::optional<HttpActionServer::AccessKeyRecord> {
        if (accessKeyId != "AKIAEXAMPLE") return std::nullopt;
        return HttpActionServer::AccessKeyRecord{.secretAccessKey = "topsecret", .userId = "alice"};
    });

    namespace http = boost::beast::http;
    http::request<http::string_body> req(http::verb::post, "/", 11);
    req.set(http::field::host, "localhost:5566");
    req.set("x-euclid-target", "esm");
    req.set("x-euclid-action", "list-objects");
    req.set("x-euclid-region", "eu-central-1");
    req.set("x-euclid-account-id", "000000000000");
    req.set("x-euclid-user-id", "alice");
    req.body() = R"({"bucketErn":"ern:esm:x"})";
    req.prepare_payload();
    Euclid::Core::HttpSignature::Sign(req, "AKIAEXAMPLE", "topsecret");

    const auto auth = HttpActionServer::Authenticate(req);
    BOOST_TEST_REQUIRE(auth.subject.has_value());
    BOOST_TEST(*auth.subject == "alice");

    // A signature the key store cannot resolve is refused, not waved through as unsigned.
    HttpActionServer::SetAccessKeyLookup([](const std::string &) -> std::optional<HttpActionServer::AccessKeyRecord> { return std::nullopt; });
    const auto denied = HttpActionServer::Authenticate(req);
    BOOST_TEST(!denied.subject.has_value());
    BOOST_TEST(!denied.denialReason.empty());

    HttpActionServer::SetAccessKeyLookup({});
}

BOOST_AUTO_TEST_CASE(ResourceGrantsNarrowACallerToNamedResources) {
    // The check a handler makes once it knows which bucket or queue a request is about. Account
    // and namespace scope are settled before a handler runs; this is the narrower question, and
    // the one an application's principal is held to.
    HttpActionServer::SetResourceLookup({});

    // Nothing wired: every resource is allowed, which is how a module that has not been taught
    // about this - and every deployment before it existed - keeps behaving.
    BOOST_TEST(HttpActionServer::IsResourceAllowed("alice", "ern:esm:eu-central-1:000000000000::bucket:inbox"));

    HttpActionServer::SetResourceLookup([](const std::string &userId, const std::string &resourceErn) {
        if (userId == "alice") return true;// stands in for a user with no grants at all
        return resourceErn == "ern:esm:eu-central-1:000000000000::bucket:inbox";
    });

    BOOST_TEST(HttpActionServer::IsResourceAllowed("app-inbox", "ern:esm:eu-central-1:000000000000::bucket:inbox"));
    BOOST_TEST(!HttpActionServer::IsResourceAllowed("app-inbox", "ern:esm:eu-central-1:000000000000::bucket:payroll"));
    BOOST_TEST(HttpActionServer::IsResourceAllowed("alice", "ern:esm:eu-central-1:000000000000::bucket:payroll"));

    // An unnamed resource is not a denial: handlers that have not resolved one yet, and actions
    // that are about no resource at all, must not be refused by this.
    BOOST_TEST(HttpActionServer::IsResourceAllowed("app-inbox", ""));
    BOOST_TEST(HttpActionServer::IsResourceAllowed("", "ern:esm:eu-central-1:000000000000::bucket:payroll"));

    HttpActionServer::SetResourceLookup({});
}

BOOST_AUTO_TEST_CASE(ConfiguredRegionRequiresMatchingRegionHeader) {
    // The region check precedes everything else in CheckScope() and is not conditional on the
    // request naming an account, so any caller that omits x-euclid-region is denied outright once
    // euclid.region is set - which is what a module-to-module caller (e.g. a transfer server
    // reaching ESM) has to send along with its bearer token.
    HttpActionServer::SetScopeLookup({});
    HttpActionServer::SetGrantLookup({});
    Configuration::instance().set<std::string>("euclid.region", "eu-central-1");

    const auto token = JwtUtils::CreateToken("alice", kJwtSecret);

    const auto noRegion = HttpActionServer::Authenticate(buildRequest(token, "", ""));
    BOOST_TEST(!noRegion.subject.has_value());
    BOOST_TEST(!noRegion.denialReason.empty());

    const auto wrongRegion = HttpActionServer::Authenticate(buildRequest(token, "", "", "us-east-1"));
    BOOST_TEST(!wrongRegion.subject.has_value());
    BOOST_TEST(!wrongRegion.denialReason.empty());

    const auto matching = HttpActionServer::Authenticate(buildRequest(token, "", "", "eu-central-1"));
    BOOST_TEST_REQUIRE(matching.subject.has_value());
    BOOST_TEST(*matching.subject == "alice");

    // Process-wide configuration, so it has to go back the way it was found for any case after
    // this one.
    Configuration::instance().set<std::string>("euclid.region", "");
}
