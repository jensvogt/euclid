// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE EmoDiskSpaceTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <filesystem>

// Euclid includes
#include <DiskSpace.h>

// disk-used-percent: how full a filesystem is, as a share of its total size, with what is reserved
// for root counted as used so that 100% is the point at which euclid can no longer write.

using Euclid::Monitoring::DiskUsedPercent;

BOOST_AUTO_TEST_CASE(UsedIsTheTotalLessWhatIsAvailable) {
    BOOST_TEST(*DiskUsedPercent(1000, 250) == 75.0, boost::test_tools::tolerance(1e-9));
    BOOST_TEST(*DiskUsedPercent(1000, 1000) == 0.0, boost::test_tools::tolerance(1e-9));
    BOOST_TEST(*DiskUsedPercent(1000, 0) == 100.0, boost::test_tools::tolerance(1e-9));
}

BOOST_AUTO_TEST_CASE(TheRootReserveCountsAsUsed) {
    // 1 TB with 5% reserved for root and nothing left for anybody else: full, not 95%.
    constexpr std::uintmax_t tb = 1'000'000'000'000;
    BOOST_TEST(*DiskUsedPercent(tb, 0) == 100.0, boost::test_tools::tolerance(1e-9));
}

BOOST_AUTO_TEST_CASE(AFilesystemWithNoSizeHasNoPercentage) {
    BOOST_TEST(!DiskUsedPercent(0, 0).has_value());
    // More available than the total is nonsense from the filesystem; it reads as empty, not negative.
    BOOST_TEST(*DiskUsedPercent(100, 500) == 0.0, boost::test_tools::tolerance(1e-9));
}

BOOST_AUTO_TEST_CASE(ARealVolumeIsBetweenZeroAndAHundred) {
    const auto space = std::filesystem::space(std::filesystem::temp_directory_path());
    const auto percent = DiskUsedPercent(space.capacity, space.available);
    BOOST_REQUIRE(percent.has_value());
    BOOST_TEST(*percent >= 0.0);
    BOOST_TEST(*percent <= 100.0);
}
