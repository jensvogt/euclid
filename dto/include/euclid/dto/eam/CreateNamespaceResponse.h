#pragma once

// Euclid include
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/eam/model/Namespace.h>

namespace Euclid::Dto::EAM {

    struct CreateNamespaceResponse : BaseDto {

        /**
         * @brief The newly created namespace
         */
        Namespace ns;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend CreateNamespaceResponse tag_invoke(boost::json::value_to_tag<CreateNamespaceResponse>, boost::json::value const &v) {
            CreateNamespaceResponse r;
            static_cast<BaseDto &>(r) = GetMetadata(v);
            r.ns = boost::json::value_to<Namespace>(v.at("namespace"));
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, CreateNamespaceResponse const &obj) {
            jv = {
                    {"namespace", boost::json::value_from(obj.ns)},
            };
        }
    };

}
