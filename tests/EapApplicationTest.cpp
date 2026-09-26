// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE EapApplicationTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <chrono>
#include <thread>

// Euclid includes
#include <euclid/database/entity/eam/User.h>
#include <euclid/database/entity/eap/Application.h>
#include <euclid/database/Database.h>
#include <euclid/database/repository/eap/MongoEapRepository.h>

using Euclid::Database::MongoEapRepository;
using Euclid::Database::Entity::EAP::Application;
using Euclid::Database::Entity::EAP::ApplicationState;
using Euclid::Database::Entity::EAP::RedeployRefusal;
using Euclid::Database::Entity::EAP::ScaleRefusal;
using Euclid::Database::Entity::EAP::RestartRefusal;
using Euclid::Database::Entity::EAP::Runtime;
using Euclid::Database::Entity::EAP::RuntimeCommandPrefix;
using Euclid::Database::Entity::EAP::RuntimeExecutableSetting;
using Euclid::Database::Entity::EAP::RuntimeFromString;
using Euclid::Database::Entity::EAP::RuntimeToString;
using Euclid::Database::Entity::GenerateRuntimeName;
using Euclid::Database::Entity::IsSafeRuntimeName;
using Euclid::Database::Entity::IssueRuntimeName;
using Euclid::Database::Entity::EAP::RuntimeName;
using Euclid::Database::Entity::EAP::VersionFromArtifactName;

// An application definition is the whole contract between EAP, the manager and the process it
// spawns, and the manager reads it back out of MongoDB rather than being told - so every field
// has to survive the round trip. A silently dropped environment map or instance count would
// surface as an application that starts wrong, not as an error.

namespace {

    Application demoApplication() {
        Application application;
        application.applicationId = "orders";
        application.runtimeName = "orders-a3f2k9x1";
        application.ern = "ern:eap:eu-central-1:000000000000:development:application:orders";
        application.accountId = "000000000000";
        application.nameSpace = "development";
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
        application.logLevel = "warning";
        return application;
    }

}// namespace

BOOST_AUTO_TEST_CASE(ApplicationSurvivesABsonRoundTrip) {
    const auto application = demoApplication();

    const auto restored = Application::fromDocument(application.toDocument().view());

    BOOST_TEST(restored.applicationId == "orders");
    BOOST_TEST(restored.runtimeName == "orders-a3f2k9x1");
    BOOST_TEST(restored.ern == application.ern);
    BOOST_TEST(restored.accountId == "000000000000");
    BOOST_TEST(restored.nameSpace == "development");
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
    BOOST_TEST(RuntimeCommandPrefix(Runtime::JAVA21) == (std::vector<std::string>{"java21", "-jar"}));
    BOOST_TEST(RuntimeCommandPrefix(Runtime::JAVA25) == (std::vector<std::string>{"java25", "-jar"}));
    BOOST_TEST(RuntimeCommandPrefix(Runtime::PYTHON) == (std::vector<std::string>{"python3"}));
    BOOST_TEST(RuntimeCommandPrefix(Runtime::NODEJS) == (std::vector<std::string>{"node"}));
    BOOST_TEST(RuntimeCommandPrefix(Runtime::BINARY).empty());
}

BOOST_AUTO_TEST_CASE(EveryVersionedRuntimeGetsItsOwnExecutable) {

    // The point of naming a version: a host with three JDKs installed side by side has no single
    // answer to "java", and a jar built for 25 does not start on 21. If two of these ever returned
    // the same key, asking for one version would silently get the other - which is the failure
    // this was added to stop.
    BOOST_TEST(RuntimeExecutableSetting(Runtime::JAVA21) == "euclid.modules.eap.runtimes.java21");
    BOOST_TEST(RuntimeExecutableSetting(Runtime::JAVA25) == "euclid.modules.eap.runtimes.java25");
    BOOST_TEST(RuntimeExecutableSetting(Runtime::JAVA) != RuntimeExecutableSetting(Runtime::JAVA21));
    BOOST_TEST(RuntimeExecutableSetting(Runtime::JAVA21) != RuntimeExecutableSetting(Runtime::JAVA25));

    // Nothing to name for something that is its own command.
    BOOST_TEST(RuntimeExecutableSetting(Runtime::BINARY).empty());
    BOOST_TEST(RuntimeExecutableSetting(Runtime::UNKNOWN).empty());

    // A key exists for every runtime that has an interpreter, so a host can move any of them.
    for (const auto runtime: {Runtime::JAVA, Runtime::JAVA21, Runtime::JAVA25, Runtime::PYTHON, Runtime::NODEJS}) {
        BOOST_TEST_CONTEXT("runtime " << RuntimeToString(runtime)) {
            BOOST_TEST(!RuntimeExecutableSetting(runtime).empty());
        }
    }
}

