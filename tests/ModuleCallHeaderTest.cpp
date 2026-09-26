// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE ModuleCallHeaderTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <string>

// Euclid includes
#include <TransferContext.h>
#include <euclid/core/Permissions.h>

// A transfer server reaches ESM over a module socket rather than through the gateway, so it builds
// the request itself - and for a while it built one with no x-euclid-target. The authorization gate
// requires "<target>:<action>", so every call asked to be allowed ":list-objects" or ":put-object",
// which are not permissions the vocabulary contains. Those are refused before a grant is read, so
// every FTP and SFTP session logged in and then took a 403 on everything, and no amount of granting
// changed it. The same actions through the gateway worked, because the proxy sets the header
// itself - which is what made it look like a grant problem.
//
// The header is part of the wire contract; this pins the client's half of it.

namespace http = boost::beast::http;

BOOST_AUTO_TEST_SUITE(ModuleCallHeaderTest)

BOOST_AUTO_TEST_CASE(a_module_request_carries_both_halves_of_the_permission) {

    const auto request = Euclid::Transfer::Detail::BuildModuleRequest(
            "esm", "list-objects", "token-value", {{"x-euclid-namespace", "development"}}, R"({"prefix":""})");

    const auto target = std::string(request["x-euclid-target"]);
    const auto action = std::string(request["x-euclid-action"]);

    BOOST_TEST(target == "esm");
    BOOST_TEST(action == "list-objects");

    // The point of the header, stated the way the gate states it: what the two build has to be a
    // permission that exists, or nothing can allow the call.
    BOOST_TEST(Euclid::Core::Permissions::Exists(Euclid::Core::Permissions::Of(target, action)));

    // The rest of the contract, so a refactor cannot quietly drop one of these either.
    BOOST_TEST(std::string(request[http::field::authorization]) == "Bearer token-value");
    BOOST_TEST(std::string(request["x-euclid-namespace"]) == "development");
    BOOST_TEST(request.body() == R"({"prefix":""})");
}

BOOST_AUTO_TEST_CASE(every_action_a_transfer_server_calls_is_a_real_permission) {

    // The four ESM actions TransferStorage uses. Each has to name a permission that exists, or the
    // gate refuses it however the caller is granted - which is the failure this test exists for.
    for (const auto *action: {"list-objects", "put-object", "get-object", "delete-object"}) {
        const auto request = Euclid::Transfer::Detail::BuildModuleRequest("esm", action, "t", {}, "");
        const auto permission = Euclid::Core::Permissions::Of(std::string(request["x-euclid-target"]),
                                                             std::string(request["x-euclid-action"]));
        BOOST_TEST_INFO("action: " << action);
        BOOST_TEST(Euclid::Core::Permissions::Exists(permission));
    }
}

BOOST_AUTO_TEST_CASE(an_empty_target_is_not_a_permission) {

    // Why the tests above matter: this is what the gate was being asked for, and it is
    // unsatisfiable - Matches() refuses a required permission outside the vocabulary whatever was
    // granted, including "*".
    BOOST_TEST(!Euclid::Core::Permissions::Exists(Euclid::Core::Permissions::Of("", "list-objects")));
    BOOST_TEST(!Euclid::Core::Permissions::Matches(Euclid::Core::Permissions::Everything,
                                                   Euclid::Core::Permissions::Of("", "list-objects")));
}

BOOST_AUTO_TEST_CASE(a_failed_call_is_logged_with_what_the_module_said) {

    // "status: 403" on its own is what made a missing header look like a missing grant: the reason
    // was in the body all along and never reached the log.
    const Euclid::Transfer::ModuleResponse refused{
            .status = 403, .body = R"({"error":"':list-objects' is not a permission any role can hold"})"};
    BOOST_TEST(refused.describe().find("not a permission any role can hold") != std::string::npos);

    // An object action answers with the object, and a download that fails must not put the file in
    // the log.
    const Euclid::Transfer::ModuleResponse binary{.status = 500, .body = std::string("\x01\x02\x03\x00 bytes", 10)};
    BOOST_TEST(binary.describe() == "500 (10 bytes)");

    const Euclid::Transfer::ModuleResponse unreachable{.status = 0, .body = ""};
    BOOST_TEST(unreachable.describe() == "0");
}

BOOST_AUTO_TEST_SUITE_END()
