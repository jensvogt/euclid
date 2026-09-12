// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/8/26.
//

// C++ includes
#include <algorithm>
#include <chrono>
#include <regex>
#include <stdexcept>

// MongoDB includes
#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/types.hpp>
#include <bsoncxx/types/bson_value/value.hpp>
#include <bsoncxx/types/bson_value/view.hpp>

// Euclid includes
#include <euclid/database/emd/DocumentStore.h>

namespace Euclid::Database::Emd {

    using bsoncxx::builder::basic::kvp;
    using bsoncxx::builder::basic::make_document;

    namespace {

        // ── Values ───────────────────────────────────────────────────────────

        bool isNumeric(const bsoncxx::type type) {
            return type == bsoncxx::type::k_int32 || type == bsoncxx::type::k_int64 || type == bsoncxx::type::k_double;
        }

        double asDouble(const bsoncxx::types::bson_value::view &value) {
            switch (value.type()) {
                case bsoncxx::type::k_int32:
                    return value.get_int32().value;
                case bsoncxx::type::k_int64:
                    return static_cast<double>(value.get_int64().value);
                case bsoncxx::type::k_double:
                    return value.get_double();
                default:
                    return 0.0;
            }
        }

        long asLong(const bsoncxx::types::bson_value::view &value) {
            switch (value.type()) {
                case bsoncxx::type::k_int32:
                    return value.get_int32().value;
                case bsoncxx::type::k_int64:
                    return static_cast<long>(value.get_int64().value);
                case bsoncxx::type::k_double:
                    return static_cast<long>(value.get_double());
                default:
                    return 0;
            }
        }

        // Orders two values the way a sort has to: -1, 0 or 1. Numbers compare as numbers whatever
        // width they were stored at - an int32 written by one client and an int64 by another are
        // the same value, and a sort that said otherwise would interleave them.
        int compare(const bsoncxx::types::bson_value::view &left, const bsoncxx::types::bson_value::view &right) {

            if (isNumeric(left.type()) && isNumeric(right.type())) {
                const auto l = asDouble(left), r = asDouble(right);
                return l < r ? -1 : (l > r ? 1 : 0);
            }
            if (left.type() != right.type()) {
                // Different types never compare equal; the order between them is arbitrary but
                // stable, which is all a sort needs.
                return static_cast<int>(left.type()) < static_cast<int>(right.type()) ? -1 : 1;
            }

            switch (left.type()) {
                case bsoncxx::type::k_string: {
                    const auto l = std::string(left.get_string().value), r = std::string(right.get_string().value);
                    return l < r ? -1 : (l > r ? 1 : 0);
                }
                case bsoncxx::type::k_bool: {
                    const auto l = left.get_bool().value, r = right.get_bool().value;
                    return l == r ? 0 : (!l ? -1 : 1);
                }
                case bsoncxx::type::k_date: {
                    const auto l = left.get_date().to_int64(), r = right.get_date().to_int64();
                    return l < r ? -1 : (l > r ? 1 : 0);
                }
                case bsoncxx::type::k_oid: {
                    const auto l = left.get_oid().value.to_string(), r = right.get_oid().value.to_string();
                    return l < r ? -1 : (l > r ? 1 : 0);
                }
                case bsoncxx::type::k_null:
                    return 0;
                default:
                    return 0;
            }
        }

        bool equals(const bsoncxx::types::bson_value::view &left, const bsoncxx::types::bson_value::view &right) {
            if (isNumeric(left.type()) && isNumeric(right.type())) return compare(left, right) == 0;
            if (left.type() != right.type()) return false;
            return compare(left, right) == 0;
        }

        // ── Appending ────────────────────────────────────────────────────────

        // Appends a value taken out of one document into another.
        //
        // Not simply kvp(key, value): re-appending a bson_value::view that holds an *empty string*
        // yields a null, because a zero-length string arrives with no data pointer for the builder
        // to copy. Every document this store writes is rebuilt field by field, and euclid writes
        // empty strings everywhere - an unscoped namespace is one - so without this a row is
        // stored with nulls where its identity should be and can never be found again.
        void appendValue(bsoncxx::builder::basic::document &target, const std::string &key, const bsoncxx::types::bson_value::view &value) {
            if (value.type() == bsoncxx::type::k_string) {
                target.append(kvp(key, std::string(value.get_string().value)));
                return;
            }
            target.append(kvp(key, value));
        }