BOOST_AUTO_TEST_CASE(TheRuntimeNameOnTheWireSurvivesARoundTrip) {

    // Stored as its name, so adding JAVA21 and JAVA25 must not have disturbed the ones already in
    // the database. An application written before this exists says "JAVA", and has to keep running
    // as the plain java this host has - if RuntimeFromString stopped answering for it, every Java
    // application deployed so far would come back UNKNOWN and refuse to start.
    for (const auto runtime: {Runtime::JAVA, Runtime::JAVA21, Runtime::JAVA25, Runtime::PYTHON,
                              Runtime::NODEJS, Runtime::BINARY}) {
        BOOST_TEST_CONTEXT("runtime " << RuntimeToString(runtime)) {
            BOOST_TEST((RuntimeFromString(RuntimeToString(runtime)) == runtime));
        }
    }

    // And anything else is still UNKNOWN rather than quietly one of them - "JAVA22" is a typo, not
    // a runtime, and create-application refuses it.
    BOOST_TEST((RuntimeFromString("JAVA22") == Runtime::UNKNOWN));
    BOOST_TEST((RuntimeFromString("java21") == Runtime::UNKNOWN));
}

BOOST_AUTO_TEST_CASE(RepositoryKeepsOneRowPerApplicationId) {
    Euclid::Database::Database::instance().initializeMemory();
    MongoEapRepository repository;

    auto application = demoApplication();
    std::ignore = repository.upsertApplication(application);

    // The second write is the same application, not a second one - applicationId doubles as the
    // manager's module name, so two rows would mean two process pools under one name.
    application.maxInstances = 16;
    std::ignore = repository.upsertApplication(application);

    BOOST_TEST(repository.countApplications("000000000000", "development") == 1);
    BOOST_TEST_REQUIRE(repository.applicationExists("000000000000", "development", "orders"));
    BOOST_TEST(repository.findApplicationByApplicationId("000000000000", "development", "orders")->maxInstances == 16);
    BOOST_TEST(repository.findApplicationByErn(application.ern).has_value());
    BOOST_TEST(repository.listApplications("000000000000", "development", "ord").size() == 1U);
    BOOST_TEST(repository.listApplications("000000000000", "development", "zzz").empty());

    // The same id in another namespace is another application, and is not what any of these find.
    BOOST_TEST(!repository.applicationExists("000000000000", "production", "orders"));
    BOOST_TEST(!repository.findApplicationByApplicationId("000000000000", "production", "orders").has_value());
    BOOST_TEST(repository.countApplications("000000000000", "production") == 0);

    repository.deleteApplication("000000000000", "development", "orders");
    BOOST_TEST(repository.countApplications("000000000000", "development") == 0);
}

BOOST_AUTO_TEST_CASE(TwoNamespacesMayEachDefineTheSameApplication) {
    Euclid::Database::Database::instance().initializeMemory();
    MongoEapRepository repository;

    auto development = demoApplication();
    auto production = demoApplication();
    production.nameSpace = "production";
    production.runtimeName = "orders-b7c1m2p9";
    production.ern = "ern:eap:eu-central-1:000000000000:production:application:orders";
    production.maxInstances = 3;

    std::ignore = repository.upsertApplication(development);
    std::ignore = repository.upsertApplication(production);

    // Two applications, not one overwritten by the other: the namespace is part of what identifies
    // an application, so "orders" in development and "orders" in production are different things.
    BOOST_TEST(repository.countApplications("000000000000", "development") == 1);
    BOOST_TEST(repository.countApplications("000000000000", "production") == 1);
    BOOST_TEST(repository.findApplicationByApplicationId("000000000000", "development", "orders")->maxInstances == 8);
    BOOST_TEST(repository.findApplicationByApplicationId("000000000000", "production", "orders")->maxInstances == 3);

    // The manager runs both, and it is the runtime name that keeps them apart there: one process
    // pool, one data directory, one socket and one module row each. Neither is derived from
    // anything, so neither can be made to collide by how the applications are named or moved.
    BOOST_TEST(RuntimeName(development) == "orders-a3f2k9x1");
    BOOST_TEST(RuntimeName(production) == "orders-b7c1m2p9");

    // The manager needs every application on the host, whatever namespace defines it - a
    // reconciler that saw one namespace's would tear down the rest as undefined.
    BOOST_TEST(repository.listAllApplications("").size() == 2U);
    BOOST_TEST(repository.listApplications("000000000000", "development", "").size() == 1U);

    // Deleting one leaves the other alone.
    repository.deleteApplication("000000000000", "development", "orders");
    BOOST_TEST(repository.listAllApplications("").size() == 1U);
    BOOST_TEST(repository.findApplicationByApplicationId("000000000000", "production", "orders").has_value());
}

