//
// Created by vogje01 on 12/10/23.
//

#include <euclid/database/entity/sqs/RedrivePolicy.h>

namespace Euclid::Database::Entity::SQS {

    bsoncxx::document::value RedrivePolicy::ToDocument() const {
        return bsoncxx::builder::basic::make_document(
            bsoncxx::builder::basic::kvp("deadLetterTargetArn", deadLetterTargetArn),
            bsoncxx::builder::basic::kvp("maxReceiveCount", static_cast<int32_t>(maxReceiveCount)));
    }

    void RedrivePolicy::FromDocument(const std::optional<bsoncxx::document::view> &document) {
        if (!document) return;
        for (const auto &field: *document) {
            if (const auto key = field.key(); key == "deadLetterTargetArn") deadLetterTargetArn = std::string(field.get_string().value);
            else if (key == "maxReceiveCount") {
                if (field.type() == bsoncxx::type::k_int32) maxReceiveCount = field.get_int32().value;
                else if (field.type() == bsoncxx::type::k_int64) maxReceiveCount = static_cast<int>(field.get_int64().value);
            }
        }
    }

} // namespace Euclid::Database::Entity::SQS