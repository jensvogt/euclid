// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

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
     * @author jensvogt47\@gmail.com
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
         * @brief Whether this object is a directory marker rather than a file.
         *
         * @par
         * Derivable from the key - a marker is a key ending in "/" - and stored anyway, because
         * the counters ask the question of every object in a bucket at once. Deriving it there
         * means a per-document expression over the whole collection every recount, and it means
         * the key has to be read to answer it, which is what stops that aggregation being served
         * from an index alone.
         *
         * @par
         * Absent on objects written before this field existed, which read as false - a directory
         * marker among them is counted as a file until it is rewritten. The counters are
         * approximate and self-correcting by design, so that resolves itself; a one-off update
         * setting the flag from the key fixes it immediately.
         */
        bool directory{};

        /**
         * @brief The key's parent path: everything up to and including its last "/", or empty for
         * an object at the bucket's root.
         *
         * @par
         * "DEMODLI/sub/a.jpg" has the parent "DEMODLI/sub/"; "a.jpg" has none. A directory marker
         * is a key in its own right, so "DEMODLI/sub/" has the parent "DEMODLI/".
         *
         * @par
         * Derivable from the key, and stored anyway, for the reason @ref directory is: a listing
         * of one level is the question clients actually ask, and without this it can only be
         * answered by a prefix match over every key in the bucket and a walk to find where one
         * folder's block ends. As a stored field it is an equality - served from the index
         * (bucketErn, parent, directory, key), counted and paged from the index alone, and the
         * folders inside a level found by seeking from one to the next through it.
         *
         * @par
         * Set on every write - MongoEsmRepository::upsertObject() derives it from the key - so
         * every object has it. Objects written before the field existed were given it by a one-off
         * backfill at startup; that ran on every ESM start, as a scan of the whole collection, long
         * after there was nothing left for it to do, and was removed once none were left.
         */
        std::string parent;

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
         * @brief Euclid's own attributes, carried across every hop and never mixed into the
         * user's.
         *
         * @par
         * The envelope, as opposed to the contents. Something that decides work is urgent, or that
         * gives it a correlation id, has to be able to say so where the fact survives being written
         * to a bucket and read back out as a queue message - and a bucket has no notion of urgency
         * of its own. Without somewhere to put it, that decision has to be smuggled through the one
         * thing that does travel.
         *
         * @par
         * Kept apart from the user's attributes rather than reserving names inside them: a caller
         * listing or setting attributes sees only its own, so euclid can add a system attribute
         * later without colliding with something somebody already stores, and a user attribute
         * called "priority" means nothing to euclid.
         */
        std::map<std::string, COM::Variant> systemAttributes;

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

    /**
     * @brief The level a key sits in: everything up to and including its last "/".
     *
     * @par
     * A directory marker names the directory itself, so it belongs to the level above it rather
     * than to its own - "a/b/" sits in "a/", the same place "a/b.txt" does. That is what makes a
     * marker show up beside its siblings instead of inside itself.
     *
     * @param key object key.
     * @return the parent path, empty for a key at the bucket's root.
     */
    inline std::string ParentOfKey(const std::string &key) {
        if (key.empty()) return {};
        // The trailing "/" of a marker is its own, not a separator from a parent, so the search
        // for the one above it starts before it.
        const auto from = IsDirectoryKey(key) ? key.size() - 1 : key.size();
        if (from == 0) return {};
        const auto slash = key.rfind('/', from - 1);
        if (slash == std::string::npos) return {};
        const auto parent = key.substr(0, slash + 1);
        // A key that is nothing but separators has no level above it; returning itself would make
        // it its own parent, and a walk upwards would never end.
        return parent == key ? std::string{} : parent;
    }

}// namespace Euclid::Database::Entity::ESM