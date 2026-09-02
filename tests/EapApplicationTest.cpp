#define BOOST_TEST_MODULE EapApplicationTest
#include <boost/test/unit_test.hpp>

// Euclid includes
#include <euclid/database/entity/eam/User.h>
#include <euclid/database/entity/eap/Application.h>
#include <euclid/database/repository/eap/MemoryEapRepository.h>

using Euclid::Database::MemoryEapRepository;
using Euclid::Database::Entity::EAP::Application;
using Euclid::Database::Entity::EAP::ApplicationState;
using Euclid::Database::Entity::EAP::RedeployRefusal;
using Euclid::Database::Entity::EAP::Runtime;
using Euclid::Database::Entity::EAP::RuntimeCommandPrefix;
using Euclid::Database::Entity::EAP::VersionFromArtifactName;

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
        application.artifactKey = "orders/orders-1.4.0.jar";
        application.version = "1.4.0";
        application.md5Sum = "0dc7cdef5e707bae7f7b6bbb5be4c32a";
        application.arguments = {"--profile", "production"};
        application.environment = {{"JAVA_TOOL_OPTIONS", "-Xmx512m"}, {"ORDERS_MODE", "batch"}};
        application.resources = {"ern:esm:eu-central-1:000000000000:development:bucket:inbox",
                                 "ern:eqs:eu-central-1:000000000000:development:queue:inbox-queue"};
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
    BOOST_TEST(restored.artifactKey == "orders/orders-1.4.0.jar");
    // Which build is deployed, and which bytes it is. Without these the definition cannot say
    // what is running, and a redeploy has nothing to compare against.
    BOOST_TEST(restored.version == "1.4.0");
    BOOST_TEST(restored.md5Sum == "0dc7cdef5e707bae7f7b6bbb5be4c32a");
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

    // What the application may touch. Mirrored onto its principal's grants, so losing it here
    // would widen an application rather than break it - which is the failure worth a test.
    BOOST_TEST_REQUIRE(restored.resources.size() == 2U);
    BOOST_TEST(restored.resources[0] == "ern:esm:eu-central-1:000000000000:development:bucket:inbox");
    BOOST_TEST(restored.resources[1] == "ern:eqs:eu-central-1:000000000000:development:queue:inbox-queue");
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

BOOST_AUTO_TEST_CASE(TechnicalPrincipalIsAnIdentityThatCannotLogIn) {
    // The identity an application runs as: created by EAP alongside the application, no password,
    // no way in through eam login, one access key to sign its calls with. The flag is what the
    // login handler refuses on, so it has to survive the round trip - a technical principal that
    // read back as login-enabled would be a person-shaped account with an empty password.
    Euclid::Database::Entity::EAM::AccessKey key;
    key.accessKeyId = "AKIAEXAMPLE";
    key.secretAccessKey = "topsecret";
    key.active = true;

    Euclid::Database::Entity::EAM::User principal;
    principal.userId = "app-orders";
    principal.accountId = "000000000000";
    principal.region = "eu-central-1";
    principal.loginEnabled = false;
    principal.accessKeys.push_back(key);

    // Its own account, and nothing else: without a grant the principal would authenticate fine
    // and then be refused by every module's GrantLookup, which checks the account a request names
    // against the grants its caller holds.
    Euclid::Database::Entity::EAM::AccountGrant grant;
    grant.accountId = "000000000000";
    grant.namespaces = {"development"};
    principal.accountGrants.push_back(grant);

    principal.resourceGrants = {"ern:esm:eu-central-1:000000000000:development:bucket:inbox"};

    const auto restored = Euclid::Database::Entity::EAM::User::fromDocument(principal.toDocument().view());
    BOOST_TEST(restored.userId == "app-orders");
    BOOST_TEST(restored.resourceGrants == (std::vector<std::string>{"ern:esm:eu-central-1:000000000000:development:bucket:inbox"}));
    BOOST_TEST(!restored.loginEnabled);
    BOOST_TEST(restored.password.empty());
    BOOST_TEST_REQUIRE(restored.accessKeys.size() == 1U);
    BOOST_TEST(restored.accessKeys[0].accessKeyId == "AKIAEXAMPLE");

    BOOST_TEST_REQUIRE(restored.accountGrants.size() == 1U);
    BOOST_TEST(restored.accountGrants[0].accountId == "000000000000");
    BOOST_TEST(!restored.accountGrants[0].isAdmin);
    BOOST_TEST(restored.accountGrants[0].namespaces == (std::vector<std::string>{"development"}));
}

