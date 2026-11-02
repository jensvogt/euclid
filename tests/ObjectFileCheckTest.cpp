// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE ObjectFileCheckTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

// Euclid includes
#include <ObjectFileCheck.h>
#include <euclid/core/DirUtils.h>
#include <euclid/core/ObjectCipher.h>

using Euclid::Core::DirUtils;
using Euclid::Core::ObjectCipher;
using Euclid::ESM::CheckObjectFile;

namespace {

    // A directory of its own, removed again however the test ends.
    struct ScratchDirectory {
        std::filesystem::path path;

        ScratchDirectory() : path(std::filesystem::temp_directory_path() /
                                  ("euclid-object-file-check-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
            std::filesystem::create_directories(path);
        }

        ~ScratchDirectory() {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }

        [[nodiscard]] std::filesystem::path write(const std::string &name, const std::string &content) const {
            const auto file = path / name;
            std::ofstream(file, std::ios::binary) << content;
            return file;
        }
    };

    const std::string kKey(32, 'k');

}// namespace

// A file in the clear is whole when it is exactly the recorded size.
BOOST_AUTO_TEST_CASE(APlainFileOfTheRecordedSizeIsWhole) {

    const ScratchDirectory dir;
    const auto file = dir.write("plain", "0123456789");

    BOOST_TEST(!CheckObjectFile(file, 10, false).has_value());
}

// What a crash leaves: the record kept, the file gone, emptied or cut off.
BOOST_AUTO_TEST_CASE(AMissingEmptyOrShortPlainFileIsReported) {

    const ScratchDirectory dir;

    const auto missing = CheckObjectFile(dir.path / "nothing", 10, false);
    BOOST_REQUIRE(missing.has_value());
    BOOST_TEST(missing->kind == "missing");
    BOOST_TEST(missing->fileSize == -1);

    const auto empty = CheckObjectFile(dir.write("empty", ""), 281861, false);
    BOOST_REQUIRE(empty.has_value());
    BOOST_TEST(empty->kind == "empty");
    BOOST_TEST(empty->expectedSize == 281861);

    const auto shortFile = CheckObjectFile(dir.write("short", "01234"), 10, false);
    BOOST_REQUIRE(shortFile.has_value());
    BOOST_TEST(shortFile->kind == "short");
    BOOST_TEST(shortFile->fileSize == 5);

    const auto longFile = CheckObjectFile(dir.write("long", "0123456789AB"), 10, false);
    BOOST_REQUIRE(longFile.has_value());
    BOOST_TEST(longFile->kind == "long");
}

// An empty object is whole as an empty file, in the clear and encrypted alike.
BOOST_AUTO_TEST_CASE(AnEmptyObjectIsNotAnEmptyFileProblem) {

    const ScratchDirectory dir;
    BOOST_TEST(!CheckObjectFile(dir.write("plain-empty", ""), 0, false).has_value());
    BOOST_TEST(!CheckObjectFile(dir.write("encrypted-empty", ObjectCipher::Encrypt(kKey, "")), 0, true).has_value());
}

// Encrypted, the file is whole when it is the length its own header implies - over several
// frames, so a whole frame cut off the end is told apart from a file that was always that long.
BOOST_AUTO_TEST_CASE(AnEncryptedFileIsMeasuredAgainstItsHeader) {

    const ScratchDirectory dir;
    const std::string plaintext(ObjectCipher::kChunkSize * 2 + 100, 'x');
    const auto encrypted = ObjectCipher::Encrypt(kKey, plaintext);
    const auto file = dir.write("encrypted", encrypted);

    BOOST_TEST(ObjectCipher::ExpectedFileSize(file) == static_cast<std::int64_t>(encrypted.size()));
    BOOST_TEST(!CheckObjectFile(file, static_cast<std::int64_t>(plaintext.size()), true).has_value());

    // The last frame cut off: every remaining frame still authenticates, and only the length says so.
    const auto lastFrame = 100 + ObjectCipher::kFrameOverhead;
    const auto truncated = dir.write("truncated", encrypted.substr(0, encrypted.size() - lastFrame));
    const auto problem = CheckObjectFile(truncated, static_cast<std::int64_t>(plaintext.size()), true);
    BOOST_REQUIRE(problem.has_value());
    BOOST_TEST(problem->kind == "short");
    BOOST_TEST(problem->expectedSize == static_cast<std::int64_t>(encrypted.size()));
}

BOOST_AUTO_TEST_CASE(AnEncryptedFileThatIsEmptyOrForeignIsReported) {

    const ScratchDirectory dir;

    const auto empty = CheckObjectFile(dir.write("empty", ""), 5000, true);
    BOOST_REQUIRE(empty.has_value());
    BOOST_TEST(empty->kind == "empty");

    // A record saying encrypted over a file that is not: what a mix-up looks like.
    const auto foreign = CheckObjectFile(dir.write("plain", std::string(5000, 'p')), 5000, true);
    BOOST_REQUIRE(foreign.has_value());
    BOOST_TEST(foreign->kind == "unreadable");
    BOOST_TEST(!foreign->detail.empty());

    // A header recording another length than the object's.
    const auto mismatch = CheckObjectFile(dir.write("other", ObjectCipher::Encrypt(kKey, "abc")), 4, true);
    BOOST_REQUIRE(mismatch.has_value());
    BOOST_TEST(mismatch->kind == "mismatch");
}

// A file written and synced is there, with its directories; a path that is not fails, saying why.
BOOST_AUTO_TEST_CASE(ASyncedFileIsReportedAsOnDisk) {

    const ScratchDirectory dir;
    const auto file = DirUtils::CreateFilePath(dir.path, "0a1b2c3d-0000-4000-8000-000000000000");
    std::ofstream(file, std::ios::binary) << "bytes";

    std::error_code ec;
    BOOST_TEST(DirUtils::SyncFile(file, ec));
    BOOST_TEST(!ec);

    BOOST_TEST(!DirUtils::SyncFile(dir.path / "not-there", ec));
    BOOST_TEST(static_cast<bool>(ec));
}
