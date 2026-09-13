// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <bsoncxx/types.hpp>

// Euclid includes
#include <euclid/database/entity/esm/PurgeJob.h>

namespace Euclid::Database::Entity::ESM {

    using std::chrono::system_clock;

    namespace {

        bsoncxx::types::b_date asDate(const system_clock::time_point &when) {
            return bsoncxx::types::b_date{std::chrono::duration_cast<std::chrono::milliseconds>(when.time_since_epoch())};
        }

    }// namespace

    bsoncxx::document::value PurgeJob::toDocument() const {

        return bsoncxx::builder::basic::make_document(
                bsoncxx::builder::basic::kvp("jobId", jobId),
                bsoncxx::builder::basic::kvp("bucketErn", bucketErn),
                bsoncxx::builder::basic::kvp("prefix", prefix),
                bsoncxx::builder::basic::kvp("deleteBucket", deleteBucket),
                bsoncxx::builder::basic::kvp("userId", userId),
                bsoncxx::builder::basic::kvp("claimedBy", claimedBy),
                bsoncxx::builder::basic::kvp("claimedAt", asDate(claimedAt)),
                bsoncxx::builder::basic::kvp("removedObjects", static_cast<std::int64_t>(removedObjects)),
                bsoncxx::builder::basic::kvp("removedSize", static_cast<std::int64_t>(removedSize)),
                bsoncxx::builder::basic::kvp("created", asDate(created)),
                bsoncxx::builder::basic::kvp("modified", asDate(modified)));
    }

    PurgeJob PurgeJob::fromDocument(const std::optional<bsoncxx::document::view> &document) {
        if (!document) return {};

        PurgeJob job;
        for (const auto &field: *document) {
            if (const auto key = field.key(); key == "_id") job.oid = field.get_oid().value.to_string();
            else if (key == "jobId") job.jobId = std::string(field.get_string().value);
            else if (key == "bucketErn") job.bucketErn = std::string(field.get_string().value);
            else if (key == "prefix") job.prefix = std::string(field.get_string().value);
            else if (key == "deleteBucket") job.deleteBucket = field.get_bool().value;
            else if (key == "userId") job.userId = std::string(field.get_string().value);
            else if (key == "claimedBy") job.claimedBy = std::string(field.get_string().value);
            else if (key == "claimedAt") job.claimedAt = system_clock::time_point{field.get_date().value};
            // Written as int64, but a document from a euclid that wrote int32 still has to read -
            // the same widening every other counter in this schema allows for.
            else if (key == "removedObjects") job.removedObjects = field.type() == bsoncxx::type::k_int32 ? field.get_int32().value : field.get_int64().value;
            else if (key == "removedSize") job.removedSize = field.type() == bsoncxx::type::k_int32 ? field.get_int32().value : field.get_int64().value;
            else if (key == "created") job.created = system_clock::time_point{field.get_date().value};
            else if (key == "modified") job.modified = system_clock::time_point{field.get_date().value};
        }
        return job;
    }

}// namespace Euclid::Database::Entity::ESM
