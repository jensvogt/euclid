//
// Created by vogje01 on 8/16/26.
//

#include <euclid/database/entity/sqs/Variant.h>

// MongoDB includes
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/document/value.hpp>
#include <bsoncxx/document/view.hpp>
#include <bsoncxx/types.hpp>

namespace Euclid::Database::Entity::SQS {

    bsoncxx::document::value Variant::ToDocument() const {
        bsoncxx::builder::basic::document doc;
        std::visit([&doc]<typename T>(const T &v) {
                       if constexpr (std::is_same_v<T, Binary>) {
                           doc.append(bsoncxx::builder::basic::kvp("type", "binary"));
                           doc.append(bsoncxx::builder::basic::kvp("value", bsoncxx::types::b_binary{bsoncxx::binary_sub_type::k_binary, static_cast<uint32_t>(v.size()), v.data()}));
                       } else if constexpr (std::is_same_v<T, int>) {
                           doc.append(bsoncxx::builder::basic::kvp("type", "int"));
                           doc.append(bsoncxx::builder::basic::kvp("value", v));
                       } else if constexpr (std::is_same_v<T, long>) {
                           doc.append(bsoncxx::builder::basic::kvp("type", "long"));
                           doc.append(bsoncxx::builder::basic::kvp("value", static_cast<int64_t>(v)));
                       } else if constexpr (std::is_same_v<T, double>) {
                           doc.append(bsoncxx::builder::basic::kvp("type", "double"));
                           doc.append(bsoncxx::builder::basic::kvp("value", v));
                       } else if constexpr (std::is_same_v<T, float>) {
                           doc.append(bsoncxx::builder::basic::kvp("type", "float"));
                           doc.append(bsoncxx::builder::basic::kvp("value", static_cast<double>(v)));
                       } else if constexpr (std::is_same_v<T, bool>) {
                           doc.append(bsoncxx::builder::basic::kvp("type", "bool"));
                           doc.append(bsoncxx::builder::basic::kvp("value", v));
                       } else if constexpr (std::is_same_v<T, std::string>) {
                           doc.append(bsoncxx::builder::basic::kvp("type", "string"));
                           doc.append(bsoncxx::builder::basic::kvp("value", v));
                       }
                   },
                   value);
        return doc.extract();
    }

    void Variant::FromDocument(const std::optional<bsoncxx::document::view> &document) {
        if (!document) return;
        const auto typeEl = (*document)["type"];
        if (!typeEl) return;
        const std::string type = std::string(typeEl.get_string().value);
        const auto valEl = (*document)["value"];
        if (!valEl) return;
        if (type == "int") value = valEl.get_int32().value;
        else if (type == "long") value = static_cast<long>(valEl.get_int64().value);
        else if (type == "double") value = valEl.get_double().value;
        else if (type == "float") value = static_cast<float>(valEl.get_double().value);
        else if (type == "bool") value = valEl.get_bool().value;
        else if (type == "string") value = std::string(valEl.get_string().value);
        else if (type == "binary") {
            const auto bin = valEl.get_binary();
            value = Binary(bin.bytes, bin.bytes + bin.size);
        }
    }

}// namespace Euclid::Database::Entity::SQS
