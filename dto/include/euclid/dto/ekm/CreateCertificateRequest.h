//
// Created by vogje01 on 9/7/26.
//

#pragma once

// C++ includes
#include <string>
#include <vector>

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::EKM {

    /**
     * @brief Generates and stores a self-signed certificate.
     *
     * @par
     * For an installation that has to serve HTTPS before anybody has bought it a certificate -
     * development, a demonstration, an internal network where the clients are told what to trust.
     * Nobody has vouched for the result, and the response says so.
     */
    struct CreateCertificateRequest {

        /**
         * @brief Name the certificate is stored under, and the one a listener names.
         */
        std::string name;

        /**
         * @brief What the certificate is for. Optional, free text.
         */
        std::string description;

        /**
         * @brief Subject common name - normally the host name callers use. Defaults to the
         * certificate's own name if not given.
         */
        std::string commonName;

        /**
         * @brief Further host names or IP addresses the certificate should be valid for. The
         * common name is always included, so this is for the alternatives: "localhost" alongside
         * "127.0.0.1", an internal name alongside the public one.
         */
        std::vector<std::string> subjectAltNames;

        /**
         * @brief How many days the certificate is valid for.
         */
        long validDays = 825;

        /**
         * @brief RSA key length in bits.
         */
        long keyBits = 2048;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this request from a JSON string
         */
        [[nodiscard]] static CreateCertificateRequest fromJson(const std::string &json) {
            return boost::json::value_to<CreateCertificateRequest>(Core::ParseJsonString(json));
        }

    private:

        friend CreateCertificateRequest tag_invoke(boost::json::value_to_tag<CreateCertificateRequest>, boost::json::value const &v) {
            CreateCertificateRequest r;
            r.name = Core::GetStringValue(v, "name");
            r.description = Core::GetStringValue(v, "description");
            r.commonName = Core::GetStringValue(v, "commonName");
            r.subjectAltNames = Core::GetStringArrayValue(v, "subjectAltNames");

            // Defaulted here rather than left at zero, so a request that names neither still
            // produces a usable certificate instead of one that expired the moment it was signed.
            r.validDays = Core::GetLongValue(v, "validDays");
            if (r.validDays <= 0) r.validDays = 825;
            r.keyBits = Core::GetLongValue(v, "keyBits");
            if (r.keyBits <= 0) r.keyBits = 2048;
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, CreateCertificateRequest const &obj) {
            boost::json::array names;
            for (const auto &name: obj.subjectAltNames) names.emplace_back(name);

            jv = {
                    {"name", obj.name},
                    {"description", obj.description},
                    {"commonName", obj.commonName},
                    {"subjectAltNames", names},
                    {"validDays", obj.validDays},
                    {"keyBits", obj.keyBits},
            };
        }
    };

}// namespace Euclid::Dto::EKM
