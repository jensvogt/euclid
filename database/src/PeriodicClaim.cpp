// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// C++ includes
#include <exception>
#include <mutex>

// MongoDB includes
#include <bsoncxx/builder/basic/document.hpp>
#include <mongocxx/options/index.hpp>
#include <mongocxx/options/update.hpp>

// Euclid includes
#include <euclid/core/LogStream.h>
#include <euclid/database/Database.h>
#include <euclid/database/PeriodicClaim.h>

namespace Euclid::Database {

    using bsoncxx::builder::basic::kvp;
    using bsoncxx::builder::basic::make_document;

    namespace {

        constexpr auto PERIODIC_COLLECTION = "periodic_claim";

        // One document per job, enforced. Without it two instances seeding the same job at the
        // same moment leave two documents behind, and from then on two of them can claim the same
        // interval - each updating its own copy, neither seeing the other.
        void ensureUnique(const Collection &collection) {
            static std::once_flag once;
            std::call_once(once, [&collection] {
                try {
                    mongocxx::options::index options;
                    options.unique(true);
                    collection.create_index(make_document(kvp("name", 1)), options);
                } catch (const std::exception &e) {
                    log_warning << "Could not create the periodic claim index, error: " << e.what();
                }
            });
        }

    }// namespace

    bool ClaimPeriodicRun(const std::string &name, const std::chrono::seconds interval) {

        try {
            auto collection = Database::instance().collection(PERIODIC_COLLECTION);
            ensureUnique(collection);

            const auto now = std::chrono::system_clock::now();
            const bsoncxx::types::b_date nowDate{std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch())};
            const bsoncxx::types::b_date nextDate{
                    std::chrono::duration_cast<std::chrono::milliseconds>((now + interval).time_since_epoch())};

            // The claim is the write: whoever's update matches has moved the job into the future,
            // and every other instance's filter no longer does. Exactly one is told to go ahead,
            // with no lock anybody has to remember to release - and an instance that dies holding
            // a claim only means the job waits for the next interval, which for a periodic
            // correction is the right failure.
            const auto due = make_document(kvp("name", name), kvp("nextRunAt", make_document(kvp("$lte", nowDate))));
            const auto claim = make_document(kvp("$set", make_document(kvp("nextRunAt", nextDate), kvp("claimedAt", nowDate))));

            if (const auto outcome = collection.update_one(due.view(), claim.view());
                outcome && outcome->modified_count() > 0) {
                return true;
            }

            // Either somebody else has it, or the job has never run here. Seeding is its own
            // write, and deliberately not an upsert of the filter above: that filter is not an
            // equality on the name, so an upsert of it would insert a second document for a job
            // that merely is not due yet. $setOnInsert leaves a job that already exists alone, so
            // this cannot move somebody else's claim.
            const auto seed = make_document(kvp("$setOnInsert", make_document(kvp("name", name), kvp("nextRunAt", nowDate))));
            mongocxx::options::update options;
            options.upsert(true);
            std::ignore = collection.update_one(make_document(kvp("name", name)).view(), seed.view(), options);

            // Not claimed this time either way: a job seeded as due now is claimed by whichever
            // instance asks next, which keeps the first run on the same footing as every other.
            return false;

        } catch (const std::exception &e) {
            // Refused rather than granted. Two instances doing the same recount is waste; a claim
            // that says yes because the database could not be asked is every instance doing it at
            // once, which is what this exists to prevent.
            log_error << "Periodic claim failed, job: " << name << ", error: " << e.what();
            return false;
        }
    }

}// namespace Euclid::Database
