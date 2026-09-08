//
// Created by vogje01 on 9/8/26.
//

// C++ includes
#include <string>
#include <vector>

// MongoDB includes
#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>

// Euclid includes
#include <EmdServer.h>
#include <euclid/database/emd/Protocol.h>

namespace Euclid::EMD {

    namespace P = Database::Emd::Protocol;

    using bsoncxx::builder::basic::kvp;
    using bsoncxx::builder::basic::make_document;
    using Database::Emd::FindOptions;

    namespace {

        // The request document, read straight out of the body. No copy and no parse: what arrives
        // is already BSON, which is the reason this module speaks it rather than JSON.
        bsoncxx::document::view requestOf(const request<string_body> &req) {
            if (req.body().size() < 5) return {};
            return {reinterpret_cast<const std::uint8_t *>(req.body().data()), req.body().size()};
        }

        std::string stringField(const bsoncxx::document::view &document, const char *name) {
            const auto element = document[name];
            return element && element.type() == bsoncxx::type::k_string ? std::string(element.get_string().value) : std::string();
        }

        bsoncxx::document::view documentField(const bsoncxx::document::view &document, const char *name) {
            const auto element = document[name];
            return element && element.type() == bsoncxx::type::k_document ? element.get_document().value : bsoncxx::document::view{};
        }

        bool boolField(const bsoncxx::document::view &document, const char *name) {
            const auto element = document[name];
            return element && element.type() == bsoncxx::type::k_bool && element.get_bool().value;
        }

        long longField(const bsoncxx::document::view &document, const char *name) {
            const auto element = document[name];
            if (!element) return 0;
            switch (element.type()) {
                case bsoncxx::type::k_int32:
                    return element.get_int32().value;
                case bsoncxx::type::k_int64:
                    return static_cast<long>(element.get_int64().value);
                default:
                    return 0;
            }
        }

        // A reply carrying a BSON document, which is what every operation answers with.
        response<string_body> bsonResponse(const request<string_body> &req, const bsoncxx::document::view &document) {
            response<string_body> res{status::ok, req.version()};
            res.set(field::content_type, "application/octet-stream");
            res.keep_alive(req.keep_alive());
            res.body() = std::string(reinterpret_cast<const char *>(document.data()), document.length());
            res.prepare_payload();
            return res;
        }

        response<string_body> foundResponse(const request<string_body> &req, const std::optional<bsoncxx::document::value> &found) {
            if (!found.has_value()) return bsonResponse(req, make_document().view());
            return bsonResponse(req, make_document(kvp(P::kFound, found->view())).view());
        }

        response<string_body> updateResponse(const request<string_body> &req, const Database::Emd::UpdateResult &result) {

            bsoncxx::builder::basic::document reply;
            reply.append(kvp(P::kMatched, static_cast<std::int64_t>(result.matched)),
                         kvp(P::kModified, static_cast<std::int64_t>(result.modified)));
            if (result.upsertedId.has_value()) reply.append(kvp(P::kUpsertedId, *result.upsertedId));
            return bsonResponse(req, reply.view());
        }

    }// namespace

    EmdServer::EmdServer(std::string socketPath, const int threads) : HttpActionServer("EMD", std::move(socketPath), threads) {}