BOOST_AUTO_TEST_CASE(AnApplicationFromBeforeRuntimeNamesKeepsRunningUnderItsOwnId) {
    // Nothing on a host moves because this field was added. An application deployed before it
    // existed has none, and it is running under its bare applicationId this minute - its
    // directory, its module row, its socket and its principal are all named that - so that is what
    // it goes on running under.
    Application legacy;
    legacy.applicationId = "orders";
    legacy.accountId = "000000000000";
    legacy.nameSpace = "development";
    BOOST_TEST(legacy.runtimeName.empty());
    BOOST_TEST(RuntimeName(legacy) == "orders");

    // And it stays absent through a round trip rather than being written as an empty name, which
    // the unique index would read as one name every such application shares.
    const auto restored = Application::fromDocument(legacy.toDocument().view());
    BOOST_TEST(restored.runtimeName.empty());
    BOOST_TEST(RuntimeName(restored) == "orders");
    BOOST_TEST(!legacy.toDocument().view().find("runtimeName").operator*());
}

BOOST_AUTO_TEST_CASE(AnIssuedRuntimeNameSaysWhichApplicationItIs) {
    // Readable, so that ps output, a module list, a log channel and a directory under the data dir
    // all still say which application they belong to...
    const auto issued = GenerateRuntimeName("orders");
    BOOST_TEST(issued.starts_with("orders-"));

    // ...and unique, which is what the suffix is for. Two applications called the same thing in
    // two namespaces are issued different names, with nothing having to coordinate that.
    BOOST_TEST(GenerateRuntimeName("orders") != GenerateRuntimeName("orders"));

    // Long enough to be worth having: 8 characters of an alphabet of 32.
    BOOST_TEST(issued.size() == std::string("orders-").size() + 8U);

    // The whole of this ends up in a unix socket path, which is capped at 108 bytes, so the id it
    // is built from is cut rather than allowed to take the name past that.
    const std::string long_id(80, 'x');
    BOOST_TEST(GenerateRuntimeName(long_id).size() == 32U + 1U + 8U);
}

BOOST_AUTO_TEST_CASE(APlainNameIsIssuedWhenNothingElseHasIt) {

    // What an operator reads off ps output, a socket path, a data directory and a log channel.
    // "protocolizing" says everything "protocolizing-5yrdi4gh" says without the noise, so the
    // plain name is taken when it is free.
    const auto free = [](const std::string &) { return false; };
    BOOST_TEST(IssueRuntimeName("protocolizing", free) == "protocolizing");
    BOOST_TEST(IssueRuntimeName("orders", free) == "orders");
}

BOOST_AUTO_TEST_CASE(ATakenNameFallsBackToASuffixedOne) {

    // Which is the case the suffix exists for: two namespaces may each define an application of
    // the same name, and a directory and a socket are per host, with nowhere to put a namespace.
    const auto onlyPlainTaken = [](const std::string &candidate) { return candidate == "orders"; };

    const auto issued = IssueRuntimeName("orders", onlyPlainTaken);
    BOOST_TEST(issued != "orders");
    BOOST_TEST(issued.starts_with("orders-"));
    BOOST_TEST(issued.size() == std::string("orders-").size() + 8U);

    // And it keeps trying rather than handing back a name somebody already has: a suffix is
    // random, so "unlikely" is not "cannot".
    int refusals = 0;
    const auto refuseTheFirstFew = [&refusals](const std::string &) { return refusals++ < 3; };
    const auto eventual = IssueRuntimeName("orders", refuseTheFirstFew);
    BOOST_TEST(eventual.starts_with("orders-"));
    BOOST_TEST(refusals == 4);
}