        void appendValue(bsoncxx::builder::basic::array &target, const bsoncxx::types::bson_value::view &value) {
            if (value.type() == bsoncxx::type::k_string) {
                target.append(std::string(value.get_string().value));
                return;
            }
            target.append(value);
        }

        // ── Owned values ─────────────────────────────────────────────────────

        // A value lifted out of a document that will not outlive it.
        //
        // Held as a one-field document rather than a bsoncxx::types::bson_value::value, which does
        // not survive an empty string: copying one out of a document and back in yields a null,
        // and a field that silently turns into null is not merely wrong - it cannot be found
        // again, because the filter that stored it looks for a string. euclid writes empty strings
        // constantly (an unscoped namespace is one), so this is the difference between a store
        // that works and one that loses rows.
        struct Owned {

            bsoncxx::document::value holder;

            [[nodiscard]]
            bsoncxx::types::bson_value::view view() const { return holder.view()["v"].get_value(); }
        };

        Owned own(const bsoncxx::types::bson_value::view &value) {
            bsoncxx::builder::basic::document holder;
            appendValue(holder, "v", value);
            return {holder.extract()};
        }

        // ── Paths ────────────────────────────────────────────────────────────

        // Every value a dotted path reaches. More than one when the path crosses an array, which
        // is MongoDB's rule and the one "accessKeys.accessKeyId" relies on: a user matches when
        // any of their keys does.
        void resolve(const bsoncxx::document::view &document, const std::string &path, std::vector<Owned> &found) {

            const auto dot = path.find('.');
            const auto head = dot == std::string::npos ? path : path.substr(0, dot);

            const auto element = document[head];
            if (!element) return;

            if (dot == std::string::npos) {
                found.push_back(own(element.get_value()));
                return;
            }

            const auto tail = path.substr(dot + 1);
            if (element.type() == bsoncxx::type::k_document) {
                resolve(element.get_document().value, tail, found);
            } else if (element.type() == bsoncxx::type::k_array) {
                for (const auto &entry: element.get_array().value) {
                    if (entry.type() == bsoncxx::type::k_document) resolve(entry.get_document().value, tail, found);
                }
            }
        }

        std::vector<Owned> valuesAt(const bsoncxx::document::view &document, const std::string &path) {
            std::vector<Owned> found;
            resolve(document, path, found);
            return found;
        }

        // ── Matching ─────────────────────────────────────────────────────────

        bool matches(const bsoncxx::document::view &document, const bsoncxx::document::view &filter);

        // Whether one field of a document satisfies one condition of a filter.
        bool matchesCondition(const std::vector<Owned> &values, const bsoncxx::document::element &condition) {

            const auto satisfied = [&values](const auto &predicate) {
                return std::ranges::any_of(values, [&predicate](const auto &value) { return predicate(value.view()); });
            };

            // An operator document - { "$gte": 5 } - rather than a value to be equal to. Told
            // apart the way the server tells them apart: by the leading dollar on the first key.
            if (condition.type() == bsoncxx::type::k_document) {
                const auto sub = condition.get_document().value;
                if (sub.begin() != sub.end() && std::string(sub.begin()->key()).starts_with("$")) {

                    for (const auto &op: sub) {
                        const auto name = std::string(op.key());
                        const auto operand = op.get_value();

                        if (name == "$eq") {
                            if (!satisfied([&](const auto &v) { return equals(v, operand); })) return false;
                        } else if (name == "$ne") {
                            if (satisfied([&](const auto &v) { return equals(v, operand); })) return false;
                            // A field that is absent is "not equal" to anything, which is what
                            // lets a filter exclude a status without excluding rows that predate
                            // the field.
                        } else if (name == "$gt") {
                            if (!satisfied([&](const auto &v) { return compare(v, operand) > 0; })) return false;
                        } else if (name == "$gte") {
                            if (!satisfied([&](const auto &v) { return compare(v, operand) >= 0; })) return false;
                        } else if (name == "$lt") {
                            if (!satisfied([&](const auto &v) { return compare(v, operand) < 0; })) return false;
                        } else if (name == "$lte") {
                            if (!satisfied([&](const auto &v) { return compare(v, operand) <= 0; })) return false;
                        } else if (name == "$in" || name == "$nin") {
                            bool anyIn = false;
                            for (const auto &candidate: operand.get_array().value) {
                                if (satisfied([&](const auto &v) { return equals(v, candidate.get_value()); })) {
                                    anyIn = true;
                                    break;
                                }
                            }
                            if (name == "$in" ? !anyIn : anyIn) return false;
                        } else if (name == "$exists") {
                            const bool wanted = operand.type() == bsoncxx::type::k_bool ? operand.get_bool().value : true;
                            if (values.empty() == wanted) return false;
                        } else if (name == "$regex") {
                            const std::regex expression(std::string(operand.get_string().value), std::regex::ECMAScript);
                            if (!satisfied([&](const auto &v) {
                                    return v.type() == bsoncxx::type::k_string
                                           && std::regex_search(std::string(v.get_string().value), expression);
                                })) {
                                return false;
                            }
                        } else if (name == "$size") {
                            // Only meaningful against the array itself, so it is checked before
                            // the path was flattened - see the caller.
                            return false;
                        } else {
                            throw std::runtime_error("EMD does not implement the query operator " + name);
                        }
                    }
                    return true;
                }
            }

            // A plain value: equal to it, or - for an array field - containing it.
            return satisfied([&](const auto &v) { return equals(v, condition.get_value()); });
        }

