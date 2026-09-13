// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE AuthorizationGateTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <algorithm>
#include <ranges>
#include <string>

// Euclid includes
#include <euclid/core/Configuration.h>
#include <euclid/core/HttpActionServer.h>
#include <euclid/core/Permissions.h>

using Euclid::Core::Configuration;
using Euclid::Core::HttpActionServer;
using Euclid::Core::Permissions;

// The gate sits in front of every module's handlers and is the only thing between an authenticated
// caller and every action euclid has. It always enforces now: the modes that let the pre-roles
// grant lists decide went when those lists did, because with nothing behind them they would have
// meant "authorize nobody" while reading as "not yet".
//
// Authorize() is driven directly rather than through a server: it is a pure function of the request,
// the mode and the registered lookup, and standing a server up would start a scheduler and a
// monitoring collector to test a decision that needs neither.

namespace {

    namespace http = boost::beast::http;

    // Clears the registered lookups, so the order the tests run in cannot change what they mean.
    struct ConfiguredMode {
        explicit ConfiguredMode(const std::string & = {}) {}
        ~ConfiguredMode() {
            HttpActionServer::SetAuthorizationLookup(nullptr);
            HttpActionServer::SetResourceAuthorizationLookup(nullptr);
        }
    };

    http::request<http::string_body> requestFor(const std::string &target, const std::string &action) {
        http::request<http::string_body> req(http::verb::post, "/", 11);
        req.set("x-euclid-target", target);
        req.set("x-euclid-action", action);
        req.set("x-euclid-account-id", "000000000000");
        req.set("x-euclid-user-id", "jens");
        req.set("x-euclid-namespace", "production");
        return req;
    }

    // Answers whatever it is told to, and records that it was asked at all - which is what the
    // legacy case turns on.
    struct Recorder {
        bool asked{false};
        bool allow{true};

        HttpActionServer::AuthorizationLookup lookup() {
            return [this](const auto &, const std::string &, const std::string &) {
                asked = true;
                return HttpActionServer::AuthorizationDecision{.allowed = allow, .reason = "because the test said so"};
            };
        }
    };

}// namespace

BOOST_AUTO_TEST_CASE(EnforceRefusesWhatTheLookupRefuses) {

    const ConfiguredMode mode("enforce");
    Recorder recorder;
    recorder.allow = false;
    HttpActionServer::SetAuthorizationLookup(recorder.lookup());

    const auto refusal = HttpActionServer::Authorize(requestFor("ens", "publish-message"));

    BOOST_REQUIRE(refusal.has_value());
    BOOST_TEST(refusal->result_int() == 403U);
    // The reason travels: a 403 that does not say why is worked around rather than fixed.
    BOOST_TEST(refusal->body().find("because the test said so") != std::string::npos, "the reason is missing: " + refusal->body());
}

BOOST_AUTO_TEST_CASE(EnforceLetsThroughWhatTheLookupAllows) {

    const ConfiguredMode mode("enforce");
    Recorder recorder;
    recorder.allow = true;
    HttpActionServer::SetAuthorizationLookup(recorder.lookup());

    BOOST_TEST(!HttpActionServer::Authorize(requestFor("ens", "publish-message")).has_value());
    BOOST_TEST(recorder.asked);
}

// A process that never wires a lookup is never refused anything. Every module wires one at startup,
// but a test binary or a tool built on HttpActionServer does not, and it must not start answering
// 403 because somebody set the mode in a shared configuration file.
BOOST_AUTO_TEST_CASE(NoLookupMeansNoRefusal) {

    const ConfiguredMode mode("enforce");
    HttpActionServer::SetAuthorizationLookup(nullptr);

    BOOST_TEST(!HttpActionServer::Authorize(requestFor("ens", "publish-message")).has_value());
}

// emm and emd are not gated by roles, because no role can name their actions - sending them through
// would refuse every request either of them ever receives. They gate themselves: emm by the
// administrator group, emd by being internal.
BOOST_AUTO_TEST_CASE(TheUnbindableModulesAreNotGatedAtAll) {

    const ConfiguredMode mode("enforce");
    Recorder recorder;
    recorder.allow = false;
    HttpActionServer::SetAuthorizationLookup(recorder.lookup());

    for (const auto &module: Permissions::UnbindableModules()) {
        const auto refusal = HttpActionServer::Authorize(requestFor(module, "import"));
        BOOST_TEST(!refusal.has_value(), module + " must not be gated by roles");
    }
    BOOST_TEST(!recorder.asked, "the unbindable modules must not even be evaluated");
}