BOOST_AUTO_TEST_CASE(AnIssuedNameIsAlwaysOneUsablePathComponent) {

    // A runtime name is a path component before it is anything else - a directory under the data
    // dir, a unix socket - and one caller deletes that directory recursively. Anything that would
    // resolve somewhere other than one level down is refused.
    BOOST_TEST(!IsSafeRuntimeName(""));
    BOOST_TEST(!IsSafeRuntimeName("."));
    BOOST_TEST(!IsSafeRuntimeName(".."));
    BOOST_TEST(!IsSafeRuntimeName("../../etc"));
    BOOST_TEST(!IsSafeRuntimeName("a/b"));
    BOOST_TEST(!IsSafeRuntimeName("a\\b"));
    BOOST_TEST(!IsSafeRuntimeName(std::string("a\0b", 4)));
    BOOST_TEST(IsSafeRuntimeName("protocolizing"));
    BOOST_TEST(IsSafeRuntimeName("orders-a1b2c3d4"));

    // An id that is not usable on its own does not come back as itself, suffix or no suffix.
    const auto free = [](const std::string &) { return false; };
    BOOST_TEST(IssueRuntimeName("..", free) != "..");
    BOOST_TEST(IsSafeRuntimeName(IssueRuntimeName("..", free)));
}

BOOST_AUTO_TEST_CASE(APlainNameIsCutToFitASocketPathToo) {

    // The cap is on the name, not on the generated part: sun_path is 108 bytes and the whole of
    // this ends up inside one. A plain name that skipped the cut would be the long one.
    const std::string longId(80, 'x');
    const auto free = [](const std::string &) { return false; };
    BOOST_TEST(IssueRuntimeName(longId, free).size() == 32U);
}

BOOST_AUTO_TEST_CASE(ScalingOneBoundIsCheckedAgainstTheOtherAsItWillStand) {

    // The case a per-field check misses. An application running 2..8 is asked for a floor of 6:
    // fine against the stored ceiling. Asked for a floor of 12, it is not - and nothing about the
    // request itself says so, because 12 is a perfectly ordinary number. It has to be checked
    // against the ceiling it will actually sit under.
    BOOST_TEST(ScaleRefusal(6, -1, 2, 8).empty());
    BOOST_TEST(!ScaleRefusal(12, -1, 2, 8).empty());

    // The same from the other side: lowering a ceiling below the floor already in place.
    BOOST_TEST(ScaleRefusal(-1, 4, 2, 8).empty());
    BOOST_TEST(!ScaleRefusal(-1, 1, 2, 8).empty());

    // And both at once, which is the one case where the stored pair does not matter at all.
    BOOST_TEST(ScaleRefusal(10, 20, 2, 8).empty());
    BOOST_TEST(!ScaleRefusal(20, 10, 2, 8).empty());
}

BOOST_AUTO_TEST_CASE(PinningAPoolAtOneSizeIsAllowed) {

    // Equal bounds leave the autoscaler no room, which is how an application is held at a fixed
    // size on purpose. Not a refusal - an operator who wants exactly four is entitled to four.
    BOOST_TEST(ScaleRefusal(4, 4, 1, 8).empty());
    BOOST_TEST(ScaleRefusal(1, 1, 1, 8).empty());
}

BOOST_AUTO_TEST_CASE(AFloorOfZeroIsRefusedRatherThanReadAsStop) {

    // A pool desired RUNNING with a floor of zero runs nothing and reads in every listing as one
    // that has failed to start. Stopping an application is a different thing and says so in the
    // field that carries the truth about it.
    const auto refusal = ScaleRefusal(0, -1, 1, 8);
    BOOST_TEST(!refusal.empty());
    BOOST_TEST(refusal.find("stop-application") != std::string::npos);

    BOOST_TEST(!ScaleRefusal(-1, 0, 1, 8).empty());
}

BOOST_AUTO_TEST_CASE(ARequestThatNamesNeitherBoundChangesNothing) {

    // -1 is "leave alone" for both, so this asks for nothing. Refused rather than silently
    // succeeding, because the caller meant something and did not say it.
    BOOST_TEST(!ScaleRefusal(-1, -1, 1, 8).empty());
}