        bool matches(const bsoncxx::document::view &document, const bsoncxx::document::view &filter) {

            for (const auto &condition: filter) {
                const auto key = std::string(condition.key());

                if (key == "$and" || key == "$or" || key == "$nor") {
                    bool any = false, all = true;
                    for (const auto &branch: condition.get_array().value) {
                        const bool ok = matches(document, branch.get_document().value);
                        any = any || ok;
                        all = all && ok;
                    }
                    if (key == "$and" && !all) return false;
                    if (key == "$or" && !any) return false;
                    if (key == "$nor" && any) return false;
                    continue;
                }
                if (key.starts_with("$")) {
                    throw std::runtime_error("EMD does not implement the query operator " + key);
                }

                // $size looks at the array rather than at what it holds, so it is answered before
                // the path is flattened into the values inside.
                if (condition.type() == bsoncxx::type::k_document) {
                    if (const auto sub = condition.get_document().value; sub.begin() != sub.end() && std::string(sub.begin()->key()) == "$size") {
                        const auto element = document[key];
                        if (!element || element.type() != bsoncxx::type::k_array) return false;
                        const auto array = element.get_array().value;
                        if (std::distance(array.begin(), array.end()) != asLong(sub.begin()->get_value())) return false;
                        continue;
                    }
                }

                if (!matchesCondition(valuesAt(document, key), condition)) return false;
            }
            return true;
        }

        // ── Updating ─────────────────────────────────────────────────────────

        // Which element of an array the filter picked out, for the positional "$" operator. MongoDB
        // remembers the element that satisfied the query and updates that one; here the same answer
        // is reached by asking the question again - the first element of the array that satisfies
        // every condition the filter placed on it.
        //
        // The rule this relies on is MongoDB's own: "$" is only defined when the filter constrained
        // the array it names, and only ever refers to the first match. A filter that constrained
        // nothing gives no element rather than the zeroth, which is what MongoDB does too.
        std::optional<std::size_t> positionalIndex(const bsoncxx::document::view &document, const bsoncxx::document::view &filter,
                                                   const std::string &arrayField) {

            const auto element = document[arrayField];
            if (!element || element.type() != bsoncxx::type::k_array) return std::nullopt;

            const auto prefix = arrayField + ".";
            std::vector<bsoncxx::document::element> conditions;
            for (const auto &condition: filter) {
                if (std::string(condition.key()).starts_with(prefix)) conditions.push_back(condition);
            }
            if (conditions.empty()) return std::nullopt;

            std::size_t index = 0;
            for (const auto &entry: element.get_array().value) {
                if (entry.type() == bsoncxx::type::k_document) {

                    const auto candidate = entry.get_document().value;
                    if (std::ranges::all_of(conditions, [&candidate, &prefix](const auto &condition) {
                            return matchesCondition(valuesAt(candidate, std::string(condition.key()).substr(prefix.size())), condition);
                        })) {
                        return index;
                    }
                }
                ++index;
            }
            return std::nullopt;
        }

