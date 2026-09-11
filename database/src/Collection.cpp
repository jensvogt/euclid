// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/8/26.
//

// C++ includes
#include <stdexcept>

// MongoDB includes
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>

// Euclid includes
#include <euclid/database/Collection.h>

namespace Euclid::Database {

    using bsoncxx::builder::basic::kvp;
    using bsoncxx::builder::basic::make_document;

    namespace {

        // Everything a cursor yielded, copied out of it. The in-memory store hands back values
        // already, and a MongoDB cursor can only be walked once - so both sides end up here, and a
        // caller can iterate the result as many times as it likes.
        std::vector<bsoncxx::document::value> drain(mongocxx::cursor cursor) {
            std::vector<bsoncxx::document::value> documents;
            for (auto &&document: cursor) documents.emplace_back(document);
            return documents;
        }

    }// namespace

    Collection::Collection(mongocxx::pool::entry entry, const std::string &databaseName, const std::string &name)
        : _entry(std::move(entry)), _name(name) {
        _collection = (**_entry)[databaseName][name];
    }

    Collection::Collection(std::shared_ptr<Emd::IDocumentStore> store, std::string name)
        : _store(std::move(store)), _name(std::move(name)) {}

    std::optional<bsoncxx::document::value> Collection::find_one(const bsoncxx::document::view_or_value filter) const {

        if (_collection.has_value()) {
            auto found = _collection->find_one(filter.view());
            if (!found) return std::nullopt;
            return bsoncxx::document::value(*found);
        }
        return _store->FindOne(_name, filter.view());
    }

    DocumentCursor Collection::find(const bsoncxx::document::view_or_value filter, const mongocxx::options::find &options) const {

        if (_collection.has_value()) {
            return DocumentCursor(drain(_collection->find(filter.view(), options)));
        }

        Emd::FindOptions storeOptions;
        if (options.sort()) storeOptions.sort = bsoncxx::document::value(options.sort()->view());
        if (options.limit()) storeOptions.limit = static_cast<long>(*options.limit());
        if (options.skip()) storeOptions.skip = static_cast<long>(*options.skip());

        return DocumentCursor(_store->Find(_name, filter.view(), storeOptions));
    }

    std::optional<bsoncxx::oid> Collection::insert_one(const bsoncxx::document::view_or_value document) const {

        if (_collection.has_value()) {
            const auto result = _collection->insert_one(document.view());
            if (!result) return std::nullopt;
            const auto id = result->inserted_id();
            if (id.type() != bsoncxx::type::k_oid) return std::nullopt;
            return id.get_oid().value;
        }
        return _store->InsertOne(_name, document.view());
    }

    std::optional<UpdateOutcome> Collection::update_one(const bsoncxx::document::view_or_value filter, const bsoncxx::document::view_or_value update,
                                                        const mongocxx::options::update &options) const {

        if (_collection.has_value()) {
            const auto result = _collection->update_one(filter.view(), update.view(), options);
            if (!result) return std::nullopt;
            return UpdateOutcome(result->matched_count(), result->modified_count(), std::nullopt);
        }

        const auto result = _store->UpdateOne(_name, filter.view(), update.view(), options.upsert().value_or(false));
        return UpdateOutcome(result.matched, result.modified, result.upsertedId);
    }

    std::optional<UpdateOutcome> Collection::update_many(const bsoncxx::document::view_or_value filter, const bsoncxx::document::view_or_value update,
                                                         const mongocxx::options::update &options) const {

        if (_collection.has_value()) {
            const auto result = _collection->update_many(filter.view(), update.view(), options);
            if (!result) return std::nullopt;
            return UpdateOutcome(result->matched_count(), result->modified_count(), std::nullopt);
        }

        const auto result = _store->UpdateMany(_name, filter.view(), update.view());
        return UpdateOutcome(result.matched, result.modified, result.upsertedId);
    }

    std::optional<UpdateOutcome> Collection::update_many(const bsoncxx::document::view_or_value filter, const mongocxx::pipeline &update) const {

        if (!_collection.has_value()) {
            throw std::runtime_error("The in-memory backend does not run pipeline updates, collection: " + _name);
        }
        const auto result = _collection->update_many(filter.view(), update);
        if (!result) return std::nullopt;
        return UpdateOutcome(result->matched_count(), result->modified_count(), std::nullopt);
    }

    std::optional<UpdateOutcome> Collection::replace_one(const bsoncxx::document::view_or_value filter, const bsoncxx::document::view_or_value replacement,
                                                         const mongocxx::options::replace &options) const {

        if (_collection.has_value()) {
            const auto result = _collection->replace_one(filter.view(), replacement.view(), options);
            if (!result) return std::nullopt;
            return UpdateOutcome(result->matched_count(), result->modified_count(), std::nullopt);
        }

        const auto result = _store->ReplaceOne(_name, filter.view(), replacement.view(), options.upsert().value_or(false));
        return UpdateOutcome(result.matched, result.modified, result.upsertedId);
    }

