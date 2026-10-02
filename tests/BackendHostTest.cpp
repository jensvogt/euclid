// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE BackendHostTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <string>

// Euclid includes
#include <Backends.h>
#include <euclid/database/Database.h>
#include <euclid/database/RepositoryFactory.h>
#include <euclid/database/entity/eap/Application.h>
#include <euclid/database/entity/emm/Module.h>

using Euclid::EAG::ApplicationRef;
using Euclid::EAG::Backend;
using Euclid::EAG::Backends;
using Euclid::Database::Entity::ModuleInstance;
using Euclid::Database::Entity::ModuleState;

// A backend used to be a port. That was enough while every instance was on the machine the gateway
// runs on, which is what ProxyServer assumed when it built every endpoint as 127.0.0.1.
//
// Once an instance record says which host it is on, the port alone is not only insufficient but
// actively wrong: two instances on two machines can hold the same number, and a gateway resolving
// the record of one against its own loopback reaches the other - or nothing, or something else
// entirely that happens to be listening.
//
// What these pin: a backend carries where it is, an empty host still means this machine so a
// single-host installation is unchanged, and an instance whose host cannot be resolved is left out
// of the rotation rather than handed to a request.

namespace {

    constexpr auto kAccount = "000000000000";
    constexpr auto kNamespace = "production";
    constexpr auto kApplication = "billing";

    ApplicationRef reference() {
        return ApplicationRef{.accountId = kAccount, .nameSpace = kNamespace, .applicationId = kApplication};
    }

    // An application the gateway can resolve, and the module the manager records its pool under.
    std::string seedApplication() {

        Euclid::Database::Entity::EAP::Application application;
        application.accountId = kAccount;
        application.nameSpace = kNamespace;
        application.applicationId = kApplication;
        application.runtimeName = "billing-000000000000-production";

        const auto stored = Euclid::Database::RepositoryFactory::instance().eapRepository()->upsertApplication(application);
        return RuntimeName(stored);
    }

    void seedInstance(const std::string &runtimeName, const std::string &instanceId,
                      const std::string &host, const int port,
                      const ModuleState state = ModuleState::RUNNING) {

        Euclid::Database::Entity::Module module;
        module.name = runtimeName;
        module.executable = "/usr/local/euclid/bin/java";

        ModuleInstance instance;
        instance.instanceId = instanceId;
        instance.pid = 4242;
        instance.host = host;
        instance.httpPort = port;
        instance.state = state;

        Euclid::Database::RepositoryFactory::instance().emmRepository()->upsertInstance(module, instance);
    }

    void freshDatabase() {
        Euclid::Database::Database::instance().initializeMemory();
    }

}// namespace

// ── The backend itself ──────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(AnUnhostedBackendIsStillLoopback) {

    // What every backend was before instances carried a host, and what every backend on a
    // single-host installation still is. The Host header has to read exactly as it did.
    const Backend backend{.host = {}, .address = boost::asio::ip::make_address("127.0.0.1"), .port = 9000};

    BOOST_TEST(backend.authority() == "127.0.0.1:9000");
}

BOOST_AUTO_TEST_CASE(AHostedBackendIsNamedByItsHost) {

    // Not the resolved address: an application that serves more than one name, or that builds an
    // absolute URL out of the Host header, needs the name it was actually called by.
    const Backend backend{.host = "node-b", .address = boost::asio::ip::make_address("127.0.0.2"), .port = 9000};

    BOOST_TEST(backend.authority() == "node-b:9000");
}

BOOST_AUTO_TEST_CASE(TheGatewaysOwnBackendIsLoopback) {

    // euclid's own gateway is the one backend that is loopback by construction rather than by
    // where it happened to be started, which is why proxyToTls never had to change.
    const auto backend = Backend::loopback(8080);

    BOOST_TEST(backend.host.empty());
    BOOST_TEST(backend.address.to_string() == "127.0.0.1");
    BOOST_TEST(backend.port == 8080);
    BOOST_TEST(backend.authority() == "127.0.0.1:8080");
}

// ── What refresh makes of an instance record ────────────────────────────────

