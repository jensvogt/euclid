#define BOOST_TEST_MODULE EapApplicationTest
#include <boost/test/unit_test.hpp>

// Euclid includes
#include <euclid/database/entity/eap/Application.h>
#include <euclid/database/repository/eap/MemoryEapRepository.h>

using Euclid::Database::MemoryEapRepository;
using Euclid::Database::Entity::EAP::Application;
using Euclid::Database::Entity::EAP::ApplicationState;
using Euclid::Database::Entity::EAP::Runtime;
using Euclid::Database::Entity::EAP::RuntimeCommandPrefix;

// An application definition is the whole contract between EAP, the manager and the process it
// spawns, and the manager reads it back out of MongoDB rather than being told - so every field
// has to survive the round trip. A silently dropped environment map or instance count would
// surface as an application that starts wrong, not as an error.

namespace {

    Application demoApplication() {
        Application application;
        application.applicationId = "orders";
        application.ern = "ern:eap:eu-central-1:000000000000:application:orders";
        application.accountId = "000000000000";
        application.region = "eu-central-1";
        application.runtime = Runtime::JAVA;
        application.bucketErn = "ern:esm:eu-central-1:000000000000:development:bucket:apps";
        application.artifactKey = "orders/orders-1.4.jar";
        application.arguments = {"--profile", "production"};
        application.environment = {{"JAVA_TOOL_OPTIONS", "-Xmx512m"}, {"ORDERS_MODE", "batch"}};
        application.userId = "appuser";
        application.minInstances = 2;
        application.maxInstances = 8;
        application.readyTimeoutMs = 45000;
        application.desiredState = ApplicationState::RUNNING;
        return application;
    }

}// namespace

BOOST_AUTO_TEST_CASE(ApplicationSurvivesABsonRoundTrip) {
    const auto application = demoApplication();

    const auto restored = Application::fromDocument(application.toDocument().view());

    BOOST_TEST(restored.applicationId == "orders");
    BOOST_TEST(restored.ern == application.ern);
    BOOST_TEST(restored.accountId == "000000000000");
    BOOST_TEST(restored.region == "eu-central-1");
    BOOST_TEST((restored.runtime == Runtime::JAVA));
    BOOST_TEST(restored.bucketErn == application.bucketErn);
    BOOST_TEST(restored.artifactKey == "orders/orders-1.4.jar");
    BOOST_TEST(restored.userId == "appuser");
    BOOST_TEST(restored.minInstances == 2);
    BOOST_TEST(restored.maxInstances == 8);
    BOOST_TEST(restored.readyTimeoutMs == 45000);
    BOOST_TEST((restored.desiredState == ApplicationState::RUNNING));

    BOOST_TEST_REQUIRE(restored.arguments.size() == 2U);
    BOOST_TEST(restored.arguments[1] == "production");

    BOOST_TEST_REQUIRE(restored.environment.size() == 2U);
    BOOST_TEST(restored.environment.at("JAVA_TOOL_OPTIONS") == "-Xmx512m");
    BOOST_TEST(restored.environment.at("ORDERS_MODE") == "batch");
}

BOOST_AUTO_TEST_CASE(AnEmptyApplicationRoundTripsToo) {
    // Definitions written before a field existed have no such field at all, and must still read.
    const auto restored = Application::fromDocument(Application{}.toDocument().view());
    BOOST_TEST(restored.arguments.empty());
    BOOST_TEST(restored.environment.empty());
    BOOST_TEST((restored.desiredState == ApplicationState::STOPPED));
}

BOOST_AUTO_TEST_CASE(RuntimeDecidesTheCommandPrefix) {
    // What the manager execs: an interpreter with the artifact as its argument, or - for a
    // compiled binary - the artifact itself, which is why BINARY has no prefix at all.
    BOOST_TEST(RuntimeCommandPrefix(Runtime::JAVA) == (std::vector<std::string>{"java", "-jar"}));
    BOOST_TEST(RuntimeCommandPrefix(Runtime::PYTHON) == (std::vector<std::string>{"python3"}));
    BOOST_TEST(RuntimeCommandPrefix(Runtime::NODEJS) == (std::vector<std::string>{"node"}));
    BOOST_TEST(RuntimeCommandPrefix(Runtime::BINARY).empty());
}

BOOST_AUTO_TEST_CASE(RepositoryKeepsOneRowPerApplicationId) {
    MemoryEapRepository repository;

    auto application = demoApplication();
    std::ignore = repository.upsertApplication(application);

    // The second write is the same application, not a second one - applicationId doubles as the
    // manager's module name, so two rows would mean two process pools under one name.
    application.maxInstances = 16;
    std::ignore = repository.upsertApplication(application);

    BOOST_TEST(repository.countApplications() == 1);
    BOOST_TEST_REQUIRE(repository.applicationExists("orders"));
    BOOST_TEST(repository.findApplicationByApplicationId("orders")->maxInstances == 16);
    BOOST_TEST(repository.findApplicationByErn(application.ern).has_value());
    BOOST_TEST(repository.listApplications("ord").size() == 1U);
    BOOST_TEST(repository.listApplications("zzz").empty());

    repository.deleteApplication("orders");
    BOOST_TEST(repository.countApplications() == 0);
}
