#pragma once

// C++ includes
#include <vector>

// Euclid includes
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/eam/model/Namespace.h>

namespace Euclid::Dto::EAM {

    struct ListNamespacesResponse : BaseDto {

        /**
         * @brief Namespaces
         */
        std::vector<Namespace> namespaces;

        /**
         * @brief total number of namespaces
         */
        long total{};

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend ListNamespacesResponse tag_invoke(boost::json::value_to_tag<ListNamespacesResponse>, boost::json::value const &v) {
            ListNamespacesResponse r;
            static_cast<BaseDto &>(r) = GetMetadata(v);
            r.namespaces = boost::json::value_to<std::vector<Namespace> >(v.at("namespaces"));
            r.total = Core::GetLongValue(v, "total");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, ListNamespacesResponse const &obj) {
            jv = {
                    {"namespaces", boost::json::value_from(obj.namespaces)},
                    {"total", obj.total},
            };
        }
    };

}