        // One element of an array, with the update applied to it: either replaced outright ("a.$")
        // or with one of its own fields set ("a.$.b").
        Owned positionalArray(const bsoncxx::types::bson_value::view &existing, const std::size_t index,
                              const std::string &subField, const bsoncxx::types::bson_value::view &value) {

            bsoncxx::builder::basic::array array;
            std::size_t at = 0;
            for (const auto &entry: existing.get_array().value) {

                if (at++ != index) {
                    appendValue(array, entry.get_value());
                    continue;
                }

                if (subField.empty()) {
                    appendValue(array, value);
                    continue;
                }

                bsoncxx::builder::basic::document replaced;
                bool seen = false;
                if (entry.type() == bsoncxx::type::k_document) {
                    for (const auto &field: entry.get_document().value) {
                        if (std::string(field.key()) == subField) {
                            appendValue(replaced, subField, value);
                            seen = true;
                            continue;
                        }
                        appendValue(replaced, std::string(field.key()), field.get_value());
                    }
                }
                if (!seen) appendValue(replaced, subField, value);
                array.append(replaced.view());
            }
            // Copied out while the builder is still alive - an array view does not own its bytes.
            return own(bsoncxx::types::bson_value::view(bsoncxx::types::b_array{array.view()}));
        }

        void requireTopLevel(const std::string &field, const std::string &op) {
            if (field.contains('.')) {
                // Refused rather than quietly setting a field whose name happens to contain a dot:
                // nothing in euclid updates a nested field, so this is a caller that has grown a
                // need the store has not, and finding out here beats finding out from the data.
                throw std::runtime_error("EMD does not implement " + op + " on the nested path " + field);
            }
        }

