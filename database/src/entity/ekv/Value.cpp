//
// Created by vogje01 on 9/10/26.
//

#include <euclid/core/CryptoUtils.h>
#include <euclid/database/entity/ekv/Value.h>

namespace Euclid::Database::Entity::EKV {

    namespace builder = bsoncxx::builder::basic;

    std::string Value::TypeName() const {
        return std::visit([]<typename T>(const T &) -> std::string {
            using Held = std::decay_t<T>;
            if constexpr (std::is_same_v<Held, std::monostate>) return "null";
            else if constexpr (std::is_same_v<Held, bool>) return "boolean";
            else if constexpr (std::is_same_v<Held, std::int64_t>) return "number";
            else if constexpr (std::is_same_v<Held, double>) return "number";
            else if constexpr (std::is_same_v<Held, std::string>) return "string";
            else if constexpr (std::is_same_v<Held, COM::Binary>) return "binary";
            else if constexpr (std::is_same_v<Held, List>) return "list";
            else return "map";
        },
                          value);
    }

    bool Value::IsKeyable() const {
        return holds<std::string>() || holds<std::int64_t>() || holds<double>() || holds<COM::Binary>();
    }

    bool Value::IsAcceptableAttributeName(const std::string &name) {
        if (name.empty()) return false;
        if (name.front() == '$') return false;
        return name.find('.') == std::string::npos;
    }

    // ── JSON ─────────────────────────────────────────────────────────────────

    Value Value::FromJson(const boost::json::value &json, const int depth) {

        if (depth > kMaxDepth) throw std::runtime_error("Value nests more than " + std::to_string(kMaxDepth) + " deep");

        switch (json.kind()) {

            case boost::json::kind::null:
                return {};

            case boost::json::kind::bool_:
                return Value{json.get_bool()};

            case boost::json::kind::int64:
                return Value{json.get_int64()};

            case boost::json::kind::uint64:
                // Anything that fits stays an integer; the rest becomes a double, which is what it
                // would have to be to be written at all.
                if (json.get_uint64() <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                    return Value{static_cast<std::int64_t>(json.get_uint64())};
                }
                return Value{static_cast<double>(json.get_uint64())};

            case boost::json::kind::double_:
                return Value{json.get_double()};

            case boost::json::kind::string:
                return Value{std::string(json.get_string())};

            case boost::json::kind::array: {
                List list;
                list.reserve(json.get_array().size());
                for (const auto &element: json.get_array()) list.push_back(FromJson(element, depth + 1));
                return Value{std::move(list)};
            }

            case boost::json::kind::object: {
                Map map;
                for (const auto &[name, element]: json.get_object()) {
                    const std::string attribute(name);
                    if (!IsAcceptableAttributeName(attribute)) {
                        throw std::runtime_error("Attribute name '" + attribute + "' cannot be stored: names may not be empty, "
                                                                                  "start with '$' or contain '.'");
                    }
                    map.emplace(attribute, FromJson(element, depth + 1));
                }
                return Value{std::move(map)};
            }
        }
        return {};
    }

    boost::json::value Value::ToJson() const {

        return std::visit([]<typename T>(const T &held) -> boost::json::value {
            using Held = std::decay_t<T>;

            if constexpr (std::is_same_v<Held, std::monostate>) {
                return nullptr;
            } else if constexpr (std::is_same_v<Held, COM::Binary>) {
                const auto encoded = Core::CryptoUtils::Base64Encode(std::string(held.begin(), held.end()));
                return boost::json::value(std::string_view(encoded));
            } else if constexpr (std::is_same_v<Held, List>) {
                boost::json::array array;
                array.reserve(held.size());
                for (const auto &element: held) array.push_back(element.ToJson());
                return array;
            } else if constexpr (std::is_same_v<Held, Map>) {
                boost::json::object object;
                for (const auto &[name, element]: held) object[name] = element.ToJson();
                return object;
            } else if constexpr (std::is_same_v<Held, std::string>) {
                return boost::json::value(std::string_view(held));
            } else {
                return held;
            }
        },
                          value);
    }

    // ── BSON ─────────────────────────────────────────────────────────────────

    namespace {

        // The value as something a builder will take. Written once and used for both a document's
        // key and an array's element, which is the only difference between the two overloads.
        template<typename Appender>
        void append(const Value &value, const Appender &appender) {

            std::visit([&appender]<typename T>(const T &held) {
                using Held = std::decay_t<T>;

                if constexpr (std::is_same_v<Held, std::monostate>) {
                    appender(bsoncxx::types::b_null{});
                } else if constexpr (std::is_same_v<Held, COM::Binary>) {
                    appender(bsoncxx::types::b_binary{bsoncxx::binary_sub_type::k_binary,
                                                      static_cast<std::uint32_t>(held.size()), held.data()});
                } else if constexpr (std::is_same_v<Held, List>) {
                    builder::array array;
                    for (const auto &element: held) element.AppendTo(array);
                    appender(array.extract());
                } else if constexpr (std::is_same_v<Held, Map>) {
                    builder::document document;
                    for (const auto &[name, element]: held) element.AppendTo(document, name);
                    appender(document.extract());
                } else {
                    appender(held);
                }
            },
                       value.value);
        }

    }// namespace

    void Value::AppendTo(builder::document &document, const std::string &key) const {
        append(*this, [&document, &key](auto &&held) { document.append(builder::kvp(key, std::forward<decltype(held)>(held))); });
    }

    void Value::AppendTo(builder::array &array) const {
        append(*this, [&array](auto &&held) { array.append(std::forward<decltype(held)>(held)); });
    }

    Value Value::FromBson(const bsoncxx::types::bson_value::view &view) {

        switch (view.type()) {

            case bsoncxx::type::k_null:
                return {};

            case bsoncxx::type::k_bool:
                return Value{view.get_bool().value};

            case bsoncxx::type::k_int32:
                return Value{static_cast<std::int64_t>(view.get_int32().value)};

            case bsoncxx::type::k_int64:
                return Value{view.get_int64().value};

            case bsoncxx::type::k_double:
                return Value{view.get_double().value};

            case bsoncxx::type::k_string:
                return Value{std::string(view.get_string().value)};

            case bsoncxx::type::k_binary: {
                const auto binary = view.get_binary();
                return Value{COM::Binary(binary.bytes, binary.bytes + binary.size)};
            }

            case bsoncxx::type::k_array: {
                List list;
                for (const auto &element: view.get_array().value) list.push_back(FromBson(element.get_value()));
                return Value{std::move(list)};
            }

            case bsoncxx::type::k_document:
                return Value{MapFromBson(view.get_document().value)};

            default:
                // Nothing writes the remaining BSON types here, so anything else is a document
                // somebody else put in the collection. Read as null rather than refused: one
                // unexpected field should not make an item unreadable.
                return {};
        }
    }

    Map Value::MapFromBson(const bsoncxx::document::view &view) {

        Map map;
        for (const auto &element: view) map.emplace(std::string(element.key()), FromBson(element.get_value()));
        return map;
    }

}// namespace Euclid::Database::Entity::EKV
