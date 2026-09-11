// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 8/31/26.
//

#pragma once

// C++ includes
#include <chrono>
#include <optional>
#include <string>
#include <vector>

// MongoDB includes
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/document/value.hpp>
#include <bsoncxx/document/view-fwd.hpp>

// Euclid includes
#include <euclid/database/entity/RuntimeName.h>
#include <euclid/database/entity/ets/TransferProtocol.h>
#include <euclid/database/entity/ets/TransferServerState.h>

namespace Euclid::Database::Entity::ETS {

    /**
     * @brief One configured transfer server: an FTP or SFTP endpoint fronting an ESM bucket.
     *
     * @par
     * The definition is the whole contract between the three modules it ties together. ETS owns
     * it and is the only thing that writes it; euclid-mgr reads it to decide which processes to
     * run; and the euclid-ftp/euclid-sftp process it spawns reads its own definition back to
     * learn which port to listen on, which EAM users and groups may log in, and which bucket
     * its clients are really talking to.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    struct TransferServer {

        /**
         * @brief ID
         */
        std::string oid;

        /**
         * @brief Name identifying this server within its account and namespace.
         *
         * What a person creates, names and asks for. Not what the server runs as - see
         * @ref runtimeName.
         */
        std::string serverId;

        /**
         * @brief The name the manager runs this server under, issued once and never changed.
         *
         * @par
         * Everything outside the definition is keyed by this: the process pool, the module row the
         * manager registers, the unix socket, the log channel, and the --transfer-server argument
         * the spawned process reads its own definition back by. None of those has an account or a
         * namespace to live in, so none of them can be keyed by a serverId, which is unique only
         * within one.
         *
         * @par
         * Stored rather than derived so that it holds still: what a server is called on a host
         * should not change because its definition was edited. See Entity::GenerateRuntimeName().
         *
         * @par
         * Empty on servers created before this field existed. They ran under their bare serverId,
         * and RuntimeName() goes on returning exactly that for them, so nothing about them moves.
         * Written only when set - see toDocument() - so the unique index on this field skips them
         * rather than seeing every one of them as the same empty name.
         */
        std::string runtimeName;

        /**
         * @brief Euclid resource name
         */
        std::string ern;

        /**
         * @brief Account this server belongs to
         */
        std::string accountId;

        /**
         * @brief Namespace this server belongs to, and whose bucket it serves.
         *
         * @par
         * Part of the server's identity: a serverId is unique within (accountId, nameSpace), and
         * the ERN carries all three. Empty for a server at the account root.
         */
        std::string nameSpace;

        /**
         * @brief Region this server belongs to
         */
        std::string region;

        /**
         * @brief Protocol clients speak to this server.
         */
        TransferProtocol protocol{TransferProtocol::SFTP};

        /**
         * @brief Address the server binds to.
         */
        std::string address{"0.0.0.0"};

        /**
         * @brief Port the server listens on. Must not collide with another server's port.
         */
        long port{};

        /**
         * @brief ERN of the ESM bucket this server's clients read and write.
         *
         * The bucket is the system of record: an upload is only complete once it is an object
         * in here, and a listing shows what the bucket holds rather than what is on local disk.
         */
        std::string bucketErn;

        /**
         * @brief Name of that bucket, kept alongside the ERN so listings stay readable without
         * a second lookup.
         */
        std::string bucketName;

        /**
         * @brief Key prefix template each client's session is rooted at, or empty for the bucket
         * root.
         *
         * @par
         * Expanded per session by Transfer::HomePrefix, with {user} standing for the EAM user ID
         * that logged in: "{user}" gives every client its own corner of the bucket, the way an
         * FTP server's per-user home directory always has, while empty - the default, and what
         * every server defined before this existed has - leaves all of them sharing one flat key
         * space at the root.
         *
         * @par
         * A template rather than a per-user mapping because a server admits users by group as
         * well as by name: a map could not name a user who has not logged in yet.
         */
        std::string homeDirectory;

        /**
         * @brief EAM user IDs allowed to log in, in addition to any member of userGroups.
         */
        std::vector<std::string> userIds;

        /**
         * @brief EAM user groups whose members are allowed to log in.
         *
         * Union with userIds rather than intersection: a user may be listed directly, belong to
         * a permitted group, or both.
         */
        std::vector<std::string> userGroups;

        /**
         * @brief Directories every session should find under its home, created at login.
         *
         * @par
         * A transfer workflow expects a shape - an inbox to deliver into, somewhere feedback comes
         * back - and a client cannot be asked to create it: it delivers into a folder that has to
         * be there already. Held here rather than in euclid.json because it belongs with
         * homeDirectory, which decides where these end up, and because an inbound supplier server
         * and an outbound distribution server want different shapes on the same host.
         *
         * @par
         * Relative to each session's home prefix, so "incoming/mix" under a "{user}" home is
         * "jvo/incoming/mix/" for one client and "oju/incoming/mix/" for another. Intermediate
         * levels are created too. Empty creates nothing, which is what every server did before.
         */
        std::vector<std::string> directories;

        /**
         * @brief Whether this server should be running. Reconciled by euclid-mgr, never set by
         * the manager itself - see TransferServerState.
         */
        TransferServerState desiredState{TransferServerState::STOPPED};

        /**
         * @brief Private SSH host key file, SFTP only. Generated on first start when absent.
         */
        std::string hostKey;

        /**
         * @brief Lowest passive data port, FTP only.
         */
        long pasvMin{};

        /**
         * @brief Highest passive data port, FTP only.
         */
        long pasvMax{};

        /**
         * @brief Creation timestamp
         */
        std::chrono::system_clock::time_point created;

        /**
         * @brief Last modification timestamp
         */
        std::chrono::system_clock::time_point modified;

        /**
         * @brief Converts the entity to a MongoDB document
         *
         * @return entity as a MongoDB document.
         */
        [[nodiscard]]
        bsoncxx::document::value toDocument() const;

        /**
         * @brief Converts the MongoDB document to an entity
         *
         * @param document MongoDB document.
         */
        static TransferServer fromDocument(const std::optional<bsoncxx::document::view> &document);
    };

    /**
     * @brief The name a transfer server runs under, as opposed to the name it is defined under.
     *
     * @param server the server
     * @return its runtimeName, or its bare serverId for one created before that field existed
     */
    std::string RuntimeName(const TransferServer &server);

}// namespace Euclid::Database::Entity::ETS
