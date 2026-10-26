// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE ArchiveZipTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <string>

// libarchive, as somebody else's reader of what ArchiveZip writes
#include <archive.h>
#include <archive_entry.h>

// Euclid includes
#include <euclid/core/ArchiveZip.h>

// The appendable zip ESM archives objects into: members stored and deflated, empty and larger
// than a buffer, read back by offset and by listing; an append interrupted half way; and a zip
// another implementation can open.

namespace {

    using Euclid::Core::ArchiveZip;

    struct TempDir {
        TempDir() : path(std::filesystem::temp_directory_path() / ("euclid-archive-zip-" + std::to_string(std::random_device{}()))) {
            std::filesystem::create_directories(path);
        }
        ~TempDir() {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
        std::filesystem::path path;
    };

    std::filesystem::path write(const std::filesystem::path &path, const std::string &contents) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << contents;
        return path;
    }

    std::string read(const std::filesystem::path &path) {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream out;
        out << in.rdbuf();
        return out.str();
    }

    // Bytes that do not compress, more than one of ArchiveZip's buffers' worth.
    std::string noise(const std::size_t length) {
        std::mt19937 random(42);
        std::string out(length, '\0');
        for (auto &c: out) c = static_cast<char>(random() & 0xffu);
        return out;
    }

    // What libarchive finds in a zip: name to contents.
    std::map<std::string, std::string> readWithLibarchive(const std::filesystem::path &zip) {
        std::map<std::string, std::string> found;
        archive *reader = archive_read_new();
        archive_read_support_format_zip_seekable(reader);
        BOOST_TEST_REQUIRE(archive_read_open_filename(reader, zip.string().c_str(), 10240) == ARCHIVE_OK);
        archive_entry *entry = nullptr;
        while (archive_read_next_header(reader, &entry) == ARCHIVE_OK) {
            std::string contents;
            char buffer[8192];
            la_ssize_t n;
            while ((n = archive_read_data(reader, buffer, sizeof buffer)) > 0) contents.append(buffer, static_cast<std::size_t>(n));
            found[archive_entry_pathname(entry)] = contents;
        }
        archive_read_free(reader);
        return found;
    }

}// namespace

