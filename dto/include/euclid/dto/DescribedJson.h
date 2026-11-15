// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <algorithm>
#include <chrono>
#include <string>
#include <string_view>
#include <type_traits>

// Boost includes
#include <boost/describe.hpp>
#include <boost/json.hpp>
#include <boost/mp11/algorithm.hpp>

// Euclid includes
#include <euclid/core/DateTimeUtils.h>
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/BaseDto.h>

/**
 * @file DescribedJson.h
 *
 * @brief JSON conversion for a DTO from its member list, instead of a hand-written pair of
 * tag_invoke functions that name every field twice.
 *
 * @par
 * A DTO lists its members once, after the struct:
 * @code
 * BOOST_DESCRIBE_STRUCT(CreateQueueResponse, (BaseDto), (name, ern))
 * @endcode
 * and its namespace pulls in the conversion with `using Json::tag_invoke;`. boost::json::value_to
 * and value_from - and through them toJson()/fromJson(), and every vector, map or other DTO that
 * holds one - then convert it with the rules below.
 *
 * @par Reading
 * The rules the Core::Get*Value helpers applied field by field, so that a DTO converted to this
 * reads exactly what it read before:
 * - a member that is missing or null is reset to its type's zero value ("", 0, false, an empty
 *   container) - not to its initializer - unless the DTO lists it in kJsonDefaults, in which case it
 *   keeps its initializer;
 * - a member the DTO lists in kJsonRequired must be there: missing, it throws as
 *   boost::json::value::at() does, and null, it throws as reading null as its type does;
 * - a string, integer, boolean or floating-point member must be that JSON type, and throws
 *   otherwise, as as_string(), as_int64(), as_bool() and as_double() do;
 * - a time_point is an ISO-8601 string;
 * - anything else - a container, a COM::Variant, another DTO - goes through boost::json::value_to.
 *
 * @par Writing
 * Every member under its own name, a time_point as ISO-8601, and a DTO derived from BaseDto with
 * the base under "metadata", as every response does - so its module has to fill it in, or it goes
 * out as empty strings.
 *
 * @par
 * A DTO whose JSON does not fit these rules keeps its own tag_invoke for that direction: a
 * hand-written friend is not a template, and wins over these. Defining only the value_to_tag one
 * keeps the generic writer, and the other way round.
 */
namespace Euclid::Dto::Json {

    template<class T>
    concept Described = boost::describe::has_describe_members<T>::value;

    namespace detail {

        using TimePoint = std::chrono::system_clock::time_point;

        // Whether T keeps member's initializer when the JSON leaves it out: T lists it in
        // `static constexpr std::array kJsonDefaults{...}`.
        template<class T>
        constexpr bool KeepsDefault(const std::string_view member) {
            if constexpr (requires { T::kJsonDefaults; }) {
                return std::ranges::find(T::kJsonDefaults, member) != std::ranges::end(T::kJsonDefaults);
            } else {
                return false;
            }
        }

        // Whether T insists on member: T lists it in `static constexpr std::array kJsonRequired{...}`.
        template<class T>
        constexpr bool IsRequired(const std::string_view member) {
            if constexpr (requires { T::kJsonRequired; }) {
                return std::ranges::find(T::kJsonRequired, member) != std::ranges::end(T::kJsonRequired);
            } else {
                return false;
            }
        }

        template<class M>
        M Read(const boost::json::value &v) {
            if constexpr (std::is_same_v<M, std::string>) {
                return std::string(v.as_string());
            } else if constexpr (std::is_same_v<M, bool>) {
                return v.as_bool();
            } else if constexpr (std::is_integral_v<M>) {
                return static_cast<M>(v.as_int64());
            } else if constexpr (std::is_floating_point_v<M>) {
                return static_cast<M>(v.as_double());
            } else if constexpr (std::is_same_v<M, TimePoint>) {
                return Core::DateTimeUtils::FromISO8601(std::string(v.as_string()));
            } else {
                return boost::json::value_to<M>(v);
            }
        }

        template<class M>
        boost::json::value Write(const M &member) {
            if constexpr (std::is_same_v<M, TimePoint>) {
                return boost::json::value(Core::DateTimeUtils::ToISO8601(member));
            } else {
                return boost::json::value_from(member);
            }
        }

        template<class T>
        constexpr void CheckBases() {
            boost::mp11::mp_for_each<boost::describe::describe_bases<T, boost::describe::mod_any_access> >([](auto base) {
                using Base = typename decltype(base)::type;
                static_assert(std::is_same_v<Base, BaseDto>, "a described DTO may only derive from BaseDto, which is written as \"metadata\"");
            });
        }
    }// namespace detail

    /**
     * @brief Reads a described DTO from a JSON object.
     */
    template<Described T>
    T FromJson(const boost::json::value &v) {
        detail::CheckBases<T>();
        T result{};
        if constexpr (std::is_base_of_v<BaseDto, T>) {
            static_cast<BaseDto &>(result) = GetMetadata(v);
        }
        boost::mp11::mp_for_each<boost::describe::describe_members<T, boost::describe::mod_public> >([&](auto member) {
            auto &field = result.*member.pointer;
            using M = std::remove_cvref_t<decltype(field)>;
            if (detail::IsRequired<T>(member.name)) {
                // at() throws for a missing member, and Read() for a null one.
                field = detail::Read<M>(v.at(member.name));
            } else if (Core::AttributeExists(v, member.name)) {
                field = detail::Read<M>(v.at(member.name));
            } else if (!detail::KeepsDefault<T>(member.name)) {
                field = M{};
            }
        });
        return result;
    }

    /**
     * @brief Writes a described DTO as a JSON object.
     */
    template<Described T>
    void ToJson(boost::json::value &jv, const T &dto) {
        detail::CheckBases<T>();
        boost::json::object object;
        if constexpr (std::is_base_of_v<BaseDto, T>) {
            object["metadata"] = boost::json::value_from(static_cast<const BaseDto &>(dto));
        }
        boost::mp11::mp_for_each<boost::describe::describe_members<T, boost::describe::mod_public> >([&](auto member) {
            object[member.name] = detail::Write(dto.*member.pointer);
        });
        jv = std::move(object);
    }

    // The customisation points boost::json looks up by argument-dependent lookup. A DTO namespace
    // brings them in with `using Json::tag_invoke;`.

    template<Described T>
    T tag_invoke(boost::json::value_to_tag<T>, const boost::json::value &v) {
        return FromJson<T>(v);
    }

    template<Described T>
    void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, const T &dto) {
        ToJson(jv, dto);
    }

}// namespace Euclid::Dto::Json
