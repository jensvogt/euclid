// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/7/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::EKM {

    using Json::tag_invoke;

    struct DeleteCertificateResponse : BaseDto {

        /**
         * @brief Euclid resource name of the certificate that was deleted
         */
        std::string ern;

        /**
         * @brief Certificate name
         */
        std::string name;

        /**
         * @brief Serializes this response to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this response from a JSON string
         */
        [[nodiscard]] static DeleteCertificateResponse fromJson(const std::string &json) {
            return boost::json::value_to<DeleteCertificateResponse>(Core::ParseJsonString(json));
        }
    };

    BOOST_DESCRIBE_STRUCT(DeleteCertificateResponse, (BaseDto), (ern, name))

}// namespace Euclid::Dto::EKM
