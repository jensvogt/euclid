//
// Created by vogje01 on 8/23/26.
//

#pragma once

// C++ includes
#include <chrono>
#include <map>
#include <optional>
#include <string>

// MongoDB includes
#include <bsoncxx/document/value-fwd.hpp>
#include <bsoncxx/builder/basic/document.hpp>

// Euclid includes
#include <euclid/database/entity/BaseEntity.h>
#include <euclid/database/entity/com/Variant.h>
#include <euclid/database/entity/esm/ObjectStatus.h>

namespace Euclid::Database::Entity::ESM {

    using std::chrono::system_clock;

    /**
     * @brief Storage object entity.
     *
     * Represents one bucket object. The object's bytes live in a flat file on disk named after
     * internalName (a UUID); key is only ever resolved to that filename through this entity, so
     * the database is the sole source of truth for the key-to-file mapping.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    struct Object final : BaseEntity {

        /**
         * @brief Owner
         */
        std::string owner;

        /**
         * @brief ID
         */
        std::string oid;

        /**
         * @brief ERN of the bucket this object belongs to
         */
        std::string bucketErn;

        /**
         * @brief Object key (path) within the bucket
         */
        std::string key;

        /**
         * @brief Internal name (UUID) of the flat file this object's bytes are stored under
         */
        std::string internalName;

        /**
         * @brief Object ERN
         */
        std::string ern;

        /**
         * @brief ERN of the EKM key this object's bytes are encrypted under, or empty if they are
         * stored in the clear.
         *
         * @par
         * Recorded per object rather than read off the bucket at download time, because the bucket
         * answers a different question: it says what happens to the next object written, while
         * this says what actually happened to this one. They differ for every object a bucket held
         * before encryption was enabled on it, and for every object still under a previous key -
         * all of which stay readable precisely because each one names its own key.
         */
        std::string encryptionKeyErn;

        /**
         * @brief Object size in bytes - the size of what a client stored, so for an encrypted
         * object the plaintext length rather than the size of the file on disk.
         */
        long size = 0;

        /**
         * @brief Lifecycle status - see ObjectStatus.
         */
        ObjectStatus status = ObjectStatus::CREATED;

        /**
         * @brief MIME content type, determined during post-processing (complete-upload); empty
         * until status reaches COMPLETED.
         */
        std::string contentType;

        /**
         * @brief MD5 checksum of the assembled object, hex-encoded, computed during
         * post-processing (complete-upload); empty until status reaches COMPLETED.
         */
        std::string md5Sum;

        /**
         * @brief User-defined attributes travelling with the object.
         *
         * @par
         * Free-form metadata the object's owner attaches to it - the storage module neither
         * interprets these nor derives anything from them, unlike contentType or md5Sum, which it
         * computes itself. Values keep their C++ type across a MongoDB round-trip (see
         * COM::Variant), so a number stored as a number reads back as one.
         */
        std::map<std::string, COM::Variant> attributes;

        /**
         * @brief Creation date
         */
        system_clock::time_point created = system_clock::now();

        /**
         * @brief Last modification date
         */
        system_clock::time_point modified = system_clock::now();

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
        static Object fromDocument(const std::optional<bsoncxx::document::view> &document);
    };

    /**
     * @brief Whether an object key names a directory rather than a file.
     *
     * @par
     * A bucket is flat, so a directory exists only as a zero-byte object whose key ends in "/" -
     * the marker an FTP/SFTP transfer server writes on MKD, the same convention S3-style clients
     * use. Listings and the bucket's object counters leave these out, so a bucket reports the
     * files it holds and nothing else; whoever manages directories (i.e. a transfer server) asks
     * for them explicitly.
     *
     * @param key object key
     * @return true if the key names a directory.
     */
    inline bool IsDirectoryKey(const std::string &key) {
        return !key.empty() && key.back() == '/';
    }

}// namespace Euclid::Database::Entity::ESM
