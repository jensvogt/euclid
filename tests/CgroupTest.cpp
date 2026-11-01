// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE CgroupTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#ifndef _WIN32
#include <unistd.h>
#endif

// Euclid includes
#include <euclid/core/Cgroup.h>

namespace Cgroup = Euclid::Core::Cgroup;

// The per-instance cgroups of docs/resource-limits.md, steps 1 and 2. The parsers are pure; the
// tree is exercised against a directory standing in for /sys/fs/cgroup, which is what a group
// looks like to the code that makes one - directories, and small files it reads and writes. What a
// directory cannot stand in for is the kernel refusing something, so the "not delegated" path is
// the one case here that is about permissions rather than layout.

namespace {

    struct TempDir {

        TempDir() : path(std::filesystem::temp_directory_path() / ("euclid-cgroup-" + std::to_string(counter().fetch_add(1)))) {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
            std::filesystem::create_directories(path);
        }

        ~TempDir() {
            std::error_code ec;
            std::filesystem::permissions(path, std::filesystem::perms::owner_all, std::filesystem::perm_options::add, ec);
            std::filesystem::remove_all(path, ec);
        }

        std::filesystem::path path;

      private:
        static std::atomic<unsigned> &counter() {
            static std::atomic<unsigned> next{0};
            return next;
        }
    };

    void write(const std::filesystem::path &path, const std::string &contents) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << contents;
    }

    std::string read(const std::filesystem::path &path) {
        std::ifstream in(path);
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    }

    // A unit's group as systemd leaves it with Delegate=memory cpu pids.
    std::filesystem::path delegatedUnit(const std::filesystem::path &mount, const std::string &controllers = "cpuset cpu io memory pids\n") {
        const auto unit = mount / "system.slice" / "euclid-wrk.service";
        write(unit / "cgroup.controllers", controllers);
        return unit;
    }

}// namespace