BOOST_AUTO_TEST_CASE(ALogLevelIsNotAChangeOfDefinition) {
    Euclid::Database::Database::instance().initializeMemory();
    MongoEapRepository repository;

    auto application = demoApplication();
    application.logLevel.clear();
    const auto stored = repository.upsertApplication(application);

    BOOST_TEST_REQUIRE(repository.setApplicationLogLevel("000000000000", "development", "orders", "off"));
    const auto changed = repository.findApplicationByApplicationId("000000000000", "development", "orders");
    BOOST_TEST_REQUIRE(changed.has_value());
    BOOST_TEST(changed->logLevel == "off");

    // The manager restarts an application whose definition changed since it started it, and it
    // decides that by comparing the modification date. Turning a log down must therefore not touch
    // it - otherwise silencing a noisy application would bounce every one of its instances, which
    // is a far worse cure than the noise.
    BOOST_TEST((changed->modified == stored.modified));

    // Everything else it was deployed with is left exactly as it was.
    BOOST_TEST(changed->maxInstances == stored.maxInstances);
    BOOST_TEST(changed->artifactKey == stored.artifactKey);

    // An empty level is how the setting is taken back, rather than a level of its own.
    BOOST_TEST_REQUIRE(repository.setApplicationLogLevel("000000000000", "development", "orders", ""));
    BOOST_TEST(repository.findApplicationByApplicationId("000000000000", "development", "orders")->logLevel.empty());

    // And an application nobody has defined is reported rather than silently accepted - including
    // one that exists, but not in the namespace the caller named.
    BOOST_TEST(!repository.setApplicationLogLevel("000000000000", "development", "nothing-of-that-name", "debug"));
    BOOST_TEST(!repository.setApplicationLogLevel("000000000000", "production", "orders", "debug"));
}

BOOST_AUTO_TEST_CASE(ARestartIsAChangeOfRevision) {
    Euclid::Database::Database::instance().initializeMemory();
    MongoEapRepository repository;

    auto application = demoApplication();
    const auto stored = repository.upsertApplication(application);

    // The revision the manager compares is the modification date as it stood when an instance was
    // started, and BSON keeps it to the millisecond - so two stamps have to be a millisecond apart
    // to be different ones. Nothing restarts an application twice within a millisecond; the test
    // does, and would otherwise be measuring the clock rather than the write.
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    BOOST_TEST_REQUIRE(repository.touchApplication("000000000000", "development", "orders"));
    const auto restarted = repository.findApplicationByApplicationId("000000000000", "development", "orders");
    BOOST_TEST_REQUIRE(restarted.has_value());

    // The whole of what a restart is: the manager stops and starts a pool whose application was
    // modified since it started it, so moving this date is the request.
    BOOST_TEST((restarted->modified > stored.modified));

    // And nothing else moves with it. What comes back has to be what was running - the same
    // artifact, the same scaling, the same log level - or a restart would be a deployment.
    BOOST_TEST((restarted->desiredState == ApplicationState::RUNNING));
    BOOST_TEST(restarted->artifactKey == stored.artifactKey);
    BOOST_TEST(restarted->md5Sum == stored.md5Sum);
    BOOST_TEST(restarted->version == stored.version);
    BOOST_TEST(restarted->minInstances == stored.minInstances);
    BOOST_TEST(restarted->maxInstances == stored.maxInstances);
    BOOST_TEST(restarted->logLevel == stored.logLevel);
    BOOST_TEST(restarted->runtimeName == stored.runtimeName);
    BOOST_TEST((restarted->created == stored.created));

    // An application nobody has defined is reported rather than silently accepted - including one
    // that exists, but not in the namespace or the account the caller named.
    BOOST_TEST(!repository.touchApplication("000000000000", "development", "nothing-of-that-name"));
    BOOST_TEST(!repository.touchApplication("000000000000", "production", "orders"));
    BOOST_TEST(!repository.touchApplication("999999999999", "development", "orders"));
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
    principal.userId = "app-orders-a3f2k9x1";
    principal.accountId = "000000000000";
    principal.region = "eu-central-1";
    principal.loginEnabled = false;
    principal.accessKeys.push_back(key);

    // What the principal may do no longer lives on the user: EAP grants it the `application` role
    // over the resources the application declared, and that is a Grant record of its own - see
    // EapServer's createTechnicalUser and RoleRepositoryTest.

    const auto restored = Euclid::Database::Entity::EAM::User::fromDocument(principal.toDocument().view());
    // Named after what the application runs as, not what it is defined as: an EAM userId is unique
    // across the installation, so a principal named for the bare id would be one principal for
    // every namespace's "orders". And because that name never changes, neither does this - an
    // application that moves namespace keeps its principal and its key.
    BOOST_TEST(restored.userId == "app-orders-a3f2k9x1");
    BOOST_TEST(restored.userId == "app-" + RuntimeName(demoApplication()));
    BOOST_TEST(!restored.loginEnabled);
    BOOST_TEST(restored.password.empty());
    BOOST_TEST_REQUIRE(restored.accessKeys.size() == 1U);
    BOOST_TEST(restored.accessKeys[0].accessKeyId == "AKIAEXAMPLE");
}