BOOST_AUTO_TEST_CASE(AnInstanceWithNoHostResolvesToLoopback) {

    // The upgrade case, and the single-host case. Deliberately never reaches a resolver: a broken
    // name service must not be able to take away the backends of an installation that never named
    // a host in the first place.
    freshDatabase();
    const auto runtimeName = seedApplication();
    seedInstance(runtimeName, "i-1", "", 9000);

    Backends backends;
    backends.refresh({reference()});

    const auto backend = backends.next(reference());
    BOOST_TEST_REQUIRE(backend.has_value());
    BOOST_TEST(backend->host.empty());
    BOOST_TEST(backend->address.to_string() == "127.0.0.1");
    BOOST_TEST(backend->port == 9000);
}

BOOST_AUTO_TEST_CASE(AnInstanceOnAnotherAddressIsReachedThere) {

    // Step 2's whole point, and the shape the proposal suggests testing it in: a backend that is
    // not loopback, on a second address of this machine, so it needs no second machine to prove.
    freshDatabase();
    const auto runtimeName = seedApplication();
    seedInstance(runtimeName, "i-1", "127.0.0.2", 9000);

    Backends backends;
    backends.refresh({reference()});

    const auto backend = backends.next(reference());
    BOOST_TEST_REQUIRE(backend.has_value());
    BOOST_TEST(backend->address.to_string() == "127.0.0.2");
    BOOST_TEST(backend->authority() == "127.0.0.2:9000");
}

BOOST_AUTO_TEST_CASE(TwoHostsOnTheSamePortAreTwoBackends) {

    // The collision that made this necessary. Both instances hold port 9000 and are distinguished
    // only by their host; a gateway that kept ports alone would have had one backend listed twice
    // and would have sent both halves of the rotation to the same process.
    freshDatabase();
    const auto runtimeName = seedApplication();
    seedInstance(runtimeName, "i-1", "127.0.0.1", 9000);
    seedInstance(runtimeName, "i-2", "127.0.0.2", 9000);

    Backends backends;
    backends.refresh({reference()});

    BOOST_TEST_REQUIRE(backends.count(reference()) == 2U);

    const auto first = backends.next(reference());
    const auto second = backends.next(reference());
    BOOST_TEST_REQUIRE(first.has_value());
    BOOST_TEST_REQUIRE(second.has_value());

    BOOST_TEST(first->port == second->port);
    BOOST_TEST(first->address.to_string() != second->address.to_string(),
               "the rotation handed out the same machine twice");
}

BOOST_AUTO_TEST_CASE(AnInstanceWhoseHostCannotBeResolvedIsNotRoutedTo) {

    // Left out exactly as a portless instance is. Sending a request to a host that does not
    // resolve fails in a way that reads as the application being broken, and the cause - one
    // manager recording a name this machine cannot look up - is nowhere in that failure.
    freshDatabase();
    const auto runtimeName = seedApplication();
    seedInstance(runtimeName, "i-1", "no-such-host.invalid", 9000);

    Backends backends;
    backends.refresh({reference()});

    BOOST_TEST(backends.count(reference()) == 0U);
    BOOST_TEST(!backends.next(reference()).has_value());
}

BOOST_AUTO_TEST_CASE(AnInstanceThatIsNotRunningIsNotRoutedTo) {

    // Unchanged by any of this, and worth holding: the host check is an addition to the state
    // check, not a replacement for it.
    freshDatabase();
    const auto runtimeName = seedApplication();
    seedInstance(runtimeName, "i-1", "127.0.0.2", 9000, ModuleState::STOPPED);

    Backends backends;
    backends.refresh({reference()});

    BOOST_TEST(backends.count(reference()) == 0U);
}

BOOST_AUTO_TEST_CASE(AnInstanceWithNoPortIsNotRoutedTo) {

    freshDatabase();
    const auto runtimeName = seedApplication();
    seedInstance(runtimeName, "i-1", "127.0.0.2", 0);

    Backends backends;
    backends.refresh({reference()});

    BOOST_TEST(backends.count(reference()) == 0U);
}