    std::optional<bsoncxx::document::value> Collection::find_one_and_update(const bsoncxx::document::view_or_value filter,
                                                                            const bsoncxx::document::view_or_value update,
                                                                            const mongocxx::options::find_one_and_update &options) const {

        if (_collection.has_value()) {
            auto found = _collection->find_one_and_update(filter.view(), update.view(), options);
            if (!found) return std::nullopt;
            return bsoncxx::document::value(*found);
        }

        const bool returnAfter = options.return_document().has_value()
                                 && *options.return_document() == mongocxx::options::return_document::k_after;
        return _store->FindOneAndUpdate(_name, filter.view(), update.view(), options.upsert().value_or(false), returnAfter);
    }

    std::optional<bsoncxx::document::value> Collection::find_one_and_delete(const bsoncxx::document::view_or_value filter) const {

        if (_collection.has_value()) {
            auto found = _collection->find_one_and_delete(filter.view());
            if (!found) return std::nullopt;
            return bsoncxx::document::value(*found);
        }
        return _store->FindOneAndDelete(_name, filter.view());
    }

    std::optional<DeleteOutcome> Collection::delete_one(const bsoncxx::document::view_or_value filter) const {

        if (_collection.has_value()) {
            const auto result = _collection->delete_one(filter.view());
            if (!result) return std::nullopt;
            return DeleteOutcome(result->deleted_count());
        }
        return DeleteOutcome(_store->Delete(_name, filter.view(), true));
    }

    std::optional<DeleteOutcome> Collection::delete_many(const bsoncxx::document::view_or_value filter) const {

        if (_collection.has_value()) {
            const auto result = _collection->delete_many(filter.view());
            if (!result) return std::nullopt;
            return DeleteOutcome(result->deleted_count());
        }
        return DeleteOutcome(_store->Delete(_name, filter.view(), false));
    }

    std::int64_t Collection::count_documents(const bsoncxx::document::view_or_value filter) const {

        if (_collection.has_value()) return _collection->count_documents(filter.view());
        return _store->CountDocuments(_name, filter.view());
    }

    void Collection::create_index(const bsoncxx::document::view_or_value keys, mongocxx::options::index &options) const {

        if (_collection.has_value()) {
            _collection->create_index(keys.view(), options);
            return;
        }
        _store->CreateIndex(_name, keys.view(), options.unique().value_or(false));
    }

    void Collection::create_index(const bsoncxx::document::view_or_value keys) const {
        mongocxx::options::index options;
        create_index(keys, options);
    }

    DocumentCursor Collection::aggregate(const mongocxx::pipeline &pipeline) const {

        if (!_collection.has_value()) {
            // Said rather than answered with nothing: an empty result would read as "no data",
            // and a monitoring graph that is empty because the backend cannot aggregate looks
            // exactly like one that is empty because nothing happened.
            throw std::runtime_error("The in-memory backend does not run aggregation pipelines, collection: " + _name);
        }
        return DocumentCursor(drain(_collection->aggregate(pipeline)));
    }

    std::vector<Emd::GroupCounts> Collection::group_count(const bsoncxx::document::view_or_value filter, const std::vector<std::string> &groupFields,
                                                          const std::string &sumField) const {

        if (!_collection.has_value()) {
            return _store->GroupCount(_name, filter.view(), groupFields, sumField);
        }

        // The same grouping, asked of the server. The identifier is a document of the grouped
        // fields even when there is only one, so both backends produce the same shape and the
        // caller reads them the same way.
        bsoncxx::builder::basic::document id;
        for (const auto &field: groupFields) id.append(kvp(field, "$" + field));

        bsoncxx::builder::basic::document group;
        group.append(kvp("_id", id.extract()));
        group.append(kvp("count", make_document(kvp("$sum", 1))));
        if (!sumField.empty()) {
            group.append(kvp("sum", make_document(kvp("$sum", "$" + sumField))));
        }

        mongocxx::pipeline pipeline;
        if (filter.view().begin() != filter.view().end()) pipeline.match(filter.view());
        pipeline.group(group.extract());

        std::vector<Emd::GroupCounts> groups;
        for (auto cursor = _collection->aggregate(pipeline); auto document: cursor) {

            Emd::GroupCounts counts;
            const auto identifier = document["_id"].get_document().value;
            for (const auto &field: groupFields) {
                const auto element = identifier[field];
                counts.key.push_back(element && element.type() == bsoncxx::type::k_string ? std::string(element.get_string().value) : std::string());
            }

            if (const auto count = document["count"]; count) {
                counts.count = count.type() == bsoncxx::type::k_int32 ? count.get_int32().value : static_cast<long>(count.get_int64().value);
            }
            if (const auto sum = document["sum"]; sum) {
                switch (sum.type()) {
                    case bsoncxx::type::k_int32:
                        counts.sum = sum.get_int32().value;
                        break;
                    case bsoncxx::type::k_int64:
                        counts.sum = static_cast<long>(sum.get_int64().value);
                        break;
                    case bsoncxx::type::k_double:
                        counts.sum = static_cast<long>(sum.get_double().value);
                        break;
                    default:
                        break;
                }
            }
            groups.push_back(std::move(counts));
        }
        return groups;
    }

}// namespace Euclid::Database
