//
// Created by vogje01 on 9/7/26.
//

#pragma once

// C++ includes
#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <vector>

// MongoDB includes
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/document/value-fwd.hpp>

// Euclid includes
#include <euclid/database/entity/BaseEntity.h>

namespace Euclid::Database::Entity::EKM {

    using std::chrono::system_clock;

    /**
     * @brief An X.509 certificate and its private key, kept by the key management module.
     *
     * @par
     * Here rather than as a pair of files next to each listener that uses one, because a
     * certificate is key material and this is where euclid keeps key material: one place that
     * knows what exists, who it belongs to and when it expires, reachable from every module and
     * from the CLI. A file path only answers the last of those, and only for whoever can already
     * read the file.
     *
     * @par
     * The private key is stored with the certificate and never leaves the server - it is not part
     * of any DTO, the same way Key::keyMaterial is not.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    struct Certificate final : BaseEntity {

        /**
         * @brief ID
         */
        std::string oid;

        /**
         * @brief Euclid resource name
         */
        std::string ern;

        /**
         * @brief Name the certificate is referred to by, chosen by whoever created it - a listener
         * names one of these. Unique within an account and namespace.
         */
        std::string name;

        /**
         * @brief What the certificate is for, in the words of whoever imported it. Free text,
         * never interpreted.
         */
        std::string description;

        /**
         * @brief PEM-encoded certificate, leaf first. May hold the intermediates a certificate
         * authority issued it with, which is what a client needs to build a path to its own
         * trust anchor.
         */
        std::string certificatePem;

        /**
         * @brief PEM-encoded private key. Never leaves the server - not part of any DTO.
         */
        std::string privateKeyPem;

        /**
         * @brief Distinguished name of the subject, read out of the certificate when it was
         * stored.
         */
        std::string subject;

        /**
         * @brief Distinguished name of the issuer. Equal to the subject when self-signed.
         */
        std::string issuer;

        /**
         * @brief Serial number, hexadecimal.
         */
        std::string serialNumber;

        /**
         * @brief SHA-256 fingerprint, lower-case hexadecimal. What somebody compares against when
         * asked whether to trust a self-signed certificate.
         */
        std::string fingerprint;

        /**
         * @brief The names the certificate is valid for, as they appear in it, e.g. "DNS:localhost"
         * or "IP:127.0.0.1".
         */
        std::vector<std::string> subjectAltNames;

        /**
         * @brief Whether euclid generated this certificate itself, rather than being given one.
         *
         * @par
         * Kept as its own field rather than derived from subject == issuer: a certificate somebody
         * imported that happens to be self-signed is still a decision they made, and one euclid
         * minted because a listener needed something to start with is not. Only the second may be
         * replaced without asking.
         */
        bool generated{false};

        /**
         * @brief Start of the validity period.
         */
        system_clock::time_point notBefore{};

        /**
         * @brief End of the validity period. What answers "is this port about to stop working".
         */
        system_clock::time_point notAfter{};

        /**
         * @brief Certificate tags
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
        static Certificate fromDocument(const std::optional<bsoncxx::document::view> &document);
    };

}// namespace Euclid::Database::Entity::EKM