        // Applies one update document, producing the document that results. Nothing is modified in
        // place: BSON here is immutable, and rebuilding is also what makes "modified" honest -
        // the caller can compare what went in with what came out.
        bsoncxx::document::value applyUpdate(const bsoncxx::document::view &document, const bsoncxx::document::view &update, const bool inserting,
                                             const bsoncxx::document::view &filter = {}) {

            std::map<std::string, Owned> fields;
            for (const auto &element: document) fields.insert_or_assign(std::string(element.key()), own(element.get_value()));

            std::vector<std::string> removed;

            for (const auto &stage: update) {
                const auto op = std::string(stage.key());

                if (!op.starts_with("$")) {
                    throw std::runtime_error("EMD expects an update document of operators, not the field " + op);
                }
                const auto body = stage.get_document().value;

                if (op == "$set") {
                    for (const auto &field: body) {
                        const auto name = std::string(field.key());

                        // "instances.$" - the element of the array that the filter matched. EMM
                        // updates a module's instance record this way on every state change, so a
                        // store without it leaves an installation unable to report what is running.
                        if (const auto dollar = name.find(".$"); dollar != std::string::npos) {

                            const auto arrayField = name.substr(0, dollar);
                            const auto rest = name.substr(dollar + 2);
                            if (!rest.empty() && !rest.starts_with(".")) {
                                throw std::runtime_error("EMD does not implement " + op + " on the nested path " + name);
                            }

                            const auto existing = fields.find(arrayField);
                            const auto index = positionalIndex(document, filter, arrayField);

                            // No matching element is not an error: MongoDB matches no document at
                            // all in that case, and the caller (EMM) falls back to $push. Leaving
                            // the array alone is the same outcome.
                            if (index.has_value() && existing != fields.end()) {
                                fields.insert_or_assign(arrayField,
                                                        positionalArray(existing->second.view(), *index,
                                                                        rest.empty() ? std::string() : rest.substr(1),
                                                                        field.get_value()));
                            }
                            continue;
                        }

                        requireTopLevel(name, op);
                        fields.insert_or_assign(name, own(field.get_value()));
                    }
                } else if (op == "$setOnInsert") {
                    if (!inserting) continue;
                    for (const auto &field: body) {
                        requireTopLevel(std::string(field.key()), op);
                        fields.insert_or_assign(std::string(field.key()), own(field.get_value()));
                    }
                } else if (op == "$currentDate") {
                    const auto now = bsoncxx::types::b_date{std::chrono::system_clock::now()};
                    for (const auto &field: body) {
                        requireTopLevel(std::string(field.key()), op);
                        fields.insert_or_assign(std::string(field.key()), own(bsoncxx::types::bson_value::view(now)));
                    }
                } else if (op == "$inc") {
                    for (const auto &field: body) {
                        const auto name = std::string(field.key());
                        requireTopLevel(name, op);

                        const auto existing = fields.find(name);
                        const bool wasDouble = existing != fields.end() && existing->second.view().type() == bsoncxx::type::k_double;
                        if (wasDouble || field.get_value().type() == bsoncxx::type::k_double) {
                            const double base = existing != fields.end() ? asDouble(existing->second.view()) : 0.0;
                            fields.insert_or_assign(name, own(bsoncxx::types::bson_value::view(bsoncxx::types::b_double{base + asDouble(field.get_value())})));
                        } else {
                            const long base = existing != fields.end() ? asLong(existing->second.view()) : 0;
                            fields.insert_or_assign(name, own(bsoncxx::types::bson_value::view(bsoncxx::types::b_int64{base + asLong(field.get_value())})));
                        }
                    }
                } else if (op == "$unset") {
                    for (const auto &field: body) {
                        fields.erase(std::string(field.key()));
                    }
                } else if (op == "$push" || op == "$pull") {
                    for (const auto &field: body) {
                        const auto name = std::string(field.key());
                        requireTopLevel(name, op);

                        bsoncxx::builder::basic::array array;
                        if (const auto existing = fields.find(name); existing != fields.end() && existing->second.view().type() == bsoncxx::type::k_array) {
                            for (const auto &entry: existing->second.view().get_array().value) {
                                if (op == "$pull" && equals(entry.get_value(), field.get_value())) continue;
                                appendValue(array, entry.get_value());
                            }
                        }
                        if (op == "$push") appendValue(array, field.get_value());
                        fields.insert_or_assign(name, own(bsoncxx::types::bson_value::view(bsoncxx::types::b_array{array.view()})));
                    }
                } else {
                    throw std::runtime_error("EMD does not implement the update operator " + op);
                }
            }

            bsoncxx::builder::basic::document result;
            for (const auto &[name, value]: fields) appendValue(result, name, value.view());
            return result.extract();
        }

        // The document an upsert starts from: the equality conditions of the filter, which is what
        // MongoDB seeds a created document with. It is why "upsert by name" stores the name
        // without anybody having to put it in the update as well.
        bsoncxx::document::value seedFromFilter(const bsoncxx::document::view &filter) {

            bsoncxx::builder::basic::document seed;
            for (const auto &condition: filter) {
                const auto key = std::string(condition.key());
                if (key.starts_with("$") || key.contains('.')) continue;
                if (condition.type() == bsoncxx::type::k_document) {
                    if (const auto sub = condition.get_document().value; sub.begin() != sub.end() && std::string(sub.begin()->key()).starts_with("$")) {
                        continue;// an operator, not a value the new document should carry
                    }
                }
                appendValue(seed, key, condition.get_value());
            }
            return seed.extract();
        }

        bsoncxx::document::value withId(const bsoncxx::document::view &document, const bsoncxx::oid &id) {

            bsoncxx::builder::basic::document result;
            result.append(kvp("_id", id));
            for (const auto &element: document) {
                if (std::string(element.key()) == "_id") continue;
                appendValue(result, std::string(element.key()), element.get_value());
            }
            return result.extract();
        }

        /**
         * @brief The "_id" of a document, or of an equality filter on one, when it is an ObjectId.
         */
        std::optional<bsoncxx::oid> oidOf(const bsoncxx::document::view &document) {
            const auto element = document["_id"];
            if (element && element.type() == bsoncxx::type::k_oid) return element.get_oid().value;
            return std::nullopt;
        }

