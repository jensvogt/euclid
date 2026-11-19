// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/7/26.
//

#pragma once

// C++ includes
#include <string>
#include <vector>

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::EKM {

    using Json::tag_invoke;

    using std::chrono::system_clock;

    /**
     * @brief A stored certificate as it is reported to a caller.
     *
     * @par
     * Everything but the private key, which never leaves the server. The certificate itself is
     * included: it is public by construction - every client that opens a connection is handed a
     * copy - and it is what somebody needs in order to add it to a trust store.
     */
    struct Certificate {

        /**
         * @brief Certificate name, as chosen when it was imported or created
         */
        std::string name;

        /**
         * @brief Euclid resource name
         */
        std::string ern;

        /**
         * @brief What the certificate is for; empty if nothing was said
         */
        std::string description;

        /**
         * @brief PEM-encoded certificate, leaf first, including any intermediates it was stored
         * with
         */
        std::string certificate;

        /**
         * @brief Distinguished name of the subject
         */
        std::string subject;

        /**
         * @brief Distinguished name of the issuer; the same as the subject when self-signed
         */
        std::string issuer;

        /**
         * @brief Serial number, hexadecimal
         */
        std::string serialNumber;

        /**
         * @brief SHA-256 fingerprint, lower-case hexadecimal
         */
        std::string fingerprint;

        /**
         * @brief The names the certificate is valid for, e.g. "DNS:localhost", "IP:127.0.0.1"
         */
        std::vector<std::string> subjectAltNames;

        /**
         * @brief Whether euclid generated this certificate itself because something needed one
         */
        bool generated{false};

        /**
         * @brief Start of the validity period
         */
        system_clock::time_point notBefore{};

        /**
         * @brief End of the validity period
         */
        system_clock::time_point notAfter{};

        /**
         * @brief Certificate tags
         */
        std::map<std::string, std::string> tags;

        /**
         * @brief Creation date
         */
        system_clock::time_point created{};

        /**
         * @brief Last modification date
         */
        system_clock::time_point modified{};

        /**
         * @brief Serializes this object to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this object from a JSON string
         */
        [[nodiscard]]
        static Certificate fromJson(const std::string &json) {
            return boost::json::value_to<Certificate>(Core::ParseJsonString(json));
        }
    };

    BOOST_DESCRIBE_STRUCT(Certificate, (), (name, ern, description, certificate, subject, issuer, serialNumber, fingerprint,
                                           subjectAltNames, generated, notBefore, notAfter, tags, created, modified))

}// namespace Euclid::Dto::EKM
