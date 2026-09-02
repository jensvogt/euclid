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
         * @brief Name identifying this server, unique across the installation.
         *
         * Doubles as the manager's module name for the spawned process, which is why it has to
         * be unique: two servers sharing a name would share a process pool.
         */
        std::string serverId;

        /**
         * @brief Euclid resource name
         */
        std::string ern;

        /**
         * @brief Account this server belongs to
         */
        std::string accountId;

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

}// namespace Euclid::Database::Entity::ETS