        /**
         * @brief The id an upsert should create a document under.
         *
         * @par
         * The one the caller named - in the document it is writing, or in the filter that did not
         * match - and a fresh one only when neither says. MongoDB does the same, and an EMM import
         * depends on it: a restored document has to keep the id it was exported under, or importing
         * the same file a second time matches nothing and stores another copy of everything.
         */
        bsoncxx::oid upsertId(const bsoncxx::document::view &document, const bsoncxx::document::view &filter) {
            if (const auto fromDocument = oidOf(document); fromDocument.has_value()) return *fromDocument;
            return oidOf(filter).value_or(bsoncxx::oid{});
        }

        bool sameDocument(const bsoncxx::document::view &left, const bsoncxx::document::view &right) {
            return left.length() == right.length() && std::memcmp(left.data(), right.data(), left.length()) == 0;
        }

    }// namespace

    // ── DocumentStore ────────────────────────────────────────────────────────

    void DocumentStore::CheckUnique(const Collection &collection, const bsoncxx::document::view document, const long replacingIndex) {

        for (const auto &keys: collection.uniqueKeys) {

            bsoncxx::builder::basic::document filter;
            bool complete = true;
            for (const auto &key: keys) {
                const auto values = valuesAt(document, key);
                if (values.empty()) {
                    complete = false;
                    break;
                }
                appendValue(filter, key, values.front().view());
            }
            // A document missing part of the key is not held to the index, the same way a sparse
            // key behaves in the real thing.
            if (!complete) continue;

            const auto view = filter.view();
            for (std::size_t i = 0; i < collection.documents.size(); ++i) {
                if (static_cast<long>(i) == replacingIndex) continue;
                if (matches(collection.documents[i].view(), view)) {
                    std::string names;
                    for (const auto &key: keys) names += (names.empty() ? "" : ", ") + key;
                    throw std::runtime_error("E11000 duplicate key: another document already has this " + names);
                }
            }
        }
    }

    std::optional<bsoncxx::document::value> DocumentStore::FindOne(const std::string &collection, const bsoncxx::document::view filter) const {

        std::lock_guard lock(_mutex);
        const auto it = _collections.find(collection);
        if (it == _collections.end()) return std::nullopt;

        for (const auto &document: it->second.documents) {
            if (matches(document.view(), filter)) return document;
        }
        return std::nullopt;
    }

    std::vector<bsoncxx::document::value> DocumentStore::Find(const std::string &collection, const bsoncxx::document::view filter,
                                                              const FindOptions &options) const {

        std::lock_guard lock(_mutex);

        std::vector<bsoncxx::document::value> result;
        if (const auto it = _collections.find(collection); it != _collections.end()) {
            for (const auto &document: it->second.documents) {
                if (matches(document.view(), filter)) result.push_back(document);
            }
        }

        if (options.sort.has_value()) {
            const auto sort = options.sort->view();
            std::ranges::stable_sort(result, [&sort](const auto &left, const auto &right) {
                for (const auto &key: sort) {
                    const auto name = std::string(key.key());
                    const auto ascending = asLong(key.get_value()) >= 0;

                    const auto leftValues = valuesAt(left.view(), name);
                    const auto rightValues = valuesAt(right.view(), name);

                    // A document missing the sort field orders before one that has it, which is
                    // where a null sorts in MongoDB too.
                    if (leftValues.empty() || rightValues.empty()) {
                        if (leftValues.size() == rightValues.size()) continue;
                        return ascending ? leftValues.empty() : rightValues.empty();
                    }
                    if (const auto order = compare(leftValues.front().view(), rightValues.front().view()); order != 0) {
                        return ascending ? order < 0 : order > 0;
                    }
                }
                return false;
            });
        }

        if (options.skip > 0) {
            const auto skip = std::min<std::size_t>(static_cast<std::size_t>(options.skip), result.size());
            result.erase(result.begin(), result.begin() + static_cast<long>(skip));
        }
        if (options.limit > 0 && static_cast<long>(result.size()) > options.limit) {
            // erase() rather than resize(): a document has no default state to grow into, and
            // shrinking is all this ever does.
            result.erase(result.begin() + options.limit, result.end());
        }
        return result;
    }