// A target nobody recognises is *not* waved through: the request still reached a module, which will
// handle it by action alone, so skipping the gate would be a way past it.
BOOST_AUTO_TEST_CASE(AnUnknownTargetIsStillEvaluated) {

    const ConfiguredMode mode("enforce");
    Recorder recorder;
    recorder.allow = false;
    HttpActionServer::SetAuthorizationLookup(recorder.lookup());

    const auto refusal = HttpActionServer::Authorize(requestFor("not-a-module", "publish-message"));

    BOOST_TEST(recorder.asked, "an unknown target must not skip the gate");
    BOOST_TEST(refusal.has_value());
}

// euclid's own metrics push depends on this. Core::Monitoring::MetricsPusher sends
// emo:push-metrics over a module's Unix socket with no credentials at all - it is a local
// module-to-module call, and EMO's handler is deliberately unauthenticated for that reason. The
// registered lookup answers "allowed" for a request with no subject, so the gate lets it by and
// the handler decides; a gate that refused unauthenticated requests instead would return 403 to a
// pusher that never reads the body, and metrics would stop arriving with nothing to say why.
//
// So this is not a convenience. Tightening it breaks monitoring in every module at once, and the
// breakage is silent.
BOOST_AUTO_TEST_CASE(TheGateLeavesUnauthenticatedRequestsToTheHandler) {

    const ConfiguredMode mode("enforce");

    // What the database-side lookup does with a request carrying no subject - see
    // Database::WireAuthorizationLookup().
    bool asked = false;
    HttpActionServer::SetAuthorizationLookup([&asked](const auto &req, const std::string &, const std::string &) {
        asked = true;
        const bool authenticated = !std::string(req[boost::beast::http::field::authorization]).empty();
        return HttpActionServer::AuthorizationDecision{
                .allowed = !authenticated,
                .reason = authenticated ? "no grant applies" : "not authenticated; left to the handler"};
    });

    // No Authorization header, the way MetricsPusher sends.
    auto push = requestFor("emo", "push-metrics");
    BOOST_TEST(!HttpActionServer::Authorize(push).has_value(), "an unauthenticated request must reach its handler");
    BOOST_TEST(asked);

    // And one that did authenticate is decided on its grants, as usual.
    auto authenticated = requestFor("emo", "push-metrics");
    authenticated.set(boost::beast::http::field::authorization, "Bearer something");
    BOOST_TEST(HttpActionServer::Authorize(authenticated).has_value());
}

BOOST_AUTO_TEST_CASE(TheUnbindableModulesHaveNoPermissionsToRequire) {

    for (const auto &module: Permissions::UnbindableModules()) {
        BOOST_TEST(!Permissions::IsBindable(module));
        for (const auto &action: {"import", "replace-one", "start-module", "find-one"}) {
            BOOST_TEST(!Permissions::Exists(Permissions::Of(module, action)));
        }
    }
    BOOST_TEST(std::ranges::contains(Permissions::UnbindableModules(), std::string("emm")));
    BOOST_TEST(std::ranges::contains(Permissions::UnbindableModules(), std::string("emd")));
}


// ── The resource half ───────────────────────────────────────────────────────
//
// The module and the action are headers, settled before any handler runs. The resource is a bucket
// ERN in a body or a queue ERN in a header, so the handler asks once it has read one. Until this
// existed, Grant::resources was stored and never consulted - and euclid's only working resource
// authorization was the resourceGrants list that roles are meant to replace.

namespace {

    constexpr auto kBucket = "ern:esm:eu-central-1:000000000000:production:bucket:reports";

    // Allows one named resource and nothing else, the way a grant with a resource pattern does.
    HttpActionServer::ResourceAuthorizationLookup allowingOnly(const std::string &allowed, bool *asked = nullptr) {
        return [allowed, asked](const auto &, const std::string &, const std::string &, const std::string &resourceErn) {
            if (asked) *asked = true;
            return HttpActionServer::AuthorizationDecision{
                    .allowed = resourceErn == allowed,
                    .reason = resourceErn == allowed ? "granted" : "no grant covers " + resourceErn};
        };
    }

}// namespace

