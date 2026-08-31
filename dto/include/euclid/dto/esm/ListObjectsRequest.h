//
// Created by vogje01 on 8/23/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::ESM {

    struct ListObjectsRequest {

        /**
         * @brief Bucket ERN
         */
        std::string bucketErn{};

        /**
         * @brief Bucket name prefix
         */
        std::string prefix{};

        /**
         * @brief Page size
         */
        long pageSize = 10;

        /**
         * @brief Page index
         */
        long pageIndex = 0;

        /**
         * @brief Sorting columns
         */
        std::string sortColumn = "name";

        /**
         * @brief Sort direction
         */
        std::string sortDirection = "asc";

        /**
         * @brief Whether directory keys are listed too.
         *
         * @par
         * A directory is a zero-byte object whose key ends in "/" (see
         * Database::Entity::ESM::IsDirectoryKey()), so leaving these out by default keeps them
         * out of every listing that only cares about files - the RUI, the CLI, an SDK. A
         * transfer server, which has to show and manage directories, sets this.
         */
        bool includeDirectories = false;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend ListObjectsRequest tag_invoke(boost::json::value_to_tag<ListObjectsRequest>, boost::json::value const &v) {
            ListObjectsRequest r;
            r.bucketErn = Core::GetStringValue(v, "bucketErn");
            r.prefix = Core::GetStringValue(v, "prefix");
            r.pageSize = Core::GetLongValue(v, "pageSize");
            r.pageIndex = Core::GetLongValue(v, "pageIndex");
            r.sortColumn = Core::GetStringValue(v, "sortColumn");
            r.sortDirection = Core::GetStringValue(v, "sortDirection");
            r.includeDirectories = Core::GetBoolValue(v, "includeDirectories");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, ListObjectsRequest const &obj) {
            jv = {
                    {"bucketErn", obj.bucketErn},
                    {"prefix", obj.prefix},
                    {"pageSize", obj.pageSize},
                    {"pageIndex", obj.pageIndex},
                    {"sortColumn", obj.sortColumn},
                    {"sortDirection", obj.sortDirection},
                    {"includeDirectories", obj.includeDirectories},
            };
        }
    };

}// namespace Euclid::Dto::ESM