    bsoncxx::oid DocumentStore::InsertOne(const std::string &collection, const bsoncxx::document::view document) {

        std::lock_guard lock(_mutex);
        auto &target = _collections[collection];

        const auto id = document["_id"] && document["_id"].type() == bsoncxx::type::k_oid
                                ? document["_id"].get_oid().value
                                : bsoncxx::oid{};
        auto stored = withId(document, id);

        CheckUnique(target, stored.view(), -1);
        target.documents.push_back(std::move(stored));
        return id;
    }

    UpdateResult DocumentStore::UpdateOne(const std::string &collection, const bsoncxx::document::view filter,
                                          const bsoncxx::document::view update, const bool upsert) {

        std::lock_guard lock(_mutex);
        auto &target = _collections[collection];

        for (std::size_t i = 0; i < target.documents.size(); ++i) {
            if (!matches(target.documents[i].view(), filter)) continue;

            auto updated = applyUpdate(target.documents[i].view(), update, false, filter);
            const bool changed = !sameDocument(target.documents[i].view(), updated.view());
            if (changed) {
                CheckUnique(target, updated.view(), static_cast<long>(i));
                target.documents[i] = std::move(updated);
            }
            return {.matched = 1, .modified = changed ? 1L : 0L};
        }

        if (!upsert) return {};

        const auto seed = seedFromFilter(filter);
        const auto applied = applyUpdate(seed.view(), update, true);
        auto created = withId(applied.view(), upsertId(applied.view(), filter));
        const auto id = created["_id"].get_oid().value;

        CheckUnique(target, created.view(), -1);
        target.documents.push_back(std::move(created));
        return {.matched = 0, .modified = 0, .upsertedId = id};
    }

    UpdateResult DocumentStore::UpdateMany(const std::string &collection, const bsoncxx::document::view filter,
                                           const bsoncxx::document::view update) {

        std::lock_guard lock(_mutex);
        auto &target = _collections[collection];

        UpdateResult result;
        for (std::size_t i = 0; i < target.documents.size(); ++i) {
            if (!matches(target.documents[i].view(), filter)) continue;

            result.matched++;
            auto updated = applyUpdate(target.documents[i].view(), update, false, filter);
            if (!sameDocument(target.documents[i].view(), updated.view())) {
                CheckUnique(target, updated.view(), static_cast<long>(i));
                target.documents[i] = std::move(updated);
                result.modified++;
            }
        }
        return result;
    }

    UpdateResult DocumentStore::ReplaceOne(const std::string &collection, const bsoncxx::document::view filter,
                                           const bsoncxx::document::view replacement, const bool upsert) {

        std::lock_guard lock(_mutex);
        auto &target = _collections[collection];

        for (std::size_t i = 0; i < target.documents.size(); ++i) {
            if (!matches(target.documents[i].view(), filter)) continue;

            // The id is the document's identity and survives a replacement, whatever the
            // replacement says about it.
            const auto id = target.documents[i].view()["_id"].get_oid().value;
            auto stored = withId(replacement, id);
            const bool changed = !sameDocument(target.documents[i].view(), stored.view());
            if (changed) {
                CheckUnique(target, stored.view(), static_cast<long>(i));
                target.documents[i] = std::move(stored);
            }
            return {.matched = 1, .modified = changed ? 1L : 0L};
        }

        if (!upsert) return {};

        auto created = withId(replacement, upsertId(replacement, filter));
        const auto id = created["_id"].get_oid().value;
        CheckUnique(target, created.view(), -1);
        target.documents.push_back(std::move(created));
        return {.matched = 0, .modified = 0, .upsertedId = id};
    }

    std::optional<bsoncxx::document::value> DocumentStore::FindOneAndUpdate(const std::string &collection, const bsoncxx::document::view filter,
                                                                           const bsoncxx::document::view update, const bool upsert, const bool returnAfter) {

        std::lock_guard lock(_mutex);
        auto &target = _collections[collection];

        for (std::size_t i = 0; i < target.documents.size(); ++i) {
            if (!matches(target.documents[i].view(), filter)) continue;

            const auto before = target.documents[i];
            auto updated = applyUpdate(before.view(), update, false, filter);
            CheckUnique(target, updated.view(), static_cast<long>(i));
            target.documents[i] = updated;
            return returnAfter ? updated : before;
        }

        if (!upsert) return std::nullopt;

        const auto seed = seedFromFilter(filter);
        auto created = withId(applyUpdate(seed.view(), update, true).view(), bsoncxx::oid{});
        CheckUnique(target, created.view(), -1);
        target.documents.push_back(created);

        // Nothing existed before, so "before" is nothing - which is what the driver returns, and
        // why every repository here asks for the document after.
        return returnAfter ? std::optional{created} : std::nullopt;
    }

