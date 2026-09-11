// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/8/26.
//

#pragma once

// C++ includes
#include <chrono>
#include <map>
#include <optional>
#include <string>

// MongoDB includes
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/document/value-fwd.hpp>

// Euclid includes
#include <euclid/database/entity/BaseEntity.h>

namespace Euclid::Database::Entity::ESS {

    using std::chrono::system_clock;

    /**
     * @brief One secret: a password, a connection string, a token - something an application needs
     * and nobody should be able to read out of a configuration file.
     *
     * @par
     * The value is never here in the clear. It is stored as ciphertext, encrypted under an EKM key
     * this row names, so a copy of the database - a dump, a backup, a mongo shell - yields nothing
     * without the key management module. That is the whole reason this module exists rather than
     * telling people to put passwords in euclid.json, and it is why there is no path through the
     * secrets module that stores an unencrypted value.
     *
     * @par
     * What is *not* encrypted is everything around it: the name, the description, the tags and the
     * dates. Those are what a caller lists, searches and audits by, and they are chosen by whoever
     * created the secret rather than being the secret itself.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    struct Secret final : BaseEntity {

        /**
         * @brief ID
         */
        std::string oid;

        /**
         * @brief Euclid resource name
         */
        std::string ern;

        /**
         * @brief Name the secret is referred to by, chosen by whoever created it. Unique within an
         * account and namespace, and what an application asks for.
         */
        std::string name;

        /**
         * @brief What the secret is for, in the words of whoever created it. Free text, never
         * interpreted, and readable by anyone who may list secrets - so it must not be used to
         * say anything that belongs in the value.
         */
        std::string description;

        /**
         * @brief The secret itself, encrypted under @ref encryptionKeyErn and base64-encoded.
         *
         * @par
         * Ciphertext, always. The module has no code path that writes anything else here, and
         * nothing but get-secret ever turns it back into a value.
         */
        std::string value;

        /**
         * @brief ERN of the EKM key the value is encrypted under.
         *
         * @par
         * Recorded per secret rather than per installation, so that re-keying one secret does not
         * touch any other, and so a secret can be read back years later under the key it was
         * actually written with - the same arrangement ESM uses for an object.
         */
        std::string encryptionKeyErn;

        /**
         * @brief How many times the value has been set, counting the first. Rotating a secret
         * moves this on, which is what lets somebody see at a glance whether a rotation that was
         * supposed to happen did.
         */
        long version = 1;

        /**
         * @brief When the value was last changed, as opposed to @ref modified, which any change to
         * the row moves - a description, a tag. This is the one an expiry policy would read.
         */
        system_clock::time_point rotated = system_clock::now();

        /**
         * @brief Secret tags
         */
        std::map<std::string, std::string> tags;

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
        static Secret fromDocument(const std::optional<bsoncxx::document::view> &document);
    };

}// namespace Euclid::Database::Entity::ESS
