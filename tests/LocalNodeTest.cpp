// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE LocalNodeTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

// Boost includes
#include <boost/json.hpp>

// Euclid includes
#include <LocalNode.h>
#include <euclid/core/BuiltinRoles.h>
#include <euclid/core/Configuration.h>
#include <euclid/database/Database.h>
#include <euclid/database/RepositoryFactory.h>

using Euclid::Core::Configuration;
namespace LocalNode = Euclid::EAP::LocalNode;

// The worker on the manager's own host, as EAP provisions it when it starts - docs/worker-nodes.md
// §13.3. What matters is that it works with nobody doing anything (a principal, a key, a grant, and
// two files the worker can start from), that doing it again changes nothing, and that nothing of the
// manager's own configuration reaches the worker's beyond what is listed.

namespace {

    const auto kDirectory = std::filesystem::temp_directory_path() / "euclid-local-node-test";

    // The configuration is a process-wide singleton and load() replaces it wholesale, so each case
    // states the whole of what it needs.
    void configure(const std::string &json) {
        const auto path = std::filesystem::temp_directory_path() / "euclid-local-node-test.json";
        std::ofstream(path, std::ios::trunc) << json;
        Configuration::instance().load(path);
    }

    std::string configurationWith(const bool enabled) {
        // A path in JSON, so its backslashes on Windows have to be escaped.
        auto directory = kDirectory.string();
        for (std::size_t at = 0; (at = directory.find('\\', at)) != std::string::npos; at += 2) directory.replace(at, 1, "\\\\");

        return R"({"euclid": {
            "account-ids": ["000000000000"],
            "region": "eu-central-1",
            "gateway": {"http": {"port": 6655}, "tls": {"enabled": true, "cert-file": "/usr/local/euclid/etc/euclid_cert.crt"}},
            "modules": {
              "eam": {"jwt-secret": "do-not-leak-me"},
              "eap": {
                "http-port-min": 9000, "http-port-max": 9999,
                "runtimes": {"java21": "/opt/jdk21/bin/java"},
                "local-node": {"enabled": )" + std::string(enabled ? "true" : "false") + R"(, "name": "local",
                               "dir": ")" + directory + R"(", "http-port-min": 10000, "http-port-max": 10999}}}}})";
    }

    void freshInstallation(const bool enabled) {
        std::error_code ec;
        std::filesystem::remove_all(kDirectory, ec);
        configure(configurationWith(enabled));
        Euclid::Database::Database::instance().initializeMemory();
    }

    boost::json::value readJson(const std::filesystem::path &path) {
        std::ifstream in(path);
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return boost::json::parse(buffer.str());
    }

}// namespace

BOOST_AUTO_TEST_CASE(NothingHappensUnlessItIsEnabled) {

    freshInstallation(false);
    LocalNode::Provision();

    BOOST_TEST(!Euclid::Database::RepositoryFactory::instance().eamRepository()->findUserByUserId("local-node").has_value());
    BOOST_TEST(!std::filesystem::exists(kDirectory / "credentials"));
}

BOOST_AUTO_TEST_CASE(ItIsAPrincipalThatCannotLogInHoldingOneKeyAndTheNodeRole) {

    freshInstallation(true);
    LocalNode::Provision();

    const auto repository = Euclid::Database::RepositoryFactory::instance().eamRepository();
    const auto user = repository->findUserByUserId("local-node");
    BOOST_TEST_REQUIRE(user.has_value());
    BOOST_TEST(!user->loginEnabled);
    BOOST_TEST(user->accountId == "000000000000");
    BOOST_TEST(user->accessKeys.size() == 1U);

    const auto grants = repository->findGrantsByPrincipals({user->ern});
    BOOST_TEST_REQUIRE(grants.size() == 1U);
    BOOST_TEST(grants.front().role == std::string(Euclid::Core::BuiltinRoles::Node));
}

BOOST_AUTO_TEST_CASE(TheCredentialsAreItsKeyInTheShapeALoginLeaves) {

    freshInstallation(true);
    LocalNode::Provision();

    const auto user = Euclid::Database::RepositoryFactory::instance().eamRepository()->findUserByUserId("local-node");
    const auto credentials = readJson(kDirectory / "credentials").as_object();

    // An empty token, which CLI::Credentials::Load needs to be present, and the key a worker signs with.
    BOOST_TEST(credentials.at("token").as_string().empty());
    BOOST_TEST(credentials.at("accessKeyId").as_string() == user->accessKeys.front().accessKeyId);
    BOOST_TEST(credentials.at("secretAccessKey").as_string() == user->accessKeys.front().secretAccessKey);
    BOOST_TEST(credentials.at("isAdmin").as_bool() == false);
}

BOOST_AUTO_TEST_CASE(TheWorkersConfigurationIsTheManagersViewOfThisHost) {

    freshInstallation(true);
    const auto worker = LocalNode::WorkerConfiguration(kDirectory / "credentials").at("euclid").at("worker").as_object();

    BOOST_TEST(worker.at("endpoint").as_string() == "https://localhost:6655");
    BOOST_TEST(worker.at("ca-cert").as_string() == "/usr/local/euclid/etc/euclid_cert.crt");
    BOOST_TEST(worker.at("address").as_string() == "127.0.0.1");
    BOOST_TEST(worker.at("node").as_string() == "local");
    BOOST_TEST(worker.at("labels").at("local").as_string() == "true");
    BOOST_TEST(worker.at("runtimes").at("java21").as_string() == "/opt/jdk21/bin/java");

    // A port range of its own, not the one the manager hands its own instances out of.
    BOOST_TEST(worker.at("http-port-min").as_int64() == 10000);
    BOOST_TEST(worker.at("http-port-max").as_int64() == 10999);
}

BOOST_AUTO_TEST_CASE(NoSecretOfTheManagersReachesTheWorkersFile) {

    // A worker refuses to start with the signing secret in its configuration, and is right to.
    freshInstallation(true);
    LocalNode::Provision();

    std::ifstream in(kDirectory / "euclid-wrk.json");
    std::ostringstream buffer;
    buffer << in.rdbuf();
    BOOST_TEST(buffer.str().find("jwt-secret") == std::string::npos);
    BOOST_TEST(buffer.str().find("do-not-leak-me") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(ProvisioningAgainChangesNothing) {

    // Every start of EAP does this; a second key or a second grant each time would be a leak of both.
    freshInstallation(true);
    LocalNode::Provision();
    const auto first = readJson(kDirectory / "credentials");
    LocalNode::Provision();

    const auto repository = Euclid::Database::RepositoryFactory::instance().eamRepository();
    const auto user = repository->findUserByUserId("local-node");
    BOOST_TEST(user->accessKeys.size() == 1U);
    BOOST_TEST(repository->findGrantsByPrincipals({user->ern}).size() == 1U);
    BOOST_TEST(readJson(kDirectory / "credentials") == first);
}

BOOST_AUTO_TEST_CASE(ADeactivatedKeyIsReplacedNotReused) {

    freshInstallation(true);
    LocalNode::Provision();

    const auto repository = Euclid::Database::RepositoryFactory::instance().eamRepository();
    auto user = *repository->findUserByUserId("local-node");
    user.accessKeys.front().active = false;
    std::ignore = repository->upsertUser(user);

    LocalNode::Provision();

    const auto reloaded = repository->findUserByUserId("local-node");
    BOOST_TEST_REQUIRE(reloaded->accessKeys.size() == 2U);
    BOOST_TEST(readJson(kDirectory / "credentials").at("accessKeyId").as_string() == reloaded->accessKeys.back().accessKeyId);
}
