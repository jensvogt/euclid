// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/8/26.
//

// C++ includes
#include <stdexcept>
#include <thread>

// Boost includes
#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

// MongoDB includes
#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>

// Euclid includes
#include <euclid/core/Configuration.h>
#include <euclid/core/LogStream.h>
#include <euclid/database/emd/Protocol.h>
#include <euclid/database/emd/RemoteDocumentStore.h>

namespace Euclid::Database::Emd {

    namespace asio = boost::asio;
    namespace beast = boost::beast;
    namespace http = boost::beast::http;
    namespace P = Protocol;

    using bsoncxx::builder::basic::kvp;
    using bsoncxx::builder::basic::make_document;

    namespace {

        // The documents of a reply array, copied out - the reply itself does not outlive the call.
        std::vector<bsoncxx::document::value> documentsOf(const bsoncxx::document::view &reply, const char *field) {
            std::vector<bsoncxx::document::value> documents;
            if (const auto element = reply[field]; element && element.type() == bsoncxx::type::k_array) {
                for (const auto &entry: element.get_array().value) {
                    if (entry.type() == bsoncxx::type::k_document) documents.emplace_back(entry.get_document().value);
                }
            }
            return documents;
        }

        long longOf(const bsoncxx::document::view &reply, const char *field) {
            const auto element = reply[field];
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

    }// namespace

    RemoteDocumentStore::RemoteDocumentStore(std::string socketPath) : _socketPath(std::move(socketPath)) {
        _connectTimeout = std::chrono::milliseconds(
                Core::Configuration::instance().getOr<long>("euclid.modules.emd.connect-timeout-ms", 1000));
    }

