// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/7/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::EKM {

    /**
     * @brief Stores a certificate somebody else issued, together with its private key.
     */
    struct ImportCertificateRequest {

        /**
         * @brief Name the certificate is stored under. A listener names one of these, so it is
         * chosen rather than generated - and importing again under the same name replaces it,
         * which is how a renewed certificate is rolled out.
         */
        std::string name;

        /**
         * @brief What the certificate is for. Optional, free text.
         */
        std::string description;

        /**
         * @brief PEM-encoded certificate, leaf first. Intermediates may follow it, and are stored
         * and served with it.
         */
        std::string certificate;

        /**
         * @brief PEM-encoded, unencrypted private key belonging to that certificate.
         */
        std::string privateKey;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this request from a JSON string
         */
        [[nodiscard]] static ImportCertificateRequest fromJson(const std::string &json) {
            return boost::json::value_to<ImportCertificateRequest>(Core::ParseJsonString(json));
        }

    private:

        friend ImportCertificateRequest tag_invoke(boost::json::value_to_tag<ImportCertificateRequest>, boost::json::value const &v) {
            ImportCertificateRequest r;
            r.name = Core::GetStringValue(v, "name");
            r.description = Core::GetStringValue(v, "description");
            r.certificate = Core::GetStringValue(v, "certificate");
            r.privateKey = Core::GetStringValue(v, "privateKey");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, ImportCertificateRequest const &obj) {
            jv = {
                    {"name", obj.name},
                    {"description", obj.description},
                    {"certificate", obj.certificate},
                    {"privateKey", obj.privateKey},
            };
        }
    };

}// namespace Euclid::Dto::EKM
