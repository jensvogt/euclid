// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE ApplicationIdentityTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <chrono>
#include <string>

// Euclid includes
#include <euclid/core/ErnUtils.h>
#include <euclid/database/Database.h>
#include <euclid/database/RepositoryFactory.h>
#include <euclid/database/entity/eap/ApplicationIdentity.h>

using Euclid::Database::Entity::EAM::AccessKey;
using Euclid::Database::Entity::EAM::Grant;
using Euclid::Database::Entity::EAM::User;
using Euclid::Database::Entity::EAP::Application;
using Euclid::Database::Entity::EAP::ApplicationEnvironment;
using Euclid::Database::Entity::EAP::ApplicationNamespace;

// What an application is told about itself, from its definition. Asked by the manager for what it
// runs and by EAP for what a worker runs, so these hold on every host - which is the reason they
// were moved out of the manager (docs/worker-nodes.md §13).

namespace {

    constexpr auto kAccount = "000000000000";

    void freshInstallation() { Euclid::Database::Database::instance().initializeMemory(); }

    void userOf(const std::string &userId, const bool withKey) {
        User user;
        user.userId = userId;
        user.accountId = kAccount;
        user.region = "eu-central-1";
        user.ern = Euclid::Core::createEamUserErn(kAccount, userId);
        user.email = userId + "@example.invalid";
        user.loginEnabled = true;
        if (withKey) user.accessKeys.push_back(AccessKey{.accessKeyId = "AKIA-" + userId, .secretAccessKey = "secret", .active = true});
        std::ignore = Euclid::Database::RepositoryFactory::instance().eamRepository()->upsertUser(user);
    }

    void grant(const std::string &userId, const std::vector<std::string> &namespaces) {
        Grant grant{.role = "application", .principal = Euclid::Core::createEamUserErn(kAccount, userId), .accountId = kAccount,
                    .namespaces = namespaces, .resources = {"*"}, .granted = std::chrono::system_clock::now(), .grantedBy = "test"};
        std::ignore = Euclid::Database::RepositoryFactory::instance().eamRepository()->addGrant(grant);
    }

    Application applicationOf(const std::string &userId) {
        Application application;
        application.applicationId = "orders";
        application.accountId = kAccount;
        application.region = "eu-central-1";
        application.userId = userId;
        application.version = "1.4.0";
        application.environment = {{"SPRING_PROFILES_ACTIVE", "prod"}, {"EUCLID_APPLICATION_ID", "spoofed"}};
        return application;
    }

}// namespace

BOOST_AUTO_TEST_CASE(TheDefinitionsOwnEnvironmentCannotShadowEuclids) {

    freshInstallation();
    userOf("app-orders", false);

    const auto environment = ApplicationEnvironment(applicationOf("app-orders"), true);
    BOOST_TEST(environment.at("SPRING_PROFILES_ACTIVE") == "prod");
    BOOST_TEST(environment.at("EUCLID_APPLICATION_ID") == "orders");
    BOOST_TEST(environment.at("EUCLID_APPLICATION_VERSION") == "1.4.0");
    BOOST_TEST(environment.at("EUCLID_USER_ID") == "app-orders");
}

BOOST_AUTO_TEST_CASE(AnOperatorsAccessKeyIsPassedOnOnlyWhereAsked) {

    // The manager passes it, as it always has; EAP does not, so it never travels to a node.
    freshInstallation();
    userOf("jens", true);

    BOOST_TEST(ApplicationEnvironment(applicationOf("jens"), true).at("EUCLID_ACCESS_KEY_ID") == "AKIA-jens");
    BOOST_TEST(!ApplicationEnvironment(applicationOf("jens"), false).contains("EUCLID_ACCESS_KEY_ID"));
    BOOST_TEST(!ApplicationEnvironment(applicationOf("jens"), false).contains("EUCLID_SECRET_ACCESS_KEY"));
}

BOOST_AUTO_TEST_CASE(HostSpecificVariablesAreNotTheDefinitions) {

    // The gateway, the certificate and the credentials file differ by host; the host adds them.
    freshInstallation();
    userOf("app-orders", false);

    const auto environment = ApplicationEnvironment(applicationOf("app-orders"), false);
    BOOST_TEST(!environment.contains("EUCLID_ENDPOINT"));
    BOOST_TEST(!environment.contains("EUCLID_CA_CERT_PATH"));
    BOOST_TEST(!environment.contains("EUCLID_CREDENTIALS_FILE"));
}

BOOST_AUTO_TEST_CASE(ANamespaceIsTheDefinitionsWhenItHasOne) {

    freshInstallation();
    userOf("app-orders", false);
    grant("app-orders", {"development"});

    auto application = applicationOf("app-orders");
    application.nameSpace = "production";
    BOOST_TEST(ApplicationNamespace(application) == "production");
}

BOOST_AUTO_TEST_CASE(OtherwiseTheOneNamespaceItsUserIsGranted) {

    freshInstallation();
    userOf("app-orders", false);
    grant("app-orders", {"development"});

    BOOST_TEST(ApplicationNamespace(applicationOf("app-orders")) == "development");
}

BOOST_AUTO_TEST_CASE(SeveralOrAWildcardIsNoAnswer) {

    freshInstallation();
    userOf("app-many", false);
    grant("app-many", {"development", "production"});
    userOf("app-all", false);
    grant("app-all", {"*"});

    BOOST_TEST(ApplicationNamespace(applicationOf("app-many")).empty());
    BOOST_TEST(ApplicationNamespace(applicationOf("app-all")).empty());
    BOOST_TEST(ApplicationNamespace(applicationOf("nobody")).empty());
}
