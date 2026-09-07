//
// Created by vogje01 on 9/7/26.
//

#pragma once

// C++ includes
#include <string>
#include <vector>

// Euclid includes
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/ekm/model/Certificate.h>

namespace Euclid::Dto::EKM {

    struct ListCertificatesResponse : BaseDto {

        /**
         * @brief Certificates list
         */
        std::vector<Certificate> certificates;

        /**
         * @brief Total number of certificates matching the request, ignoring paging
         */
        long total{};

        /**
         * @brief Serializes this response to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend ListCertificatesResponse tag_invoke(boost::json::value_to_tag<ListCertificatesResponse>, boost::json::value const &v) {
            ListCertificatesResponse r;
            static_cast<BaseDto &>(r) = GetMetadata(v);
            r.certificates = boost::json::value_to<std::vector<Certificate> >(v.at("certificates"));
            r.total = Core::GetLongValue(v, "total");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, ListCertificatesResponse const &obj) {
            jv = {
                    {"certificates", boost::json::value_from(obj.certificates)},
                    {"total", obj.total},
            };
        }
    };

}// namespace Euclid::Dto::EKM