BOOST_AUTO_TEST_SUITE(ArchiveZipTest)

    BOOST_AUTO_TEST_CASE(MembersAreReadBackByTheirOffset) {
        const TempDir dir;
        const auto zip = dir.path / "a" / "archive-x-012026.zip";
        const std::string text(200000, 'a');
        const auto big = noise(300000);

        const auto first = ArchiveZip::Append(zip, "text.txt", write(dir.path / "t", text), true);
        const auto second = ArchiveZip::Append(zip, "dir/noise.bin", write(dir.path / "n", big), false);
        const auto third = ArchiveZip::Append(zip, "empty", write(dir.path / "e", ""), true);

        BOOST_TEST(first.method == 8);
        BOOST_TEST(first.compressedSize < first.size);
        BOOST_TEST(second.method == 0);
        BOOST_TEST(second.compressedSize == big.size());
        BOOST_TEST(third.size == 0u);

        ArchiveZip::Extract(zip, second, dir.path / "out2");
        ArchiveZip::Extract(zip, first, dir.path / "out1");
        ArchiveZip::Extract(zip, third, dir.path / "out3");
        BOOST_TEST(read(dir.path / "out1") == text);
        BOOST_TEST(read(dir.path / "out2") == big);
        BOOST_TEST(read(dir.path / "out3").empty());
    }

    BOOST_AUTO_TEST_CASE(TheDirectoryListsEveryMemberInOrder) {
        const TempDir dir;
        const auto zip = dir.path / "z.zip";
        const auto a = ArchiveZip::Append(zip, "a", write(dir.path / "a", "one"), true);
        const auto b = ArchiveZip::Append(zip, "b", write(dir.path / "b", "two"), false);

        const auto entries = ArchiveZip::List(zip);
        BOOST_TEST_REQUIRE(entries.size() == 2u);
        BOOST_TEST(entries[0].name == "a");
        BOOST_TEST(entries[0].member.offset == a.offset);
        BOOST_TEST(entries[0].member.crc == a.crc);
        BOOST_TEST(entries[1].name == "b");
        BOOST_TEST(entries[1].member.offset == b.offset);
    }

    BOOST_AUTO_TEST_CASE(AnotherReaderOpensIt) {
        const TempDir dir;
        const auto zip = dir.path / "z.zip";
        const auto big = noise(100000);
        ArchiveZip::Append(zip, "deflated.txt", write(dir.path / "a", std::string(5000, 'x')), true);
        ArchiveZip::Append(zip, "stored.bin", write(dir.path / "b", big), false);

        const auto found = readWithLibarchive(zip);
        BOOST_TEST_REQUIRE(found.size() == 2u);
        BOOST_TEST(found.at("deflated.txt") == std::string(5000, 'x'));
        BOOST_TEST(found.at("stored.bin") == big);
    }

    BOOST_AUTO_TEST_CASE(AnInterruptedAppendIsDroppedByTheNext) {
        const TempDir dir;
        const auto zip = dir.path / "z.zip";
        const auto kept = ArchiveZip::Append(zip, "kept", write(dir.path / "a", "kept bytes"), true);
        const auto end = std::filesystem::file_size(zip);

        // What a crash part way through the next append leaves: the old directory overwritten by
        // a header with no signature yet and some of its bytes, and no end records.
        {
            std::fstream out(zip, std::ios::in | std::ios::out | std::ios::binary);
            out.seekp(static_cast<std::streamoff>(kept.offset + 30 + 4 + 20 + kept.compressedSize));
            out << std::string(30, '\0') << "half a member";
        }
        std::filesystem::resize_file(zip, end - 40);

        const auto next = ArchiveZip::Append(zip, "next", write(dir.path / "b", "next bytes"), true);
        const auto entries = ArchiveZip::List(zip);
        BOOST_TEST_REQUIRE(entries.size() == 2u);
        BOOST_TEST(entries[0].name == "kept");
        BOOST_TEST(entries[1].name == "next");

        ArchiveZip::Extract(zip, kept, dir.path / "k");
        ArchiveZip::Extract(zip, next, dir.path / "n");
        BOOST_TEST(read(dir.path / "k") == "kept bytes");
        BOOST_TEST(read(dir.path / "n") == "next bytes");
        BOOST_TEST(readWithLibarchive(zip).size() == 2u);
    }

    BOOST_AUTO_TEST_CASE(AMemberThatDoesNotCheckOutIsNotWritten) {
        const TempDir dir;
        const auto zip = dir.path / "z.zip";
        auto member = ArchiveZip::Append(zip, "a", write(dir.path / "a", "some bytes"), false);
        member.crc ^= 1u;

        BOOST_CHECK_THROW(ArchiveZip::Extract(zip, member, dir.path / "out"), std::runtime_error);
        BOOST_TEST(!std::filesystem::exists(dir.path / "out"));

        member.offset += 1;
        BOOST_CHECK_THROW(ArchiveZip::Extract(zip, member, dir.path / "out"), std::runtime_error);
    }

    BOOST_AUTO_TEST_CASE(RemovingTakesTheLockFileWithIt) {
        const TempDir dir;
        const auto zip = dir.path / "z.zip";
        ArchiveZip::Append(zip, "a", write(dir.path / "a", "x"), true);
        BOOST_TEST(std::filesystem::exists(dir.path / "z.zip.lock"));

        BOOST_TEST(ArchiveZip::Remove(zip));
        BOOST_TEST(!std::filesystem::exists(zip));
        BOOST_TEST(!std::filesystem::exists(dir.path / "z.zip.lock"));
        BOOST_TEST(!ArchiveZip::Remove(zip));
    }

BOOST_AUTO_TEST_SUITE_END()
