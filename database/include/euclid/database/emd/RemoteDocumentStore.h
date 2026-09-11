// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/8/26.
//

#pragma once

// C++ includes
#include <chrono>
#include <string>

// Euclid includes
#include <euclid/database/emd/IDocumentStore.h>

namespace Euclid::Database::Emd {

    /**
     * @brief A document store held by the EMD process, reached over its socket.
     *
     * @par
     * The other half of the point of EMD: the store lives in one process and every module talks to
     * the same one, so a login issued by EAM is visible to the module the caller goes on to use.
     * That is what the per-process in-memory backend could never do, and why it could not stand in
     * for a database in anything but a single-module test.
     *
     * @par
     * A connection per call, rather than one held open. A module answers requests on a pool of
     * threads, and a shared socket would need a lock across every database operation in the
     * process - which is a worse bottleneck than opening a Unix socket, and a good deal easier to
     * get wrong. Connecting is retried for a short while, because a module may be up before the
     * store is: the manager starts them in dependency order, but nothing guarantees the first
     * query waits for it.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class RemoteDocumentStore final : public IDocumentStore {

    public:

        /**
         * @brief Talks to the store at this socket.
         *
         * @param socketPath Unix domain socket the EMD process listens on.
         */
        explicit RemoteDocumentStore(std::string socketPath);

        [[nodiscard]]
        std::optional<bsoncxx::document::value> FindOne(const std::string &collection, bsoncxx::document::view filter) const override;

        [[nodiscard]]
        std::vector<bsoncxx::document::value> Find(const std::string &collection, bsoncxx::document::view filter,
                                                   const FindOptions &options = {}) const override;

        bsoncxx::oid InsertOne(const std::string &collection, bsoncxx::document::view document) override;

        UpdateResult UpdateOne(const std::string &collection, bsoncxx::document::view filter,
                               bsoncxx::document::view update, bool upsert = false) override;

        UpdateResult UpdateMany(const std::string &collection, bsoncxx::document::view filter,
                                bsoncxx::document::view update) override;

        UpdateResult ReplaceOne(const std::string &collection, bsoncxx::document::view filter,
                                bsoncxx::document::view replacement, bool upsert = false) override;

        std::optional<bsoncxx::document::value> FindOneAndUpdate(const std::string &collection, bsoncxx::document::view filter,
                                                                 bsoncxx::document::view update, bool upsert, bool returnAfter) override;

        std::optional<bsoncxx::document::value> FindOneAndDelete(const std::string &collection, bsoncxx::document::view filter) override;

        long Delete(const std::string &collection, bsoncxx::document::view filter, bool single = false) override;

        [[nodiscard]]
        long CountDocuments(const std::string &collection, bsoncxx::document::view filter) const override;

        [[nodiscard]]
        std::vector<GroupCounts> GroupCount(const std::string &collection, bsoncxx::document::view filter,
                                            const std::vector<std::string> &groupFields, const std::string &sumField = {}) const override;

        void CreateIndex(const std::string &collection, bsoncxx::document::view keys, bool unique) override;

        void Clear() override;

        [[nodiscard]]
        std::vector<std::string> Collections() const override;

        [[nodiscard]]
        long Size(const std::string &collection) const override;

    private:

        /**
         * @brief Sends one request and returns the reply document.
         *
         * @param action operation name - see Protocol.
         * @param request arguments, as BSON.
         * @return the reply.
         * @throws std::runtime_error if the store could not be reached, or reported an error - the
         * same failure the in-process store raises, so a caller cannot tell where it happened.
         */
        [[nodiscard]]
        bsoncxx::document::value Call(const std::string &action, bsoncxx::document::view request) const;

        /**
         * @brief Socket the store listens on.
         */
        std::string _socketPath;

        /**
         * @brief How long to keep retrying a connection before giving up on a call.
         *
         * @par
         * Short on purpose. The store is started before anything that uses it - the manager starts
         * it first, and every module declares it depends on it - so a connection that does not
         * succeed at once means the store is not there, and waiting changes nothing. What the
         * retry is for is the seam either side of a restart, where a module's next query may
         * arrive a moment before the store is listening again.
         *
         * @par
         * The one caller that regularly finds it absent is the manager, during the startup that
         * will eventually launch it: it does a little database work first, and every second spent
         * waiting there is a second added to the start of an installation whose whole appeal is
         * coming up quickly. Raise euclid.modules.emd.connect-timeout-ms if a slower machine needs
         * it.
         */
        std::chrono::milliseconds _connectTimeout{std::chrono::seconds(1)};
    };

}// namespace Euclid::Database::Emd
