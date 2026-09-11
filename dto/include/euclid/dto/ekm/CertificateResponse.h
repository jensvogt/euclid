// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/7/26.
//

#pragma once

// Euclid includes
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/ekm/model/Certificate.h>

namespace Euclid::Dto::EKM {

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
         * @brief Serializes this response to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend CertificateResponse tag_invoke(boost::json::value_to_tag<CertificateResponse>, boost::json::value const &v) {
            CertificateResponse r;
            static_cast<BaseDto &>(r) = GetMetadata(v);
            r.certificate = boost::json::value_to<Certificate>(v.at("certificate"));
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, CertificateResponse const &obj) {
            jv = {
                    {"certificate", boost::json::value_from(obj.certificate)},
            };
        }
    };

}// namespace Euclid::Dto::EKM
