//
// Created by vogje01 on 8/16/26.
//

#pragma once

// C++ includes
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::COM {

    /**
     * @brief Raw binary payload for a message attribute.
     */
    using Binary = std::vector<unsigned char>;

    namespace detail {

        constexpr char kBase64Chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

        inline std::string ToBase64(const Binary &data) {
            std::string out;
            out.reserve((data.size() + 2) / 3 * 4);
            std::size_t i = 0;
            for (; i + 3 <= data.size(); i += 3) {
                const auto n = static_cast<uint32_t>(data[i]) << 16 | static_cast<uint32_t>(data[i + 1]) << 8 | static_cast<uint32_t>(data[i + 2]);
                out += kBase64Chars[n >> 18 & 0x3F];
                out += kBase64Chars[n >> 12 & 0x3F];
                out += kBase64Chars[n >> 6 & 0x3F];
                out += kBase64Chars[n & 0x3F];
            }
            if (const std::size_t remaining = data.size() - i; remaining == 1) {
                const auto n = static_cast<uint32_t>(data[i]) << 16;
                out += kBase64Chars[n >> 18 & 0x3F];
                out += kBase64Chars[n >> 12 & 0x3F];
                out += "==";
            } else if (remaining == 2) {
                const auto n = static_cast<uint32_t>(data[i]) << 16 | static_cast<uint32_t>(data[i + 1]) << 8;
                out += kBase64Chars[n >> 18 & 0x3F];
                out += kBase64Chars[n >> 12 & 0x3F];
                out += kBase64Chars[n >> 6 & 0x3F];
                out += "=";
            }
            return out;
        }

        inline int DecodeBase64Char(const char c) {
            if (c >= 'A' && c <= 'Z') return c - 'A';
            if (c >= 'a' && c <= 'z') return c - 'a' + 26;
            if (c >= '0' && c <= '9') return c - '0' + 52;
            if (c == '+') return 62;
            if (c == '/') return 63;
            return -1;
        }

        inline Binary FromBase64(const std::string &input) {
            Binary out;
            out.reserve(input.size() / 4 * 3);
            int val = 0, bits = -8;
            for (const char c: input) {
                if (c == '=') break;
                const int d = DecodeBase64Char(c);
                if (d < 0) continue;
                val = val << 6 | d;
                bits += 6;
                if (bits >= 0) {
                    out.push_back(static_cast<unsigned char>(val >> bits & 0xFF));
                    bits -= 8;
                }
            }
            return out;
        }

    }// namespace detail

    /**
     * @brief Typed value of a message attribute.
     *
     * Wraps a std::variant so the concrete C++ type (int, long, double, float, bool,
     * string or binary) survives a JSON round-trip via an explicit "type" discriminator.
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

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, Variant const &obj) {
            std::visit([&jv]<typename T>(T const &v) {
                           if constexpr (std::is_same_v<T, Binary>) {
                               jv = {{"type", "binary"}, {"value", detail::ToBase64(v)}};
                           } else if constexpr (std::is_same_v<T, int>) {
                               jv = {{"type", "int"}, {"value", v}};
                           } else if constexpr (std::is_same_v<T, long>) {
                               jv = {{"type", "long"}, {"value", v}};
                           } else if constexpr (std::is_same_v<T, double>) {
                               jv = {{"type", "double"}, {"value", v}};
                           } else if constexpr (std::is_same_v<T, float>) {
                               jv = {{"type", "float"}, {"value", v}};
                           } else if constexpr (std::is_same_v<T, bool>) {
                               jv = {{"type", "bool"}, {"value", v}};
                           } else if constexpr (std::is_same_v<T, std::string>) {
                               jv = {{"type", "string"}, {"value", v}};
                           }
                       },
                       obj.value);
        }

        friend Variant tag_invoke(boost::json::value_to_tag<Variant>, boost::json::value const &v) {
            Variant r;
            const std::string type = Core::GetStringValue(v, "type");
            const boost::json::value &val = v.at("value");
            if (type == "int") r.value = static_cast<int>(val.as_int64());
            else if (type == "long") r.value = val.as_int64();
            else if (type == "double") r.value = val.as_double();
            else if (type == "float") r.value = static_cast<float>(val.as_double());
            else if (type == "bool") r.value = val.as_bool();
            else if (type == "string") r.value = std::string(val.as_string());
            else if (type == "binary") r.value = detail::FromBase64(std::string(val.as_string()));
            return r;
        }
    };

}// namespace Euclid::Dto::EQS