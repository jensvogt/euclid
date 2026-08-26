#pragma once

// C++ includes
#include <string>

namespace Euclid::Database::Entity {

    /**
     * @brief Shared account/namespace/region scoping fields for resource entities (Queue, Bucket,
     * Object, ...).
     *
     * Plain data, no virtual methods - entities inherit it purely so entity.accountId/entity.region
     * keep working as direct member access (no vtable, no extra indirection at call sites). Each
     * entity still writes its own toDocument()/fromDocument(), same as every other entity in this
     * codebase - there's no shared serialization dispatch here.
     */
    struct BaseEntity {

        /**
         * @brief Region the resource was created in.
         */
        std::string region;

        /**
         * @brief Account the resource belongs to.
         */
        std::string accountId;

        /**
         * @brief Namespace within accountId the resource belongs to. Empty means unscoped by
         * namespace (not "all namespaces") - "namespace" is a reserved word, so the field is named
         * namespaceName; it is serialized as "namespace" in BSON/JSON.
         */
        std::string namespaceName;
    };

}
