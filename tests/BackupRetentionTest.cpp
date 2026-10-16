// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE BackupRetentionTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// Euclid includes
#include <Backup.h>

using Euclid::EMM::BackupsToRemove;
using Euclid::EMM::IsBackupArchive;

// EMM writes a backup every night and keeps the newest few. What that means in practice is a
// scheduled task deleting files on a production host, so what it will and will not delete is
// worth stating rather than assuming.
//
// Two properties carry the weight. It deletes only files it recognises as its own, whatever else
// the directory holds; and a retention count it cannot make sense of leaves everything alone
// rather than removing it all.

BOOST_AUTO_TEST_CASE(TheNewestArchivesAreKept) {

    const std::vector<std::string> names{
            "euclid-backup-2026-09-18T01-00-00.zip",
            "euclid-backup-2026-09-19T01-00-00.zip",
            "euclid-backup-2026-09-20T01-00-00.zip",
    };

    const auto removed = BackupsToRemove(names, 2);
    BOOST_REQUIRE(removed.size() == 1U);
    BOOST_TEST(removed.front() == "euclid-backup-2026-09-18T01-00-00.zip");
}

BOOST_AUTO_TEST_CASE(FewerArchivesThanTheLimitRemovesNothing) {

    const std::vector<std::string> names{
            "euclid-backup-2026-09-19T01-00-00.zip",
            "euclid-backup-2026-09-20T01-00-00.zip",
    };
    BOOST_TEST(BackupsToRemove(names, 7).empty());
    BOOST_TEST(BackupsToRemove(names, 2).empty());
    BOOST_TEST(BackupsToRemove({}, 7).empty());
}

BOOST_AUTO_TEST_CASE(OldestGoFirstAndTheOrderIsByName) {

    // Archives are named with an ISO timestamp, which is the only reason age can be decided from
    // a name at all. If the name format ever stops sorting chronologically, this stops being true
    // and retention starts deleting the wrong files.
    std::vector<std::string> names{
            "euclid-backup-2026-09-20T01-00-00.zip",
            "euclid-backup-2026-09-17T01-00-00.zip",
            "euclid-backup-2026-09-19T01-00-00.zip",
            "euclid-backup-2026-09-18T01-00-00.zip",
    };

    const auto removed = BackupsToRemove(names, 1);
    BOOST_REQUIRE(removed.size() == 3U);
    BOOST_TEST(removed[0] == "euclid-backup-2026-09-17T01-00-00.zip");
    BOOST_TEST(removed[1] == "euclid-backup-2026-09-18T01-00-00.zip");
    BOOST_TEST(removed[2] == "euclid-backup-2026-09-19T01-00-00.zip");
}

BOOST_AUTO_TEST_CASE(NothingElseInTheDirectoryIsEverDeleted) {

    // The backup directory is a directory on somebody's server, and retention runs unattended. A
    // file that is not one of these archives is not this task's to remove, however old it is and
    // however full the disk.
    const std::vector<std::string> names{
            "euclid-backup-2026-09-17T01-00-00.zip",
            "euclid-backup-2026-09-18T01-00-00.zip",
            "euclid-backup-2026-09-19T01-00-00.zip",
            "notes.txt",
            "euclid-backup-2026-09-19T01-00-00.json",// the payload, mid-write
            "backup.zip",
            "euclid-backup-.zip",// prefix and suffix but no timestamp between them
            "important-database-dump.zip",
    };

    const auto removed = BackupsToRemove(names, 1);
    for (const auto &name: removed) {
        BOOST_TEST_CONTEXT(name) { BOOST_TEST(IsBackupArchive(name)); }
    }
    BOOST_REQUIRE(removed.size() == 2U);
    BOOST_TEST(removed[0] == "euclid-backup-2026-09-17T01-00-00.zip");
    BOOST_TEST(removed[1] == "euclid-backup-2026-09-18T01-00-00.zip");
}

