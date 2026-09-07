#define BOOST_TEST_MODULE DirUtilsTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

// Euclid includes
#include <euclid/core/DirUtils.h>

using Euclid::Core::DirUtils;

// Objects are named after a generated UUID and were all kept in one directory, which stopped
// working at nine million of them: ext4's directory index reaches its maximum depth and refuses a
// share of new names with ENOSPC while the disk is nearly empty. Fanning the name out over two
// levels of itself fixes that, and what has to hold afterwards is that the two layouts coexist -
// every object written before the change is still found, still removed, and still served, with
// nothing migrated.

namespace {

    // A directory of this test's own under the system temp directory, removed with everything in
    // it when the test ends. Numbered rather than random, so a leftover directory names the case
    // it came from and nothing here depends on a seeded RNG.
    struct TempDir {

        TempDir() : path(std::filesystem::temp_directory_path() / ("euclid-dirutils-" + std::to_string(counter().fetch_add(1)))) {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
            std::filesystem::create_directories(path);
        }

        ~TempDir() {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }

        std::filesystem::path path;

    private:
        static std::atomic<unsigned> &counter() {
            static std::atomic<unsigned> next{0};
            return next;
        }
    };

    void write(const std::filesystem::path &path, const std::string &contents = "x") {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << contents;
    }

    constexpr auto kName = "96719be3-d61a-4ade-a522-7af0ad89bd3c";

}// namespace

BOOST_AUTO_TEST_SUITE(DirUtilsTest)

    BOOST_AUTO_TEST_CASE(NameIsFannedOutOverItsOwnFirstCharacters) {

        const TempDir root;
        const auto path = DirUtils::ShardedFilePath(root.path, kName);

        BOOST_CHECK_EQUAL(path, root.path / "objects" / "96" / "71" / kName);

        // Derived from the name alone, so the same name always lands in the same place and nothing
        // has to record where anything went.
        BOOST_CHECK_EQUAL(DirUtils::ShardedFilePath(root.path, kName), path);
    }

    BOOST_AUTO_TEST_CASE(NamesThatCannotBeFannedOutStayFlat) {

        const TempDir root;

        // Too short to take two levels out of.
        BOOST_CHECK_EQUAL(DirUtils::ShardedFilePath(root.path, "abc"), root.path / "abc");

        // A name carrying a separator or a parent reference must never be built into a path -
        // fanning these out is how a storage directory gets written outside of.
        BOOST_CHECK_EQUAL(DirUtils::ShardedFilePath(root.path, "../../etc/passwd"), root.path / "../../etc/passwd");
        BOOST_CHECK_EQUAL(DirUtils::ShardedFilePath(root.path, "sub/dir/name"), root.path / "sub/dir/name");
        BOOST_CHECK_EQUAL(DirUtils::ShardedFilePath(root.path, ".."), root.path / "..");
    }

    BOOST_AUTO_TEST_CASE(CreateMakesTheDirectoryTheFileGoesIn) {

        const TempDir root;
        const auto path = DirUtils::CreateFilePath(root.path, kName);

        BOOST_CHECK_EQUAL(path, root.path / "objects" / "96" / "71" / kName);
        BOOST_CHECK(std::filesystem::is_directory(path.parent_path()));

        // Usable straight away: that is the whole point of handing back a created directory.
        write(path, "content");
        BOOST_CHECK(std::filesystem::exists(path));
    }

    BOOST_AUTO_TEST_CASE(FindPrefersTheFannedOutFile) {

        const TempDir root;
        write(DirUtils::ShardedFilePath(root.path, kName));

        BOOST_CHECK_EQUAL(DirUtils::FindFilePath(root.path, kName), root.path / "objects" / "96" / "71" / kName);
    }

    BOOST_AUTO_TEST_CASE(FindFallsBackToWhereTheOldObjectsAre) {

        const TempDir root;
        write(root.path / kName);

        // Everything written before the fan-out existed is still where it was, and is still found
        // without anything having been moved.
        BOOST_CHECK_EQUAL(DirUtils::FindFilePath(root.path, kName), root.path / kName);
    }

    BOOST_AUTO_TEST_CASE(FindNamesTheFlatPathForAFileThatIsNowhere) {

        const TempDir root;

        // So a caller can open it and report the failure against a path, rather than having to
        // decide for itself what the file would have been called.
        BOOST_CHECK_EQUAL(DirUtils::FindFilePath(root.path, kName), root.path / kName);
    }

    BOOST_AUTO_TEST_CASE(RemoveFindsTheFileInEitherLayout) {

        const TempDir root;
        std::error_code ec;

        write(root.path / kName);
        BOOST_CHECK(DirUtils::RemoveFile(root.path, kName, ec));
        BOOST_CHECK(!ec);
        BOOST_CHECK(!std::filesystem::exists(root.path / kName));

        write(DirUtils::ShardedFilePath(root.path, kName));
        BOOST_CHECK(DirUtils::RemoveFile(root.path, kName, ec));
        BOOST_CHECK(!ec);
        BOOST_CHECK(!std::filesystem::exists(DirUtils::ShardedFilePath(root.path, kName)));
    }

    BOOST_AUTO_TEST_CASE(RemoveTakesBothCopiesWhenThereAreTwo) {

        const TempDir root;
        write(root.path / kName);
        write(DirUtils::ShardedFilePath(root.path, kName));

        // A file in both places - an object written flat and later moved, say - must not leave the
        // second copy behind, unreferenced and invisible.
        std::error_code ec;
        BOOST_CHECK(DirUtils::RemoveFile(root.path, kName, ec));
        BOOST_CHECK(!std::filesystem::exists(root.path / kName));
        BOOST_CHECK(!std::filesystem::exists(DirUtils::ShardedFilePath(root.path, kName)));
    }

    BOOST_AUTO_TEST_CASE(RemovingWhatIsNotThereIsNotAnError) {

        const TempDir root;
        std::error_code ec;

        BOOST_CHECK(!DirUtils::RemoveFile(root.path, kName, ec));
        BOOST_CHECK(!ec);
    }

BOOST_AUTO_TEST_SUITE_END()
