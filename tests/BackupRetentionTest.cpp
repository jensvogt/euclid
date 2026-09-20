// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE BackupRetentionTest
#include <boost/test/unit_test.hpp>

// C++ includes
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
