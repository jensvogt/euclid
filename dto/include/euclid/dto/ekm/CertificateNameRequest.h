// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/7/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::EKM {

    using Json::tag_invoke;

    /**
     * @brief Names one certificate, for the actions that need nothing else: get-certificate and
     * delete-certificate.
     */
    struct CertificateNameRequest {

        /**
         * @brief Certificate name, within the caller's own account and namespace
         */
        std::string name;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this request from a JSON string
         */
        [[nodiscard]] static CertificateNameRequest fromJson(const std::string &json) {
            return boost::json::value_to<CertificateNameRequest>(Core::ParseJsonString(json));
        }
    };

    BOOST_DESCRIBE_STRUCT(CertificateNameRequest, (), (name))

}// namespace Euclid::Dto::EKM
