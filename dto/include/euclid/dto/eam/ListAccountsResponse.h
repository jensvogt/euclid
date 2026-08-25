#pragma once

// C++ includes
#include <vector>

// Euclid includes
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/eam/model/Account.h>

namespace Euclid::Dto::EAM {

    struct ListAccountsResponse : BaseDto {

        /**
         * @brief Accounts
         */
        std::vector<Account> accounts;

        /**
         * @brief total number of accounts
         */
        long total{};

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend ListAccountsResponse tag_invoke(boost::json::value_to_tag<ListAccountsResponse>, boost::json::value const &v) {
            ListAccountsResponse r;
            static_cast<BaseDto &>(r) = GetMetadata(v);
            r.accounts = boost::json::value_to<std::vector<Account> >(v.at("accounts"));
            r.total = Core::GetLongValue(v, "total");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, ListAccountsResponse const &obj) {
            jv = {
                    {"accounts", boost::json::value_from(obj.accounts)},
                    {"total", obj.total},
            };
        }
    };

}
