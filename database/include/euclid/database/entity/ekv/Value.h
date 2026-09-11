// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/10/26.
//

#pragma once

// C++ includes
#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>

// Boost includes
#include <boost/json.hpp>

// MongoDB includes
#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/document/view.hpp>
#include <bsoncxx/types.hpp>
#include <bsoncxx/types/bson_value/view.hpp>

// Euclid includes
#include <euclid/database/entity/com/Variant.h>

namespace Euclid::Database::Entity::EKV {

    struct Value;

    /**
     * @brief An ordered list of values.
     */
    using List = std::vector<Value>;

    /**
     * @brief An item's attributes, or a nested object's. Ordered by name, so that a document
     * written and read back comes out in a predictable order rather than in whatever order a hash
     * happened to produce.
     */
    using Map = std::map<std::string, Value>;

    /**
     * @brief One attribute value: a scalar, or a list or map of them.
     *
     * @par
     * The same idea as Entity::COM::Variant - a C++ type that survives a round trip through the
     * database because the type is stored rather than guessed - with the one difference this store
     * needs: it nests. A message attribute is a scalar and always will be; an item is a document,
     * and a document whose values could not themselves be documents would be a poor one.
     *
     * @par
     * Kept separate from COM::Variant rather than growing it, because that type is what EQS, ENS
     * and ESM store their attributes as, and changing the shape of something four modules read
     * from the database is not a thing to do in passing for the sake of a fifth.
     *
     * @par
     * Binary has no JSON spelling, so nothing arriving over the wire can be one today; it is here
     * because the type mapping to BSON is what it is, and because an SDK that wants to write bytes
     * should not have to wait for a new storage format.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    struct Value {

        /**
         * @brief The value. std::monostate is a stored null - an attribute that is present and
         * empty, which is not the same as one that is absent.
         */
        std::variant<std::monostate, bool, std::int64_t, double, std::string, COM::Binary, List, Map> value;

        Value() = default;

        template<typename T>
            requires(!std::is_same_v<std::decay_t<T>, Value> && std::is_constructible_v<decltype(value), T>)
        explicit Value(T &&v) : value(std::forward<T>(v)) {}

        /**
         * @brief Whether the value holds a T.
         */
        template<typename T>
        [[nodiscard]] bool holds() const { return std::holds_alternative<T>(value); }

        /**
         * @brief The value as a T.
         *
         * @throws std::bad_variant_access if it holds something else.
         */
        template<typename T>
        [[nodiscard]] const T &get() const { return std::get<T>(value); }

        /**
         * @brief Whether this is a stored null.
         */
        [[nodiscard]] bool IsNull() const { return std::holds_alternative<std::monostate>(value); }

        /**
         * @brief What kind of value this is, for an error message: "string", "number", "list", ...
         */
        [[nodiscard]] std::string TypeName() const;

        /**
         * @brief Whether this may be a partition or sort key.
         *
         * @par
         * Only a string, a number or binary. Not because the others could not be stored, but
         * because a key is compared and ordered, and there is no answer to whether one map sorts
         * before another that anybody would want to depend on.
         */
        [[nodiscard]] bool IsKeyable() const;

        /**
         * @brief Reads a value from JSON, as a caller wrote it.
         *
         * @param json the value to read.
         * @param depth how deep this already is; nesting is bounded (see kMaxDepth).
         * @return the value.
         * @throws std::runtime_error if it nests too deeply, or names an attribute the store
         * cannot hold (see IsAcceptableAttributeName).
         */
        static Value FromJson(const boost::json::value &json, int depth = 0);

        /**
         * @brief The value as JSON, to answer a caller with.
         *
         * @par
         * Binary comes back base64-encoded, which is the only thing JSON can do with it - and the
         * reason nothing may be written that way, since the store would otherwise not be able to
         * say whether a string is a string.
         */
        [[nodiscard]] boost::json::value ToJson() const;

        /**
         * @brief Appends the value to a document under @p key.
         */
        void AppendTo(bsoncxx::builder::basic::document &document, const std::string &key) const;

        /**
         * @brief Appends the value to an array.
         */
        void AppendTo(bsoncxx::builder::basic::array &array) const;

        /**
         * @brief Reads a value back from BSON.
         */
        static Value FromBson(const bsoncxx::types::bson_value::view &view);

        /**
         * @brief Reads a whole document back as a map.
         */
        static Map MapFromBson(const bsoncxx::document::view &view);

        /**
         * @brief How deeply a value may nest. Deep enough for any document somebody meant to
         * write, shallow enough that nothing recursive here can be handed a stack overflow.
         */
        static constexpr int kMaxDepth = 32;

        /**
         * @brief Whether an attribute may be called this.
         *
         * @par
         * Empty names, names starting with '$' and names containing '.' are refused. Those are
         * what a BSON document cannot hold without escaping, and escaping would mean what comes
         * back is not quite what went in - a worse answer than saying no at the door.
         */
        static bool IsAcceptableAttributeName(const std::string &name);
    };

}// namespace Euclid::Database::Entity::EKV
