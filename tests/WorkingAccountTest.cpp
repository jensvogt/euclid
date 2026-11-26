// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE WorkingAccountTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <optional>
#include <string>

// Euclid includes
#include <euclid/database/RepositoryFactory.h>

using Euclid::Database::InWorkingAccount;
using Euclid::Database::Entity::EAM::User;

// A user whose record lives in 000000000000 and who logged into 408066031601 was shown the queues
// and applications of 000000000000: the handlers scoped their listings by User::accountId, the
// home account, rather than by the account the session named. InWorkingAccount() is what every
// handler's caller now goes through.

namespace {

    constexpr auto kHomeAccount = "000000000000";
    constexpr auto kOtherAccount = "408066031601";

    User userInHomeAccount() {
        User user;
        user.userId = "jens";
        user.accountId = kHomeAccount;
        return user;
    }

    boost::beast::http::request<boost::beast::http::string_body> requestFor(const std::string &accountId) {
        boost::beast::http::request<boost::beast::http::string_body> req;
        if (!accountId.empty()) req.set("x-euclid-account-id", accountId);
        return req;
    }

}// namespace

BOOST_AUTO_TEST_CASE(TheAccountTheSessionNamesIsTheOneWorkedIn) {
    const auto user = InWorkingAccount(userInHomeAccount(), requestFor(kOtherAccount));
    BOOST_CHECK_EQUAL(user.accountId, kOtherAccount);
    BOOST_CHECK_EQUAL(user.userId, "jens");
}

BOOST_AUTO_TEST_CASE(NoAccountNamedMeansTheHomeAccount) {
    const auto user = InWorkingAccount(userInHomeAccount(), requestFor(""));
    BOOST_CHECK_EQUAL(user.accountId, kHomeAccount);
}

BOOST_AUTO_TEST_CASE(TheHomeAccountNamedIsTheHomeAccount) {
    const auto user = InWorkingAccount(userInHomeAccount(), requestFor(kHomeAccount));
    BOOST_CHECK_EQUAL(user.accountId, kHomeAccount);
}

BOOST_AUTO_TEST_CASE(AnUnknownCallerStaysUnknown) {
    const auto user = InWorkingAccount(std::optional<User>{}, requestFor(kOtherAccount));
    BOOST_CHECK(!user.has_value());
}

BOOST_AUTO_TEST_CASE(AFoundCallerIsMovedToo) {
    const auto user = InWorkingAccount(std::optional{userInHomeAccount()}, requestFor(kOtherAccount));
    BOOST_REQUIRE(user.has_value());
    BOOST_CHECK_EQUAL(user->accountId, kOtherAccount);
}