BOOST_AUTO_TEST_CASE(AnUnreadableRetentionCountDeletesNothingRatherThanEverything) {

    // A missing or mistyped setting reads as 0. The safe reading of "keep 0 backups" is "do not
    // manage retention", not "delete every backup this installation has" - the second is
    // unrecoverable and would be done by a task nobody is watching at one in the morning.
    const std::vector<std::string> names{
            "euclid-backup-2026-09-18T01-00-00.zip",
            "euclid-backup-2026-09-19T01-00-00.zip",
            "euclid-backup-2026-09-20T01-00-00.zip",
    };

    BOOST_TEST(BackupsToRemove(names, 0).empty());
    BOOST_TEST(BackupsToRemove(names, -1).empty());
}

BOOST_AUTO_TEST_CASE(WhatCountsAsOneOfOurArchives) {

    BOOST_TEST(IsBackupArchive("euclid-backup-2026-09-20T01-00-00.zip"));
    BOOST_TEST(!IsBackupArchive("euclid-backup-2026-09-20T01-00-00.json"));
    BOOST_TEST(!IsBackupArchive("euclid-backup-.zip"));
    BOOST_TEST(!IsBackupArchive("backup.zip"));
    BOOST_TEST(!IsBackupArchive(""));
    BOOST_TEST(!IsBackupArchive("euclid-backup-"));
}

// ── list-backups ─────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(TheStartTimeIsReadFromTheName) {

    using Euclid::EMM::BackupStartedAt;

    BOOST_TEST(BackupStartedAt("euclid-backup-2026-10-08T01-00-00.zip") == "2026-10-08T01:00:00Z");
    BOOST_TEST(BackupStartedAt("euclid-backup-2026-10-08T01-00-00.json") == "2026-10-08T01:00:00Z");
    BOOST_TEST(BackupStartedAt("euclid-backup-.zip").empty());
    BOOST_TEST(BackupStartedAt("euclid-backup-2026-10-08 01-00-00.zip").empty());
    BOOST_TEST(BackupStartedAt("euclid-backup-2026-1x-08T01-00-00.zip").empty());
    BOOST_TEST(BackupStartedAt("notes-2026-10-08T01-00-00.zip").empty());
}

namespace {

    // A directory of its own, removed again however the test ends.
    struct ScratchDirectory {
        std::filesystem::path path;

        ScratchDirectory() : path(std::filesystem::temp_directory_path() /
                                  ("euclid-list-backups-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
            std::filesystem::create_directories(path);
        }

        ~ScratchDirectory() {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }

        void write(const std::string &name, const std::string &content) const {
            std::ofstream(path / name, std::ios::binary) << content;
        }
    };

}// namespace

// What a night the backup died on looks like: the older archives, and the export it never zipped.
// That one is listed - incomplete - because it is the only trace such a night leaves.
BOOST_AUTO_TEST_CASE(BackupsAreListedNewestFirstIncludingOneThatNeverFinished) {

    const ScratchDirectory dir;
    dir.write("euclid-backup-2026-10-07T01-00-00.zip", "zip7");
    dir.write("euclid-backup-2026-10-08T01-00-00.zip", "zip8");
    dir.write("euclid-backup-2026-10-09T01-00-00.json", "{\"half\":");
    dir.write("README.txt", "not a backup");
    std::filesystem::create_directory(dir.path / "euclid-backup-2026-10-10T01-00-00.zip");

    std::error_code ec;
    const auto backups = Euclid::EMM::ListBackups(dir.path, ec);

    BOOST_TEST(!ec);
    BOOST_REQUIRE(backups.size() == 3U);
    BOOST_TEST(backups[0].name == "euclid-backup-2026-10-09T01-00-00.json");
    BOOST_TEST(!backups[0].complete);
    BOOST_TEST(backups[0].startedAt == "2026-10-09T01:00:00Z");
    BOOST_TEST(backups[1].name == "euclid-backup-2026-10-08T01-00-00.zip");
    BOOST_TEST(backups[1].complete);
    BOOST_TEST(backups[1].size == 4U);
    BOOST_TEST(backups[2].name == "euclid-backup-2026-10-07T01-00-00.zip");
}

// No backup has run yet: nothing to list, and nothing wrong.
BOOST_AUTO_TEST_CASE(AMissingDirectoryIsAnEmptyListNotAnError) {

    std::error_code ec;
    const auto backups = Euclid::EMM::ListBackups(std::filesystem::temp_directory_path() / "euclid-no-such-backup-directory", ec);

    BOOST_TEST(!ec);
    BOOST_TEST(backups.empty());
}