BOOST_AUTO_TEST_SUITE(Parsing)

    BOOST_AUTO_TEST_CASE(SelfCgroupIsTheV2Line) {
        BOOST_CHECK_EQUAL(Cgroup::ParseSelfCgroup("0::/system.slice/euclid-wrk.service\n").value_or("?"), "system.slice/euclid-wrk.service");
        // A hybrid host lists v1 hierarchies too; only the v2 line is ours.
        BOOST_CHECK_EQUAL(Cgroup::ParseSelfCgroup("12:memory:/system.slice\n0::/user.slice/x.scope\n").value_or("?"), "user.slice/x.scope");
        BOOST_CHECK(!Cgroup::ParseSelfCgroup("12:memory:/system.slice\n").has_value());
        BOOST_CHECK(!Cgroup::ParseSelfCgroup("").has_value());
        // The root group is a path, and an empty one; Setup() refuses it, the parser does not.
        BOOST_CHECK_EQUAL(Cgroup::ParseSelfCgroup("0::/\n").value_or("?"), "");
    }

    BOOST_AUTO_TEST_CASE(KeyedFiles) {
        const std::string events = "low 0\nhigh 12\nmax 3\noom 1\noom_kill 2\noom_group_kill 0\n";
        BOOST_CHECK_EQUAL(Cgroup::KeyedValue(events, "oom_kill").value_or(-9), 2);
        // A key that is the prefix of another must not match it.
        BOOST_CHECK_EQUAL(Cgroup::KeyedValue(events, "oom").value_or(-9), 1);
        BOOST_CHECK(!Cgroup::KeyedValue(events, "missing").has_value());
        BOOST_CHECK_EQUAL(Cgroup::KeyedValue("usage_usec 45790870700\nthrottled_usec 7", "usage_usec").value_or(-9), 45790870700LL);
    }

    BOOST_AUTO_TEST_CASE(SingleValues) {
        BOOST_CHECK_EQUAL(Cgroup::SingleValue("6999707648\n").value_or(-9), 6999707648LL);
        BOOST_CHECK_EQUAL(Cgroup::SingleValue("max\n").value_or(-9), -1);
        BOOST_CHECK(!Cgroup::SingleValue("").has_value());
        BOOST_CHECK(!Cgroup::SingleValue("12abc").has_value());
    }

    BOOST_AUTO_TEST_CASE(Controllers) {
        BOOST_CHECK(Cgroup::HasController("cpuset cpu io memory pids\n", "memory"));
        BOOST_CHECK(Cgroup::HasController("cpuset cpu io memory pids\n", "pids"));
        // cpu is not cpuset.
        BOOST_CHECK(!Cgroup::HasController("cpuset io memory\n", "cpu"));
        BOOST_CHECK(!Cgroup::HasController("", "memory"));
    }

    BOOST_AUTO_TEST_CASE(CpuMaxReadsAsCores) {
        BOOST_CHECK_CLOSE(Cgroup::ParseCpuMax("150000 100000\n").value_or(-9), 1.5, 1e-9);
        BOOST_CHECK_CLOSE(Cgroup::ParseCpuMax("50000 100000").value_or(-9), 0.5, 1e-9);
        BOOST_CHECK_EQUAL(Cgroup::ParseCpuMax("max 100000\n").value_or(-9), -1.0);
        BOOST_CHECK(!Cgroup::ParseCpuMax("").has_value());
        BOOST_CHECK(!Cgroup::ParseCpuMax("150000").has_value());
        BOOST_CHECK(!Cgroup::ParseCpuMax("150000 0").has_value());
    }

    BOOST_AUTO_TEST_CASE(ThrottlingIsAShareOfPeriods) {
        // 50 of the 200 periods since the last reading ran out of quota.
        BOOST_CHECK_CLOSE(Cgroup::ThrottledPercent(1000, 100, 1200, 150).value_or(-9), 25.0, 1e-9);
        BOOST_CHECK_CLOSE(Cgroup::ThrottledPercent(1000, 100, 1200, 100).value_or(-9), 0.0, 1e-9);
        // No period passed - an idle instance - is no reading, not zero and not a division by it.
        BOOST_CHECK(!Cgroup::ThrottledPercent(1000, 100, 1000, 100).has_value());
        // Counters that went backwards belong to another process.
        BOOST_CHECK(!Cgroup::ThrottledPercent(1000, 100, 10, 1).has_value());
        BOOST_CHECK(!Cgroup::ThrottledPercent(-1, -1, 10, 1).has_value());
    }

    BOOST_AUTO_TEST_CASE(MemoryHighSitsBelowTheCeiling) {
        constexpr long long gib = 1024LL * 1024 * 1024;
        const auto high = Cgroup::MemoryHighFor(gib, 90);
        BOOST_CHECK(high > 0 && high < gib);
        BOOST_CHECK_EQUAL(high % 4096, 0);
        BOOST_CHECK_EQUAL(Cgroup::MemoryHighFor(gib, 0), -1);
        BOOST_CHECK_EQUAL(Cgroup::MemoryHighFor(gib, 100), -1);
        BOOST_CHECK_EQUAL(Cgroup::MemoryHighFor(0, 90), -1);
    }

    BOOST_AUTO_TEST_CASE(NamesCannotLeaveTheTree) {
        BOOST_CHECK_EQUAL(Cgroup::SafeName("import-onix3"), "import-onix3");
        BOOST_CHECK_EQUAL(Cgroup::SafeName("../../etc"), "etc");
        BOOST_CHECK_EQUAL(Cgroup::SafeName(".."), "unnamed");
        BOOST_CHECK_EQUAL(Cgroup::SafeName(""), "unnamed");
        BOOST_CHECK_EQUAL(Cgroup::SafeName("a\nb"), "ab");
    }

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(Usage)

    BOOST_AUTO_TEST_CASE(ReadsWhatTheKernelProvides) {
        TempDir temp;
        const auto group = temp.path / "g";
        write(group / "memory.current", "1048576\n");
        write(group / "memory.peak", "4194304\n");
        write(group / "memory.max", "max\n");
        write(group / "memory.events", "low 0\nhigh 0\nmax 0\noom 1\noom_kill 1\n");
        write(group / "cpu.stat", "usage_usec 900\nuser_usec 600\nsystem_usec 300\nthrottled_usec 25\n");
        write(group / "cgroup.events", "populated 1\nfrozen 0\n");

        const auto usage = Cgroup::ReadUsage(group);
        BOOST_REQUIRE(usage.has_value());
        BOOST_CHECK_EQUAL(usage->memoryBytes, 1048576);
        BOOST_CHECK_EQUAL(usage->memoryPeakBytes, 4194304);
        BOOST_CHECK_EQUAL(usage->memoryMaxBytes, -1);
        BOOST_CHECK_EQUAL(usage->oomKills, 1);
        BOOST_CHECK_EQUAL(usage->cpuUsageUsec, 900);
        BOOST_CHECK_EQUAL(usage->cpuThrottledUsec, 25);
        BOOST_CHECK(usage->populated);
    }

    BOOST_AUTO_TEST_CASE(MissingFiguresStayUnknown) {
        // memory.peak arrived in Linux 5.19, and cpu.stat's throttling only with the cpu
        // controller: absent is -1, never zero, so nobody records a peak of nothing.
        TempDir temp;
        write(temp.path / "g" / "memory.current", "10\n");
        const auto usage = Cgroup::ReadUsage(temp.path / "g");
        BOOST_REQUIRE(usage.has_value());
        BOOST_CHECK_EQUAL(usage->memoryBytes, 10);
        BOOST_CHECK_EQUAL(usage->memoryPeakBytes, -1);
        BOOST_CHECK_EQUAL(usage->oomKills, -1);
        BOOST_CHECK_EQUAL(usage->cpuThrottledUsec, -1);
        BOOST_CHECK(!usage->populated);

        BOOST_CHECK(!Cgroup::ReadUsage(temp.path / "absent").has_value());
    }

