// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Starting an upload must never retire the version already published under that key.
//
// The bug this pins: create-upload seeded the row with CREATED whether or not the key already
// held an object, and get-object serves nothing that is not COMPLETED. So from the moment a
// re-upload began, readers were told 409 for an object that was finished, hashed and still on
// disk under its own internalName - and an upload that never completed left it that way for good.
// 21 objects on the development installation were in exactly that state, every one of them still
// carrying the md5Sum and internalName that proved it had been readable first.
//

#define BOOST_TEST_MODULE ObjectUploadStatusTest

// C++ includes
#include <optional>
#include <vector>

// Boost includes
#include <boost/test/included/unit_test.hpp>

// Euclid includes
#include <euclid/database/entity/esm/ObjectStatus.h>

using namespace Euclid::Database::Entity::ESM;

namespace {

    // Every status a row can actually be found in. UNKNOWN is included deliberately: it is what a
    // row written by an older version or a failed parse reads as, and an upload has to cope with
    // finding one.
    const std::vector<ObjectStatus> kAllStatuses{ObjectStatus::CREATED, ObjectStatus::UPLOADING,
                                                 ObjectStatus::UPLOADED, ObjectStatus::COMPLETED,
                                                 ObjectStatus::UNKNOWN};
}// namespace

BOOST_AUTO_TEST_SUITE(ObjectUploadStatusTest)

// ── The invariant ───────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(CreatingAnUploadNeverMakesAReadableObjectUnreadable) {

    // The property, over every status rather than the one case that was reported: if a reader
    // could have the object a moment before create-upload, it can still have it a moment after.
    for (const auto status: kAllStatuses) {
        const auto seeded = StatusForCreatedUpload(std::optional{status});

        const bool wasReadable = IsDownloadable(status);
        const bool stillReadable = IsDownloadable(seeded);

        BOOST_TEST_CONTEXT("existing status " << ObjectStatusToString(status)) {
            const bool preserved = !wasReadable || stillReadable;
            BOOST_TEST(preserved);
        }
    }
}

BOOST_AUTO_TEST_CASE(ReUploadingKeepsTheCompletedVersionServable) {

    // The reported case, stated on its own so a regression names itself: app-splitting re-uploads
    // a key it already published, and the parser reading that key must not start getting 409.
    const auto seeded = StatusForCreatedUpload(std::optional{ObjectStatus::COMPLETED});

    BOOST_TEST(ObjectStatusToString(seeded) == "COMPLETED");
    BOOST_TEST(IsDownloadable(seeded));
}

BOOST_AUTO_TEST_CASE(AnAbandonedReUploadLeavesTheObjectReadable) {

    // An upload that is never completed never writes the row again, so whatever create-upload
    // seeded is what the key keeps - forever. That is what turned a race into 21 dead objects,
    // and it is why this is about the seeded value rather than about closing a window quickly.
    const auto afterCreate = StatusForCreatedUpload(std::optional{ObjectStatus::COMPLETED});

    // No upload-part, no complete-upload: nothing else runs.
    BOOST_TEST(IsDownloadable(afterCreate));
}

// ── A first upload is the other half ────────────────────────────────────────

BOOST_AUTO_TEST_CASE(AFirstUploadStartsAtCreatedAndIsNotServed) {

    // Nothing to protect and nothing to read: a reader that arrives now must be refused rather
    // than handed a file that is still being written.
    const auto seeded = StatusForCreatedUpload(std::nullopt);

    BOOST_TEST(ObjectStatusToString(seeded) == "CREATED");
    BOOST_TEST(!IsDownloadable(seeded));
}

BOOST_AUTO_TEST_CASE(AnUploadOverAnUnfinishedUploadStaysUnreadable) {

    // Re-uploading over a key whose previous upload never finished has no good version to keep,
    // so it must not become readable either - preserving the status has to cut both ways.
    for (const auto status: {ObjectStatus::CREATED, ObjectStatus::UPLOADING, ObjectStatus::UPLOADED}) {
        BOOST_TEST_CONTEXT("existing status " << ObjectStatusToString(status)) {
            const bool readable = IsDownloadable(StatusForCreatedUpload(std::optional{status}));
            BOOST_TEST(!readable);
        }
    }
}

// ── What "readable" means ───────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(OnlyCompletedIsDownloadable) {

    // Pinned because two handlers ask this question and both used to spell it out themselves; a
    // third that spells it differently is how the gate drifts.
    for (const auto status: kAllStatuses) {
        BOOST_TEST_CONTEXT("status " << ObjectStatusToString(status)) {
            const bool expected = status == ObjectStatus::COMPLETED;
            BOOST_TEST(IsDownloadable(status) == expected);
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
