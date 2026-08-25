#pragma once

// Euclid include
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/eam/model/Account.h>

namespace Euclid::Dto::EAM {

    struct CreateAccountResponse : BaseDto {

        /**
         * @brief The newly created account
         */
        Account account;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend CreateAccountResponse tag_invoke(boost::json::value_to_tag<CreateAccountResponse>, boost::json::value const &v) {
            CreateAccountResponse r;
            static_cast<BaseDto &>(r) = GetMetadata(v);
            r.account = boost::json::value_to<Account>(v.at("account"));
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, CreateAccountResponse const &obj) {
            jv = {
                    {"account", boost::json::value_from(obj.account)},
            };
        }
    };

}