BOOST_AUTO_TEST_SUITE_END()

#ifndef _WIN32

BOOST_AUTO_TEST_SUITE(Tree)

    BOOST_AUTO_TEST_CASE(DelegatedUnitGetsTheLayout) {
        TempDir temp;
        const auto unit = delegatedUnit(temp.path);

        const auto tree = Cgroup::Tree::Setup(temp.path, "system.slice/euclid-wrk.service");
        BOOST_REQUIRE_MESSAGE(tree.Enabled(), tree.Reason());
        BOOST_CHECK_EQUAL(tree.Root().value_or("?"), unit);

        // This process left the unit's group before any controller was enabled there.
        BOOST_CHECK_EQUAL(read(unit / "worker" / "cgroup.procs"), std::to_string(::getpid()));
        // Only the controllers the host offers, memory first.
        BOOST_CHECK_EQUAL(read(unit / "cgroup.subtree_control"), "+memory +cpu +pids");
        BOOST_CHECK_EQUAL(read(unit / "applications" / "cgroup.subtree_control"), "+memory +cpu +pids");

        const auto group = tree.CreateInstance("import-onix3", "0d6a416d-4b7b-454e-9ee6-113b4af9ee36");
        BOOST_REQUIRE(group.has_value());
        BOOST_CHECK_EQUAL(*group, unit / "applications" / "import-onix3" / "0d6a416d-4b7b-454e-9ee6-113b4af9ee36");
        BOOST_CHECK(std::filesystem::is_directory(*group));
        BOOST_CHECK_EQUAL(read(unit / "applications" / "import-onix3" / "cgroup.subtree_control"), "+memory +cpu +pids");

        // Made twice is the same group.
        BOOST_CHECK_EQUAL(tree.CreateInstance("import-onix3", "0d6a416d-4b7b-454e-9ee6-113b4af9ee36").value_or("?"), *group);
    }

    BOOST_AUTO_TEST_CASE(OptionalControllersAreOptional) {
        TempDir temp;
        const auto unit = delegatedUnit(temp.path, "memory\n");
        const auto tree = Cgroup::Tree::Setup(temp.path, "system.slice/euclid-wrk.service");
        BOOST_REQUIRE_MESSAGE(tree.Enabled(), tree.Reason());
        BOOST_CHECK_EQUAL(read(unit / "cgroup.subtree_control"), "+memory");
    }

    BOOST_AUTO_TEST_CASE(NoMemoryControllerIsNotEnabled) {
        // What an undelegated unit looks like from inside: systemd enabled nothing for it.
        TempDir temp;
        const auto unit = delegatedUnit(temp.path, "");
        const auto tree = Cgroup::Tree::Setup(temp.path, "system.slice/euclid-wrk.service");
        BOOST_CHECK(!tree.Enabled());
        BOOST_CHECK(tree.Reason().find("Delegate=") != std::string::npos);
        // The node's total is still there to read.
        BOOST_CHECK_EQUAL(tree.Root().value_or("?"), unit);
        BOOST_CHECK(!tree.CreateInstance("app", "i").has_value());
        // Nothing was written: an undelegated tree is left exactly as found.
        BOOST_CHECK(!std::filesystem::exists(unit / "worker"));
    }

    BOOST_AUTO_TEST_CASE(NotOwnedIsNotEnabled) {
        if (::geteuid() == 0) return;// root can write anywhere, which is not the case under test
        TempDir temp;
        const auto unit = delegatedUnit(temp.path);
        std::filesystem::permissions(unit, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);
        const auto tree = Cgroup::Tree::Setup(temp.path, "system.slice/euclid-wrk.service");
        std::filesystem::permissions(unit, std::filesystem::perms::owner_all);
        BOOST_CHECK(!tree.Enabled());
        BOOST_CHECK(tree.Reason().find("not delegated") != std::string::npos);
    }

    BOOST_AUTO_TEST_CASE(NoGroupIsNotEnabled) {
        TempDir temp;
        BOOST_CHECK(!Cgroup::Tree::Setup(temp.path, "system.slice/absent.service").Enabled());
        BOOST_CHECK(!Cgroup::Tree::Setup(temp.path, "").Enabled());
        BOOST_CHECK(!Cgroup::Tree().Enabled());
    }

    BOOST_AUTO_TEST_CASE(RemoveAndSweep) {
        TempDir temp;
        const auto unit = delegatedUnit(temp.path);
        const auto tree = Cgroup::Tree::Setup(temp.path, "system.slice/euclid-wrk.service");
        BOOST_REQUIRE(tree.Enabled());

        const auto first = tree.CreateInstance("parser", "one");
        const auto second = tree.CreateInstance("parser", "two");
        const auto third = tree.CreateInstance("export", "three");
        BOOST_REQUIRE(first && second && third);

        BOOST_CHECK(Cgroup::Tree::RemoveInstance(*first));
        BOOST_CHECK(!std::filesystem::exists(*first));
        // Already gone counts as removed.
        BOOST_CHECK(Cgroup::Tree::RemoveInstance(*first));

        // A previous run's leftovers go; the running instance stays.
        tree.Sweep([](const std::string &instanceId) { return instanceId == "two"; });
        BOOST_CHECK(std::filesystem::exists(*second));
        BOOST_CHECK(!std::filesystem::exists(*third));
    }

    BOOST_AUTO_TEST_CASE(MemoryLimitSetsMaxHighAndSwap) {
        TempDir temp;
        delegatedUnit(temp.path);
        const auto tree = Cgroup::Tree::Setup(temp.path, "system.slice/euclid-wrk.service");
        BOOST_REQUIRE(tree.Enabled());
        const auto group = tree.CreateInstance("parser", "one");
        BOOST_REQUIRE(group);
        // cgroupfs has memory.swap.max only with swap accounting; a plain directory has to be
        // given one to stand in for a kernel that has it.
        write(*group / "memory.swap.max", "max\n");

        BOOST_CHECK(!Cgroup::ApplyMemoryLimit(*group, 1024LL * 1024 * 1024, 90).has_value());
        BOOST_CHECK_EQUAL(read(*group / "memory.max"), "1073741824");
        BOOST_CHECK_EQUAL(read(*group / "memory.high"), std::to_string(Cgroup::MemoryHighFor(1024LL * 1024 * 1024, 90)));
        BOOST_CHECK_EQUAL(read(*group / "memory.swap.max"), "0");
    }

    BOOST_AUTO_TEST_CASE(NoSwapAccountingLeavesSwapAlone) {
        TempDir temp;
        delegatedUnit(temp.path);
        const auto tree = Cgroup::Tree::Setup(temp.path, "system.slice/euclid-wrk.service");
        const auto group = tree.CreateInstance("parser", "one");
        BOOST_REQUIRE(group);
        BOOST_CHECK(!Cgroup::ApplyMemoryLimit(*group, 512LL * 1024 * 1024, 0).has_value());
        BOOST_CHECK_EQUAL(read(*group / "memory.max"), "536870912");
        // 0 percent leaves memory.high unset; no swap file means none is made.
        BOOST_CHECK(!std::filesystem::exists(*group / "memory.high"));
        BOOST_CHECK(!std::filesystem::exists(*group / "memory.swap.max"));
        BOOST_CHECK(Cgroup::ApplyMemoryLimit(*group, 0, 90).has_value());
    }

    BOOST_AUTO_TEST_CASE(CpuLimitIsAQuotaPerPeriod) {
        TempDir temp;
        delegatedUnit(temp.path);
        const auto tree = Cgroup::Tree::Setup(temp.path, "system.slice/euclid-wrk.service");
        const auto group = tree.CreateInstance("parser", "one");
        BOOST_REQUIRE(group);
        BOOST_CHECK(!Cgroup::ApplyCpuLimit(*group, 1.5).has_value());
        BOOST_CHECK_EQUAL(read(*group / "cpu.max"), "150000 100000");
        BOOST_CHECK_CLOSE(Cgroup::ReadUsage(*group)->cpuLimit, 1.5, 1e-9);
        // The kernel's floor, 1 ms in 100.
        BOOST_CHECK(!Cgroup::ApplyCpuLimit(*group, 0.001).has_value());
        BOOST_CHECK_EQUAL(read(*group / "cpu.max"), "1000 100000");
        BOOST_CHECK(Cgroup::ApplyCpuLimit(*group, 0.0).has_value());
    }

    BOOST_AUTO_TEST_CASE(TaskLimitIsPidsMax) {
        TempDir temp;
        delegatedUnit(temp.path);
        const auto tree = Cgroup::Tree::Setup(temp.path, "system.slice/euclid-wrk.service");
        const auto group = tree.CreateInstance("parser", "one");
        BOOST_REQUIRE(group);
        BOOST_CHECK(!Cgroup::ApplyTaskLimit(*group, 4096).has_value());
        BOOST_CHECK_EQUAL(read(*group / "pids.max"), "4096");
        BOOST_CHECK(Cgroup::ApplyTaskLimit(*group, 0).has_value());

        write(*group / "pids.current", "175\n");
        write(*group / "pids.events", "max 3\n");
        const auto usage = Cgroup::ReadUsage(*group);
        BOOST_CHECK_EQUAL(usage->tasks, 175);
        BOOST_CHECK_EQUAL(usage->tasksMax, 4096);
        BOOST_CHECK_EQUAL(usage->tasksLimitHits, 3);
    }

    BOOST_AUTO_TEST_CASE(ChildJoinsThroughTheOpenedFile) {
        TempDir temp;
        const auto unit = delegatedUnit(temp.path);
        const auto tree = Cgroup::Tree::Setup(temp.path, "system.slice/euclid-wrk.service");
        BOOST_REQUIRE(tree.Enabled());
        const auto group = tree.CreateInstance("parser", "one");
        BOOST_REQUIRE(group);

        // cgroupfs makes cgroup.procs with the directory; a plain one has to be given one.
        write(*group / "cgroup.procs", "");
        const int fd = Cgroup::OpenProcs(*group);
        BOOST_REQUIRE(fd >= 0);
        BOOST_CHECK(Cgroup::JoinFromChild(fd));
        ::close(fd);
        BOOST_CHECK_EQUAL(read(*group / "cgroup.procs"), "0");

        BOOST_CHECK(!Cgroup::JoinFromChild(-1));
        BOOST_CHECK_EQUAL(Cgroup::OpenProcs(temp.path / "absent"), -1);
    }

BOOST_AUTO_TEST_SUITE_END()

#endif