    bsoncxx::document::value RemoteDocumentStore::Call(const std::string &action, const bsoncxx::document::view request) const {

        asio::io_context ioc;
        asio::local::stream_protocol::socket socket(ioc);

        // Retried rather than failed at once: a module can be answering requests before the store
        // is listening - the manager starts them in dependency order, but a module that queries on
        // its own schedule does not wait for that.
        const auto started = std::chrono::steady_clock::now();
        const auto deadline = started + _connectTimeout;
        bool waited = false;

        for (;;) {
            boost::system::error_code ec;
            socket.connect(asio::local::stream_protocol::endpoint(_socketPath), ec);
            if (!ec) break;

            if (std::chrono::steady_clock::now() >= deadline) {
                throw std::runtime_error("Could not reach the memory database at " + _socketPath + ": " + ec.message());
            }

            // Said once, after the first second, so a process that is waiting for the store says
            // so rather than appearing to hang.
            if (!waited) {
                waited = true;
                log_debug << "Waiting for the memory database at " << _socketPath;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        http::request<http::string_body> httpRequest{http::verb::post, "/", 11};
        httpRequest.set(http::field::host, "emd");
        httpRequest.set("x-euclid-action", action);
        httpRequest.set(http::field::content_type, "application/octet-stream");
        httpRequest.body() = std::string(reinterpret_cast<const char *>(request.data()), request.length());
        httpRequest.prepare_payload();

        http::write(socket, httpRequest);

        beast::flat_buffer buffer;
        http::response<http::string_body> httpResponse;
        http::read(socket, buffer, httpResponse);

        boost::system::error_code ignored;
        socket.shutdown(asio::local::stream_protocol::socket::shutdown_both, ignored);

        if (httpResponse.body().size() < 5) {
            throw std::runtime_error("The memory database answered " + std::to_string(httpResponse.result_int()) + " with no document");
        }

        auto reply = bsoncxx::document::value(
                bsoncxx::document::view(reinterpret_cast<const std::uint8_t *>(httpResponse.body().data()), httpResponse.body().size()));

        // An error the store raised - an operator it does not implement, a duplicate key - is
        // raised again here, so that a caller sees the same failure whether the store is in this
        // process or another one.
        if (const auto error = reply.view()[P::kError]; error && error.type() == bsoncxx::type::k_string) {
            throw std::runtime_error(std::string(error.get_string().value));
        }
        return reply;
    }

    std::optional<bsoncxx::document::value> RemoteDocumentStore::FindOne(const std::string &collection, const bsoncxx::document::view filter) const {

        const auto reply = Call(P::kFindOne, make_document(kvp(P::kCollection, collection), kvp(P::kFilter, filter)).view());
        const auto found = reply.view()[P::kFound];
        if (!found || found.type() != bsoncxx::type::k_document) return std::nullopt;
        return bsoncxx::document::value(found.get_document().value);
    }

    std::vector<bsoncxx::document::value> RemoteDocumentStore::Find(const std::string &collection, const bsoncxx::document::view filter,
                                                                    const FindOptions &options) const {

        bsoncxx::builder::basic::document request;
        request.append(kvp(P::kCollection, collection), kvp(P::kFilter, filter));
        if (options.sort.has_value()) request.append(kvp(P::kSort, options.sort->view()));
        if (options.limit > 0) request.append(kvp(P::kLimit, static_cast<std::int64_t>(options.limit)));
        if (options.skip > 0) request.append(kvp(P::kSkip, static_cast<std::int64_t>(options.skip)));

        const auto reply = Call(P::kFind, request.view());
        return documentsOf(reply.view(), P::kDocuments);
    }

    bsoncxx::oid RemoteDocumentStore::InsertOne(const std::string &collection, const bsoncxx::document::view document) {

        const auto reply = Call(P::kInsertOne, make_document(kvp(P::kCollection, collection), kvp(P::kDocument, document)).view());
        const auto oid = reply.view()[P::kOid];
        return oid && oid.type() == bsoncxx::type::k_oid ? oid.get_oid().value : bsoncxx::oid{};
    }

    UpdateResult RemoteDocumentStore::UpdateOne(const std::string &collection, const bsoncxx::document::view filter,
                                                const bsoncxx::document::view update, const bool upsert) {

        const auto reply = Call(P::kUpdateOne, make_document(kvp(P::kCollection, collection), kvp(P::kFilter, filter),
                                                             kvp(P::kUpdate, update), kvp(P::kUpsert, upsert))
                                                       .view());
        UpdateResult result{.matched = longOf(reply.view(), P::kMatched), .modified = longOf(reply.view(), P::kModified)};
        if (const auto id = reply.view()[P::kUpsertedId]; id && id.type() == bsoncxx::type::k_oid) result.upsertedId = id.get_oid().value;
        return result;
    }

    UpdateResult RemoteDocumentStore::UpdateMany(const std::string &collection, const bsoncxx::document::view filter,
                                                 const bsoncxx::document::view update) {

        const auto reply = Call(P::kUpdateMany, make_document(kvp(P::kCollection, collection), kvp(P::kFilter, filter),
                                                              kvp(P::kUpdate, update))
                                                        .view());
        return {.matched = longOf(reply.view(), P::kMatched), .modified = longOf(reply.view(), P::kModified)};
    }

    UpdateResult RemoteDocumentStore::ReplaceOne(const std::string &collection, const bsoncxx::document::view filter,
                                                 const bsoncxx::document::view replacement, const bool upsert) {

        const auto reply = Call(P::kReplaceOne, make_document(kvp(P::kCollection, collection), kvp(P::kFilter, filter),
                                                              kvp(P::kDocument, replacement), kvp(P::kUpsert, upsert))
                                                        .view());
        UpdateResult result{.matched = longOf(reply.view(), P::kMatched), .modified = longOf(reply.view(), P::kModified)};
        if (const auto id = reply.view()[P::kUpsertedId]; id && id.type() == bsoncxx::type::k_oid) result.upsertedId = id.get_oid().value;
        return result;
    }

    std::optional<bsoncxx::document::value> RemoteDocumentStore::FindOneAndUpdate(const std::string &collection, const bsoncxx::document::view filter,
                                                                                  const bsoncxx::document::view update, const bool upsert,
                                                                                  const bool returnAfter) {

        const auto reply = Call(P::kFindOneAndUpdate, make_document(kvp(P::kCollection, collection), kvp(P::kFilter, filter),
                                                                    kvp(P::kUpdate, update), kvp(P::kUpsert, upsert),
                                                                    kvp(P::kReturnAfter, returnAfter))
                                                              .view());
        const auto found = reply.view()[P::kFound];
        if (!found || found.type() != bsoncxx::type::k_document) return std::nullopt;
        return bsoncxx::document::value(found.get_document().value);
    }

    std::optional<bsoncxx::document::value> RemoteDocumentStore::FindOneAndDelete(const std::string &collection, const bsoncxx::document::view filter) {

        const auto reply = Call(P::kFindOneAndDelete, make_document(kvp(P::kCollection, collection), kvp(P::kFilter, filter)).view());
        const auto found = reply.view()[P::kFound];
        if (!found || found.type() != bsoncxx::type::k_document) return std::nullopt;
        return bsoncxx::document::value(found.get_document().value);
    }

    long RemoteDocumentStore::Delete(const std::string &collection, const bsoncxx::document::view filter, const bool single) {

        const auto reply = Call(P::kDelete, make_document(kvp(P::kCollection, collection), kvp(P::kFilter, filter), kvp(P::kSingle, single)).view());
        return longOf(reply.view(), P::kCount_);
    }

    long RemoteDocumentStore::CountDocuments(const std::string &collection, const bsoncxx::document::view filter) const {

        const auto reply = Call(P::kCount, make_document(kvp(P::kCollection, collection), kvp(P::kFilter, filter)).view());
        return longOf(reply.view(), P::kCount_);
    }

    std::vector<GroupCounts> RemoteDocumentStore::GroupCount(const std::string &collection, const bsoncxx::document::view filter,
                                                             const std::vector<std::string> &groupFields, const std::string &sumField) const {

        bsoncxx::builder::basic::array fields;
        for (const auto &field: groupFields) fields.append(field);

        const auto reply = Call(P::kGroupCount, make_document(kvp(P::kCollection, collection), kvp(P::kFilter, filter),
                                                              kvp(P::kGroupFields, fields), kvp(P::kSumField, sumField))
                                                        .view());

        std::vector<GroupCounts> groups;
        if (const auto element = reply.view()[P::kGroups]; element && element.type() == bsoncxx::type::k_array) {
            for (const auto &entry: element.get_array().value) {
                const auto group = entry.get_document().value;

                GroupCounts counts;
                if (const auto key = group[P::kKey]; key && key.type() == bsoncxx::type::k_array) {
                    for (const auto &value: key.get_array().value) counts.key.emplace_back(value.get_string().value);
                }
                counts.count = longOf(group, P::kCount_);
                counts.sum = longOf(group, P::kSum);
                groups.push_back(std::move(counts));
            }
        }
        return groups;
    }

    void RemoteDocumentStore::CreateIndex(const std::string &collection, const bsoncxx::document::view keys, const bool unique) {
        std::ignore = Call(P::kCreateIndex, make_document(kvp(P::kCollection, collection), kvp(P::kKeys, keys), kvp(P::kUnique, unique)).view());
    }

    void RemoteDocumentStore::Clear() {
        // Deliberately not offered over the socket: one module emptying the store every other
        // module is using is not something a euclid process has any business doing. A test that
        // wants a clean store starts a new one.
        throw std::runtime_error("The shared memory database cannot be cleared by a client");
    }

    std::vector<std::string> RemoteDocumentStore::Collections() const {
        // Only the monitoring module asks, and only to report sizes; answering with nothing costs
        // it a metric rather than an error.
        return {};
    }

    long RemoteDocumentStore::Size(const std::string &collection) const {
        return CountDocuments(collection, bsoncxx::document::view{});
    }

}// namespace Euclid::Database::Emd
