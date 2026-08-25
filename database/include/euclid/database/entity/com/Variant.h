//
// Created by vogje01 on 8/16/26.
//

#pragma once

// C++ includes
#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

// MongoDB includes
#include <bsoncxx/types.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/document/value-fwd.hpp>
#include <bsoncxx/document/value.hpp>
#include <bsoncxx/document/view-fwd.hpp>
#include <bsoncxx/document/view.hpp>

namespace Euclid::Database::Entity::COM {

    /**
     * @brief Raw binary payload for a message attribute.
     */
    using Binary = std::vector<uint8_t>;

    /**
     * @brief Typed value of a message attribute.
     *
     * Wraps a std::variant so the concrete C++ type (int, long, double, float, bool,
     * string or binary) survives a MongoDB round-trip via an explicit "type" discriminator.
     */
    struct Variant {

        std::variant<int, long, double, float, bool, std::string, Binary> value;

        Variant() = default;

        template<typename T>
            requires(!std::is_same_v<std::decay_t<T>, Variant> && std::is_constructible_v<decltype(value), T>)
        explicit Variant(T &&v) : value(std::forward<T>(v)) {}

        /**
         * @brief Checks whether the value currently holds a T.
         */
        template<typename T>
        [[nodiscard]] bool holds() const {
            return std::holds_alternative<T>(value);
        }

        /**
         * @brief Returns the value as a T, throws std::bad_variant_access if it does not hold a T.
         */
        template<typename T>
        [[nodiscard]] const T &get() const {
            return std::get<T>(value);
        }

        /**
         * @brief Converts the entity to a MongoDB document
         *
         * @return entity as a MongoDB document.
         */
        [[nodiscard]]
        bsoncxx::document::value ToDocument() const;

        /**
         * @brief Converts the MongoDB document to an entity
         *
         * @param document MongoDB document.
         */
        void FromDocument(const std::optional<bsoncxx::document::view> &document);
    };

}// namespace Euclid::Database::Entity::SQS