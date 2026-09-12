// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Mongo includes
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/json.hpp>
#include <mongocxx/options/replace.hpp>

// Euclid includes
#include <ExportImport.h>
#include <euclid/core/LogStream.h>

namespace Euclid::EMM {

    // Both of these take a Database::Collection rather than a mongocxx::collection, because export
    // and import have to work on whichever backend the process was pointed at. Reaching for
    // Database::client() - which is what they used to do - throws outright when the store is the
    // in-memory one, since a process using that has no connection pool to acquire from, and an
    // import of a perfectly good file then failed with "MongoDB not initialized".

    boost::json::array ExportCollection(const Database::Collection &collection) {
        boost::json::array docs;
        for (auto cursor = collection.find({}); const auto &doc: cursor) {
            docs.push_back(boost::json::parse(bsoncxx::to_json(doc, bsoncxx::ExtendedJsonMode::k_relaxed)));
        }
        return docs;
    }

    ImportOutcome ImportCollection(const Database::Collection &collection, const std::string &name, const boost::json::array &docs) {

        namespace basic = bsoncxx::builder::basic;

        ImportOutcome outcome;
        mongocxx::options::replace opts;
        opts.upsert(true);

        for (const auto &doc: docs) {
            try {
                const auto bsonDoc = bsoncxx::from_json(boost::json::serialize(doc));
                const auto idElement = bsonDoc.view()["_id"];
                // Without an "_id" there is nothing to upsert on, and inserting anyway would make a
                // second copy of the document on every re-import.
                if (!idElement) {
                    ++outcome.failed;
                    continue;
                }
                collection.replace_one(basic::make_document(basic::kvp("_id", idElement.get_value())), bsonDoc.view(), opts);
                ++outcome.imported;
            } catch (const std::exception &e) {
                // The name is passed in rather than asked of the collection: Database::Collection
                // names what it was opened as, and the caller already has it in hand.
                log_warning << "emm import: skipping malformed document, collection: " << name << ", error: " << e.what();
                ++outcome.failed;
            }
        }

        return outcome;
    }

}// namespace Euclid::EMM
