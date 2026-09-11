// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE OneLoginTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <chrono>
#include <string>

// Euclid includes
#include <euclid/cli/eam/OneLogin.h>
#include <euclid/core/Totp.h>

using Euclid::CLI::OneLoginClient;
using Euclid::CLI::OneLoginConfiguration;
using Euclid::CLI::OneLoginError;
using Euclid::Core::Totp;

// The two parts of the unattended OneLogin login that can be checked without an account: the
// one-time codes it computes, and what it makes of the shapes OneLogin answers in.

namespace {

    // RFC 6238's test secret: the ASCII digits "12345678901234567890", base32-encoded, which is
    // the form a provider hands a secret over in.
    constexpr auto kRfcSecret = "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ";

    std::chrono::seconds at(const long unixSeconds) {
        return std::chrono::seconds(unixSeconds);
    }

}// namespace

// ── one-time codes ───────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(totp_matches_the_rfc_6238_vectors) {

    // Every SHA-1 vector from the specification's own table. If these agree, a provider will agree
    // with the codes too - there is nothing else to be right about.
    BOOST_TEST(Totp::CodeAt(kRfcSecret, at(59), 8) == "94287082");
    BOOST_TEST(Totp::CodeAt(kRfcSecret, at(1111111109), 8) == "07081804");
    BOOST_TEST(Totp::CodeAt(kRfcSecret, at(1111111111), 8) == "14050471");
    BOOST_TEST(Totp::CodeAt(kRfcSecret, at(1234567890), 8) == "89005924");
    BOOST_TEST(Totp::CodeAt(kRfcSecret, at(2000000000), 8) == "69279037");

    // The last one sits in the twenty-third century, past what a system_clock time_point can hold
    // here - which is exactly why CodeAt() takes seconds rather than a time_point.
    BOOST_TEST(Totp::CodeAt(kRfcSecret, at(20000000000), 8) == "65353130");
}

BOOST_AUTO_TEST_CASE(a_code_is_six_digits_and_holds_for_its_step) {

    const auto code = Totp::CodeAt(kRfcSecret, at(1111111111));
    BOOST_TEST(code.size() == 6);
    BOOST_TEST(code == Totp::CodeAt(kRfcSecret, at(1111111111 + 9)));// same 30-second step
    BOOST_TEST(code != Totp::CodeAt(kRfcSecret, at(1111111111 + 60)));

    // And the time_point form agrees with it, for any time this machine's clock can express.
    BOOST_TEST(code == Totp::Code(kRfcSecret, std::chrono::system_clock::from_time_t(1111111111)));
}

BOOST_AUTO_TEST_CASE(a_secret_is_read_the_way_people_paste_it) {

    // Providers print secrets in groups and sometimes with padding; none of that is part of the
    // secret, and a person pasting it should not have to tidy it first.
    BOOST_TEST(Totp::Base32Decode("gezd gnbv gy3t qojq") == Totp::Base32Decode("GEZDGNBVGY3TQOJQ"));
    BOOST_TEST(Totp::Base32Decode("MFRGG===") == Totp::Base32Decode("mfrgg"));
    BOOST_CHECK_THROW(std::ignore = Totp::Base32Decode("not base32 - 1889!"), std::runtime_error);
}

// ── what OneLogin answers ────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(an_assertion_without_a_second_factor_is_read) {

    // An account with no MFA: the assertion arrives directly.
    const auto answer = boost::json::parse(R"({"status":{"type":"success","code":200},"data":"PHNhbWxwOlJlc3BvbnNlLz4="})");
    const auto parsed = OneLoginClient::ParseAssertionAnswer(answer);

    BOOST_TEST(parsed.assertion == "PHNhbWxwOlJlc3BvbnNlLz4=");
    BOOST_TEST(parsed.stateToken.empty());
}