BOOST_AUTO_TEST_CASE(UsersWrittenBeforeTheFlagExistedCanStillLogIn) {
    // Every human already in the database predates loginEnabled, and their documents have no such
    // field. Defaulting to true is what keeps them able to log in after this upgrade.
    const Euclid::Database::Entity::EAM::User user;
    BOOST_TEST(user.loginEnabled);
    // And are unrestricted: an empty grant list means no resource restriction, so nobody who
    // predates this becomes unable to reach their own buckets.
    BOOST_TEST(user.resourceGrants.empty());
}

BOOST_AUTO_TEST_CASE(TwoTechnicalPrincipalsCanCoexist) {
    // eam_user carries a unique index on email. It is sparse, but sparse only skips documents
    // with no email field at all - an empty string is a value, so a second principal storing ""
    // collides with the first. Every principal therefore gets an address of its own, under the
    // domain RFC 2606 reserves for names that must never resolve.
    const auto address = [](const std::string &applicationId) { return "app-" + applicationId + "@euclid.invalid"; };

    BOOST_TEST(address("inbox") != address("demo"));
    BOOST_TEST(address("inbox") == "app-inbox@euclid.invalid");
    BOOST_TEST(!address("inbox").empty());

    // And it survives the round trip, since it is the stored value the index is built on.
    Euclid::Database::Entity::EAM::User principal;
    principal.userId = "app-inbox";
    principal.email = address("inbox");
    principal.loginEnabled = false;

    const auto restored = Euclid::Database::Entity::EAM::User::fromDocument(principal.toDocument().view());
    BOOST_TEST(restored.email == "app-inbox@euclid.invalid");
}

BOOST_AUTO_TEST_CASE(AVersionIsReadOutOfTheArtifactName) {
    // The ordinary case: builds carry their version in their own name, so nobody has to repeat it.
    BOOST_TEST(VersionFromArtifactName("orders-1.4.0.jar") == "1.4.0");
    BOOST_TEST(VersionFromArtifactName("apps/file-copy-service-2.0.11-SNAPSHOT.jar") == "2.0.11");
    BOOST_TEST(VersionFromArtifactName("euclid_app-0.9.13.py") == "0.9.13");

    // And the cases where it has to be asked for instead of guessed: no version at all, and the
    // two-component kind that is not one.
    BOOST_TEST(VersionFromArtifactName("orders.jar").empty());
    BOOST_TEST(VersionFromArtifactName("orders-1.4.jar").empty());
}

BOOST_AUTO_TEST_CASE(ARedeployHasToBeANewBuild) {
    const std::string deployedVersion = "1.4.0";
    const std::string deployedMd5 = "0dc7cdef5e707bae7f7b6bbb5be4c32a";
    const std::string otherMd5 = "655d7ed7f70afed3e3e437b71f992611";

    // A new version carrying new bytes: the only thing a deployment is supposed to be.
    BOOST_TEST(RedeployRefusal(deployedVersion, deployedMd5, "1.5.0", otherMd5).empty());

    // New bytes under the version already running - afterwards nothing can say which build is up.
    BOOST_TEST(!RedeployRefusal(deployedVersion, deployedMd5, "1.4.0", otherMd5).empty());

    // A version bump that ships the build already deployed: the restart would change nothing.
    BOOST_TEST(!RedeployRefusal(deployedVersion, deployedMd5, "1.5.0", deployedMd5).empty());

    // An application defined before versions existed carries neither, and its first redeploy is
    // what fills them in - refusing it would leave it with no way forward at all.
    BOOST_TEST(RedeployRefusal("", "", "1.0.0", otherMd5).empty());

    // The reasons are what an operator is shown, so they have to name what is actually wrong.
    BOOST_TEST(RedeployRefusal(deployedVersion, deployedMd5, "1.4.0", otherMd5).find("1.4.0") != std::string::npos);
    BOOST_TEST(RedeployRefusal(deployedVersion, deployedMd5, "1.5.0", deployedMd5).find("byte for byte") != std::string::npos);
}