BOOST_AUTO_TEST_CASE(AResourceIsRefusedUnlessAGrantCoversIt) {

    const ConfiguredMode mode("enforce");
    HttpActionServer::SetResourceAuthorizationLookup(allowingOnly(kBucket));

    const auto req = requestFor("esm", "put-object");

    BOOST_TEST(!HttpActionServer::AuthorizeResource(req, kBucket).has_value());
    BOOST_TEST(HttpActionServer::IsResourceAuthorized(req, kBucket));

    // And does not cover this one. The whole point of the change: an empty resourceGrants used to
    // mean "unrestricted", and a grant that does not name a resource now means nothing at all.
    const auto refusal = HttpActionServer::AuthorizeResource(req, "ern:esm:...:bucket:payroll");
    BOOST_REQUIRE(refusal.has_value());
    BOOST_TEST(refusal->result_int() == 403U);
    BOOST_TEST(refusal->body().find("no grant covers") != std::string::npos, "the reason is missing: " + refusal->body());
}

// A process with no resource lookup falls back to the caller's own answer rather than refusing
// everything - the same rule the module-level gate follows.
BOOST_AUTO_TEST_CASE(NoResourceLookupRefusesNothing) {

    // Same rule as the module-level gate: a process that registered no lookup has no grants to
    // consult and must not start answering 403 because of it.
    const ConfiguredMode mode;
    HttpActionServer::SetResourceAuthorizationLookup(nullptr);

    const auto req = requestFor("esm", "put-object");

    BOOST_TEST(!HttpActionServer::AuthorizeResource(req, kBucket).has_value());
    BOOST_TEST(HttpActionServer::IsResourceAuthorized(req, kBucket));
}


// ── The upgrade guard ───────────────────────────────────────────────────────
//
// `euclid.authorization.mode` had three values while euclid was moving off the per-user grant
// lists. Those lists are gone. "legacy" and "shadow" both meant "let the old mechanism decide", so
// honouring either now would mean letting *nothing* decide: every authenticated caller allowed
// everything, with nothing in any log to say so. A configuration file written before this release
// still says one of them, so finding one has to be fatal rather than quiet.

BOOST_AUTO_TEST_CASE(AConfigurationFromBeforeTheRolesReleaseRefusesToStart) {

    for (const auto &stale: {"legacy", "shadow"}) {
        Configuration::instance().set("euclid.authorization.mode", std::string(stale));
        BOOST_CHECK_THROW(HttpActionServer::RequireEnforcingAuthorization(), std::runtime_error);
    }

    Configuration::instance().set("euclid.authorization.mode", std::string("enforce"));
}

BOOST_AUTO_TEST_CASE(EnforceAndAnAbsentSettingBothStart) {

    Configuration::instance().set("euclid.authorization.mode", std::string("enforce"));
    BOOST_CHECK_NO_THROW(HttpActionServer::RequireEnforcingAuthorization());

    // Absent is the same as enforce: there is only one way to run now, so a file that never
    // mentioned the setting is a file that needs no change.
    Configuration::instance().set("euclid.authorization.mode", std::string(""));
    BOOST_CHECK_THROW(HttpActionServer::RequireEnforcingAuthorization(), std::runtime_error);
}

// The message has to name the setting, the value it found, and what to do - an operator reading a
// process that refused to start has nothing else to go on.
BOOST_AUTO_TEST_CASE(TheRefusalSaysWhatToChange) {

    Configuration::instance().set("euclid.authorization.mode", std::string("legacy"));

    try {
        HttpActionServer::RequireEnforcingAuthorization();
        BOOST_FAIL("expected a refusal");
    } catch (const std::runtime_error &ex) {
        const std::string what = ex.what();
        BOOST_TEST(what.find("euclid.authorization.mode") != std::string::npos, what);
        BOOST_TEST(what.find("legacy") != std::string::npos, what);
        BOOST_TEST(what.find("enforce") != std::string::npos, what);
    }

    Configuration::instance().set("euclid.authorization.mode", std::string("enforce"));
}
