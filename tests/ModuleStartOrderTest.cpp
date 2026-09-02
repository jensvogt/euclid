#define BOOST_TEST_MODULE ModuleStartOrderTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <algorithm>
#include <map>
#include <string>
#include <vector>

// Euclid includes
#include <euclid/manager/StartOrder.h>

using Euclid::main::topologicalStartOrder;

// The order modules are started in used to be whatever order the manager happened to hold them
// in - alphabetical, because they live in a std::map keyed by name - and a module that called
// another during its own start-up was relying on the alphabet. These cover the rule that replaced
// it, including the two cases nobody can check by starting an installation and looking at it: a
// dependency naming something that is not there, and a cycle.

namespace {

    using Dependencies = std::map<std::string, std::vector<std::string> >;

    // Where a name ended up, so the assertions can talk about "before" rather than exact indices -
    // any order satisfying the dependencies is a correct answer.
    std::size_t positionOf(const std::vector<std::string> &order, const std::string &name) {
        const auto it = std::ranges::find(order, name);
        BOOST_TEST_REQUIRE((it != order.end()), "'" << name << "' is missing from the start order");
        return static_cast<std::size_t>(std::distance(order.begin(), it));
    }

}// namespace

BOOST_AUTO_TEST_CASE(EverythingRegisteredIsStartedExactlyOnce) {
    const Dependencies dependencies = {{"eam", {}}, {"eap", {"ees", "esm"}}, {"ees", {}}, {"esm", {"eam"}}};

    auto order = topologicalStartOrder(dependencies);

    BOOST_TEST_REQUIRE(order.size() == 4U);
    std::ranges::sort(order);
    BOOST_TEST(order == (std::vector<std::string>{"eam", "eap", "ees", "esm"}));
}

BOOST_AUTO_TEST_CASE(ADependencyStartsBeforeWhatDependsOnIt) {
    const Dependencies dependencies = {{"eam", {}}, {"eap", {"ees", "esm"}}, {"ees", {}}, {"esm", {"eam"}}};

    const auto order = topologicalStartOrder(dependencies);

    BOOST_TEST(positionOf(order, "ees") < positionOf(order, "eap"));
    BOOST_TEST(positionOf(order, "esm") < positionOf(order, "eap"));
    // Transitively too: eap needs esm, and esm needs eam.
    BOOST_TEST(positionOf(order, "eam") < positionOf(order, "esm"));
    BOOST_TEST(positionOf(order, "eam") < positionOf(order, "eap"));
}

BOOST_AUTO_TEST_CASE(AnInstallationDeclaringNothingKeepsTheOrderItAlwaysHad) {
    // The upgrade path: a configuration with no dependencies at all has to start exactly as it
    // did before, or this change would reorder every existing installation silently.
    const Dependencies dependencies = {{"eam", {}}, {"eap", {}}, {"ees", {}}, {"ekm", {}}, {"esm", {}}};

    const auto order = topologicalStartOrder(dependencies);

    BOOST_TEST(order == (std::vector<std::string>{"eam", "eap", "ees", "ekm", "esm"}));
}

BOOST_AUTO_TEST_CASE(ADependencyOnSomethingUnregisteredIsIgnored) {
    // Inactive in euclid.json, or misspelled. Waiting for it would mean never starting, which is
    // a worse answer than starting without it and saying so in the log.
    const Dependencies dependencies = {{"eap", {"nosuchmodule"}}, {"ees", {}}};

    const auto order = topologicalStartOrder(dependencies);

    BOOST_TEST_REQUIRE(order.size() == 2U);
    BOOST_TEST(std::ranges::contains(order, "eap"));
    BOOST_TEST(!std::ranges::contains(order, "nosuchmodule"));
}

BOOST_AUTO_TEST_CASE(ACycleStillStartsEverything) {
    // Nothing can satisfy a -> b -> a, so the point is only that the manager reports it and
    // starts both anyway. Refusing to start half an installation over a configuration mistake
    // would turn a misconfiguration into an outage.
    const Dependencies dependencies = {{"a", {"b"}}, {"b", {"a"}}, {"c", {}}};

    auto order = topologicalStartOrder(dependencies);

    BOOST_TEST_REQUIRE(order.size() == 3U);
    std::ranges::sort(order);
    BOOST_TEST(order == (std::vector<std::string>{"a", "b", "c"}));
}

BOOST_AUTO_TEST_CASE(ALongerCycleAlsoTerminates) {
    const Dependencies dependencies = {{"a", {"b"}}, {"b", {"c"}}, {"c", {"a"}}};

    const auto order = topologicalStartOrder(dependencies);

    BOOST_TEST(order.size() == 3U);
}

BOOST_AUTO_TEST_CASE(AModuleDependingOnItselfIsNotAHang) {
    const Dependencies dependencies = {{"a", {"a"}}, {"b", {}}};

    const auto order = topologicalStartOrder(dependencies);

    BOOST_TEST(order.size() == 2U);
}

BOOST_AUTO_TEST_CASE(AChainIsStartedInOrder) {
    // Named so that the alphabet disagrees with the dependencies: if the sort were not actually
    // sorting, "a" would come first, and it has to come last.
    const Dependencies dependencies = {{"a", {"b"}}, {"b", {"c"}}, {"c", {}}};

    const auto order = topologicalStartOrder(dependencies);

    BOOST_TEST(order == (std::vector<std::string>{"c", "b", "a"}));
}

BOOST_AUTO_TEST_CASE(NothingRegisteredIsNotAFailure) {
    BOOST_TEST(topologicalStartOrder({}).empty());
}