    response<string_body> EmdServer::Dispatch(const request<string_body> &req) {

        const auto action = std::string(req["x-euclid-action"]);
        const auto request = requestOf(req);
        const auto collection = stringField(request, P::kCollection);

        log_trace << "EMD action=" << action << ", collection=" << collection;

        try {

            if (action == P::kFindOne) {
                return foundResponse(req, _store.FindOne(collection, documentField(request, P::kFilter)));
            }

            if (action == P::kFind) {

                FindOptions options;
                if (const auto sort = documentField(request, P::kSort); sort.begin() != sort.end()) {
                    options.sort = bsoncxx::document::value(sort);
                }
                options.limit = longField(request, P::kLimit);
                options.skip = longField(request, P::kSkip);

                bsoncxx::builder::basic::array documents;
                for (const auto &document: _store.Find(collection, documentField(request, P::kFilter), options)) {
                    documents.append(document.view());
                }
                return bsonResponse(req, make_document(kvp(P::kDocuments, documents)).view());
            }

            if (action == P::kInsertOne) {
                const auto id = _store.InsertOne(collection, documentField(request, P::kDocument));
                return bsonResponse(req, make_document(kvp(P::kOid, id)).view());
            }

            if (action == P::kUpdateOne) {
                return updateResponse(req, _store.UpdateOne(collection, documentField(request, P::kFilter),
                                                            documentField(request, P::kUpdate), boolField(request, P::kUpsert)));
            }

            if (action == P::kUpdateMany) {
                return updateResponse(req, _store.UpdateMany(collection, documentField(request, P::kFilter),
                                                             documentField(request, P::kUpdate)));
            }

            if (action == P::kReplaceOne) {
                return updateResponse(req, _store.ReplaceOne(collection, documentField(request, P::kFilter),
                                                             documentField(request, P::kDocument), boolField(request, P::kUpsert)));
            }

            if (action == P::kFindOneAndUpdate) {
                return foundResponse(req, _store.FindOneAndUpdate(collection, documentField(request, P::kFilter),
                                                                  documentField(request, P::kUpdate),
                                                                  boolField(request, P::kUpsert), boolField(request, P::kReturnAfter)));
            }

            if (action == P::kFindOneAndDelete) {
                return foundResponse(req, _store.FindOneAndDelete(collection, documentField(request, P::kFilter)));
            }

            if (action == P::kDelete) {
                const auto deleted = _store.Delete(collection, documentField(request, P::kFilter), boolField(request, P::kSingle));
                return bsonResponse(req, make_document(kvp(P::kCount_, static_cast<std::int64_t>(deleted))).view());
            }

            if (action == P::kCount) {
                const auto counted = _store.CountDocuments(collection, documentField(request, P::kFilter));
                return bsonResponse(req, make_document(kvp(P::kCount_, static_cast<std::int64_t>(counted))).view());
            }

            if (action == P::kGroupCount) {

                std::vector<std::string> groupFields;
                if (const auto element = request[P::kGroupFields]; element && element.type() == bsoncxx::type::k_array) {
                    for (const auto &field: element.get_array().value) groupFields.emplace_back(field.get_string().value);
                }

                bsoncxx::builder::basic::array groups;
                for (const auto &group: _store.GroupCount(collection, documentField(request, P::kFilter), groupFields,
                                                          stringField(request, P::kSumField))) {

                    bsoncxx::builder::basic::array key;
                    for (const auto &value: group.key) key.append(value);

                    groups.append(make_document(kvp(P::kKey, key),
                                                kvp(P::kCount_, static_cast<std::int64_t>(group.count)),
                                                kvp(P::kSum, static_cast<std::int64_t>(group.sum))));
                }
                return bsonResponse(req, make_document(kvp(P::kGroups, groups)).view());
            }

            if (action == P::kCreateIndex) {
                _store.CreateIndex(collection, documentField(request, P::kKeys), boolField(request, P::kUnique));
                return bsonResponse(req, make_document().view());
            }

            if (action == "get-metrics") return MetricsResponse(req);

            return bsonResponse(req, make_document(kvp(P::kError, "EMD does not implement the action " + action)).view());

        } catch (const std::exception &e) {

            // Reported in the reply rather than as an HTTP failure, so the client can raise the
            // same exception the in-process store would have raised - a duplicate key, an operator
            // this store does not implement. A caller must not be able to tell from the failure
            // whether the store was in its own process or another one.
            log_debug << "EMD " << action << " failed, collection: " << collection << ", error: " << e.what();
            return bsonResponse(req, make_document(kvp(P::kError, std::string(e.what()))).view());
        }
    }

}// namespace Euclid::EMD