BOOST_AUTO_TEST_CASE(UsersWrittenBeforeTheFlagExistedCanStillLogIn) {
    // Every human already in the database predates loginEnabled, and their documents have no such
    // field. Defaulting to true is what keeps them able to log in after this upgrade.
    const Euclid::Database::Entity::EAM::User user;
    BOOST_TEST(user.loginEnabled);
    // And are unrestricted: an empty grant list means no resource restriction, so nobody who
    // predates this becomes unable to reach their own buckets.
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

    // A new version carrying new bytes: the plain case.
    BOOST_TEST(RedeployRefusal(deployedVersion, deployedMd5, "1.5.0", otherMd5, true).empty());

    // New bytes under the version already running - allowed. A rebuilt snapshot keeps its number,
    // and it is the bytes that make it a different build.
    BOOST_TEST(RedeployRefusal(deployedVersion, deployedMd5, "1.4.0", otherMd5, true).empty());

    // The build already deployed, whatever it is called: the restart would change nothing.
    BOOST_TEST(!RedeployRefusal(deployedVersion, deployedMd5, "1.5.0", deployedMd5, true).empty());
    BOOST_TEST(!RedeployRefusal(deployedVersion, deployedMd5, "1.4.0", deployedMd5, true).empty());

    // An application defined before versions existed carries neither, and its first redeploy is
    // what fills them in - refusing it would leave it with no way forward at all.
    BOOST_TEST(RedeployRefusal("", "", "1.0.0", otherMd5, true).empty());

    // The reason is what an operator is shown, so it has to name what is actually wrong.
    BOOST_TEST(RedeployRefusal(deployedVersion, deployedMd5, "1.5.0", deployedMd5, true).find("byte for byte") != std::string::npos);
}

// Both checksums come out of the database, and the bytes one of them was taken over can be gone -
// an ESM object is a row and a file, and the file can be removed from under the row. Refusing there
// is refusing on the strength of a hash of bytes that do not exist, at the moment the redeploy is
// most likely to be the thing putting the artifact back.
BOOST_AUTO_TEST_CASE(ARedeployIsNotRefusedOverAnArtifactThatIsNoLongerOnDisk) {
    const std::string deployedVersion = "1.4.0";
    const std::string deployedMd5 = "0dc7cdef5e707bae7f7b6bbb5be4c32a";

    // The same checksum, which is what the refusal is made of - and no bytes behind it.
    BOOST_TEST(RedeployRefusal(deployedVersion, deployedMd5, "1.4.0", deployedMd5, false).empty());
    BOOST_TEST(RedeployRefusal(deployedVersion, deployedMd5, "1.5.0", deployedMd5, false).empty());

    // And the rule is unchanged for everything else: bytes that are there are still compared, so
    // this is not a way of quietly turning the check off.
    BOOST_TEST(!RedeployRefusal(deployedVersion, deployedMd5, "1.4.0", deployedMd5, true).empty());
}

BOOST_AUTO_TEST_CASE(OnlyARunningApplicationCanBeRestarted) {
    auto application = demoApplication();

    // Restarting what should be running is the whole use: the instances go and come back.
    application.desiredState = ApplicationState::RUNNING;
    BOOST_TEST(RestartRefusal(application).empty());

    // Stopped, there is nothing to restart, and the only way to honour the request would be to
    // start it - which is precisely what somebody asked not to happen. Refused, rather than
    // quietly doing nothing: "restarted" and "still stopped" are acted on differently.
    application.desiredState = ApplicationState::STOPPED;
    BOOST_TEST(!RestartRefusal(application).empty());

    // The reason is what an operator is shown, so it names the application and what to do instead.
    const auto refusal = RestartRefusal(application);
    BOOST_TEST(refusal.find("orders") != std::string::npos);
    BOOST_TEST(refusal.find("start-application") != std::string::npos);
}