BOOST_AUTO_TEST_CASE(a_second_factor_challenge_is_read) {

    const auto answer = boost::json::parse(R"({
        "status": {"type": "success", "code": 200, "message": "MFA is required for this user"},
        "data": [{
            "state_token": "xyz-state-token",
            "devices": [{"device_id": 1234567, "device_type": "OneLogin Protect"}],
            "callback_url": "https://api.onelogin.com/api/2/saml_assertion/verify_factor",
            "user": {"email": "jens.vogt@example.com"}
        }]
    })");
    const auto parsed = OneLoginClient::ParseAssertionAnswer(answer);

    BOOST_TEST(parsed.assertion.empty());
    BOOST_TEST(parsed.stateToken == "xyz-state-token");

    // The device ID arrives as a number, and goes back out as the string the next call wants.
    BOOST_REQUIRE(parsed.devices.size() == 1);
    BOOST_TEST(parsed.devices.front().id == "1234567");

    // Carried so that somebody being asked for a code is told where to look for it.
    BOOST_TEST(parsed.devices.front().type == "OneLogin Protect");
    BOOST_TEST(parsed.message.find("MFA is required") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(a_device_id_that_is_already_a_string_is_read_too) {

    const auto answer = boost::json::parse(R"({"data":[{"state_token":"t","devices":[{"device_id":"98765"}]}]})");
    BOOST_TEST(OneLoginClient::ParseAssertionAnswer(answer).devices.front().id == "98765");
}

BOOST_AUTO_TEST_CASE(every_enrolled_device_is_kept_in_order) {

    // The case that matters: two devices, and the one a person actually uses is not the first.
    // Taking the first silently is how a perfectly correct code gets refused, which is why all of
    // them are carried through to where the choice can be made.
    const auto answer = boost::json::parse(R"({
        "status": {"message": "MFA is required for this user"},
        "data": [{
            "state_token": "st",
            "devices": [
                {"device_id": 111, "device_type": "OneLogin Protect"},
                {"device_id": 222, "device_type": "Google Authenticator"}
            ]
        }]
    })");
    const auto parsed = OneLoginClient::ParseAssertionAnswer(answer);

    BOOST_REQUIRE(parsed.devices.size() == 2);
    BOOST_TEST(parsed.devices[0].type == "OneLogin Protect");
    BOOST_TEST(parsed.devices[1].id == "222");
    BOOST_TEST(parsed.devices[1].type == "Google Authenticator");
}

BOOST_AUTO_TEST_CASE(a_device_without_an_id_is_not_offered) {

    // Nothing can be verified against a device that cannot be named to verify_factor.
    const auto answer = boost::json::parse(R"({"data":[{"state_token":"t","devices":[{"device_type":"Unnamed"},{"device_id":7,"device_type":"Real"}]}]})");
    const auto parsed = OneLoginClient::ParseAssertionAnswer(answer);

    BOOST_REQUIRE(parsed.devices.size() == 1);
    BOOST_TEST(parsed.devices.front().id == "7");
}

BOOST_AUTO_TEST_CASE(the_verified_assertion_is_read) {

    // What verify_factor answers with once the code is accepted.
    const auto answer = boost::json::parse(R"({"status":{"type":"success"},"data":"PHNhbWw+YXNzZXJ0aW9uPC9zYW1sPg=="})");
    BOOST_TEST(OneLoginClient::ParseAssertionAnswer(answer).assertion == "PHNhbWw+YXNzZXJ0aW9uPC9zYW1sPg==");
}

BOOST_AUTO_TEST_CASE(an_answer_with_neither_is_refused_with_what_it_said) {

    const auto answer = boost::json::parse(R"({"status":{"type":"bad request","message":"Authentication Failed: Invalid user credentials"},"data":[]})");

    try {
        std::ignore = OneLoginClient::ParseAssertionAnswer(answer);
        BOOST_FAIL("an answer carrying neither an assertion nor a challenge should be refused");
    } catch (const OneLoginError &e) {
        BOOST_TEST(std::string(e.what()).find("Invalid user credentials") != std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(an_answer_in_no_known_shape_is_refused) {

    BOOST_CHECK_THROW(std::ignore = OneLoginClient::ParseAssertionAnswer(boost::json::parse("[1,2,3]")), OneLoginError);
}

// ── configuration ────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(the_only_application_needs_no_name) {

    OneLoginConfiguration config;
    config.applications = {{"default", "111111"}};

    BOOST_TEST(config.ApplicationId() == "111111");
}

BOOST_AUTO_TEST_CASE(one_of_several_applications_is_chosen_by_name) {

    OneLoginConfiguration config;
    config.applications = {{"int", "111111"}, {"prod", "222222"}};

    BOOST_TEST(config.ApplicationId().empty());// ambiguous, and not guessed at

    config.application = "prod";
    BOOST_TEST(config.ApplicationId() == "222222");

    config.application = "staging";
    BOOST_TEST(config.ApplicationId().empty());
}

BOOST_AUTO_TEST_CASE(an_incomplete_configuration_names_what_is_missing) {

    const OneLoginConfiguration empty;
    BOOST_TEST(empty.Validate().size() == 5);// subdomain, client id, client secret, user, app id

    OneLoginConfiguration config;
    config.subDomain = "libri-gmbh";
    config.clientId = "id";
    config.clientSecret = "secret";
    config.user = "jens.vogt@example.com";
    config.applications = {{"int", "111111"}, {"prod", "222222"}};

    // Everything is there, but which of the two applications is meant is not.
    const auto problems = config.Validate();
    BOOST_TEST(problems.size() == 1);
    BOOST_TEST(problems.front().find("--application") != std::string::npos);

    config.application = "int";
    BOOST_TEST(config.Validate().empty());
}
