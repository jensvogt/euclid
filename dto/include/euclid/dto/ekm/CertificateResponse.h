// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/7/26.
//

#pragma once

// C++ includes
#include <array>
#include <string_view>

// Euclid includes
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/DescribedJson.h>
#include <euclid/dto/ekm/model/Certificate.h>

namespace Euclid::Dto::EKM {

    using Json::tag_invoke;

    /**
     * @brief One certificate, as answered by create-certificate, import-certificate and
     * get-certificate.
     *
     * @par
     * One response type for the three of them because they all answer the same question - what is
     * stored under this name now - and a caller that imports and then reads back should not have
     * to parse two shapes to compare them.
     */
    struct CertificateResponse : BaseDto {

        /**
         * @brief The stored certificate, without its private key
         */
        Certificate certificate;

        /**
         * @brief The members that must be in the JSON: missing or null, reading throws.
         */
        static constexpr std::array<std::string_view, 1> kJsonRequired{"certificate"};

        /**
         * @brief Serializes this response to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }
    };

    BOOST_DESCRIBE_STRUCT(CertificateResponse, (BaseDto), (certificate))

}// namespace Euclid::Dto::EKM