    std::optional<bsoncxx::document::value> DocumentStore::FindOneAndDelete(const std::string &collection, const bsoncxx::document::view filter) {

        std::lock_guard lock(_mutex);
        const auto it = _collections.find(collection);
        if (it == _collections.end()) return std::nullopt;

        for (auto document = it->second.documents.begin(); document != it->second.documents.end(); ++document) {
            if (!matches(document->view(), filter)) continue;

            auto removed = *document;
            it->second.documents.erase(document);
            return removed;
        }
        return std::nullopt;
    }

    long DocumentStore::Delete(const std::string &collection, const bsoncxx::document::view filter, const bool single) {

        std::lock_guard lock(_mutex);
        const auto it = _collections.find(collection);
        if (it == _collections.end()) return 0;

        long deleted = 0;
        auto &documents = it->second.documents;
        for (auto document = documents.begin(); document != documents.end();) {
            if (!matches(document->view(), filter)) {
                ++document;
                continue;
            }
            document = documents.erase(document);
            deleted++;
            if (single) break;
        }
        return deleted;
    }

    long DocumentStore::CountDocuments(const std::string &collection, const bsoncxx::document::view filter) const {

        std::lock_guard lock(_mutex);
        const auto it = _collections.find(collection);
        if (it == _collections.end()) return 0;

        return std::ranges::count_if(it->second.documents, [&filter](const auto &document) {
            return matches(document.view(), filter);
        });
    }

    std::vector<GroupCounts> DocumentStore::GroupCount(const std::string &collection, const bsoncxx::document::view filter,
                                                       const std::vector<std::string> &groupFields, const std::string &sumField) const {

        std::lock_guard lock(_mutex);

        std::vector<GroupCounts> groups;
        const auto it = _collections.find(collection);
        if (it == _collections.end()) return groups;

        for (const auto &document: it->second.documents) {
            if (!matches(document.view(), filter)) continue;

            std::vector<std::string> key;
            key.reserve(groupFields.size());
            for (const auto &field: groupFields) {
                const auto values = valuesAt(document.view(), field);
                key.push_back(values.empty() || values.front().view().type() != bsoncxx::type::k_string
                                      ? std::string()
                                      : std::string(values.front().view().get_string().value));
            }

            const auto existing = std::ranges::find_if(groups, [&key](const auto &group) { return group.key == key; });
            auto &group = existing != groups.end() ? *existing : groups.emplace_back(GroupCounts{.key = key});

            group.count++;
            if (!sumField.empty()) {
                if (const auto values = valuesAt(document.view(), sumField); !values.empty()) {
                    group.sum += asLong(values.front().view());
                }
            }
        }
        return groups;
    }

    void DocumentStore::CreateIndex(const std::string &collection, const bsoncxx::document::view keys, const bool unique) {

        std::lock_guard lock(_mutex);
        auto &target = _collections[collection];
        if (!unique) return;

        std::vector<std::string> fields;
        for (const auto &key: keys) fields.emplace_back(key.key());

        if (std::ranges::find(target.uniqueKeys, fields) == target.uniqueKeys.end()) {
            target.uniqueKeys.push_back(std::move(fields));
        }
    }

    void DocumentStore::Clear() {
        std::lock_guard lock(_mutex);
        _collections.clear();
    }

    std::vector<std::string> DocumentStore::Collections() const {

        std::lock_guard lock(_mutex);
        std::vector<std::string> names;
        for (const auto &[name, collection]: _collections) {
            if (!collection.documents.empty()) names.push_back(name);
        }
        return names;
    }

    long DocumentStore::Size(const std::string &collection) const {

        std::lock_guard lock(_mutex);
        const auto it = _collections.find(collection);
        return it == _collections.end() ? 0 : static_cast<long>(it->second.documents.size());
    }

}// namespace Euclid::Database::Emd
