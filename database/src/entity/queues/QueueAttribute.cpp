//
// Created by vogje01 on 12/10/23.
//

#include <euclid/database/entity/queues/QueueAttribute.h>

namespace Euclid::Database::Entity::Queues {

    namespace {
        long getBsonInt(const bsoncxx::document::element &field) {
            if (field.type() == bsoncxx::type::k_int64) return field.get_int64().value;
            if (field.type() == bsoncxx::type::k_int32) return field.get_int32().value;
            return 0;
        }
    }

    bsoncxx::document::value QueueAttribute::ToDocument() const {
        return bsoncxx::builder::basic::make_document(
                bsoncxx::builder::basic::kvp("delaySeconds", static_cast<int32_t>(delaySeconds)),
                bsoncxx::builder::basic::kvp("maxMessageSize", static_cast<int32_t>(maxMessageSize)),
                bsoncxx::builder::basic::kvp("messageRetentionPeriod", static_cast<int32_t>(messageRetentionPeriod)),
                bsoncxx::builder::basic::kvp("receiveMessageWaitTime", static_cast<int32_t>(receiveMessageWaitTime)),
                bsoncxx::builder::basic::kvp("visibilityTimeout", static_cast<int32_t>(visibilityTimeout)),
                bsoncxx::builder::basic::kvp("policy", policy),
                bsoncxx::builder::basic::kvp("redriveAllowPolicy", redriveAllowPolicy),
                bsoncxx::builder::basic::kvp("approximateNumberOfMessages", static_cast<int32_t>(approximateNumberOfMessages)),
                bsoncxx::builder::basic::kvp("approximateNumberOfMessagesDelayed", static_cast<int32_t>(approximateNumberOfMessagesDelayed)),
                bsoncxx::builder::basic::kvp("approximateNumberOfMessagesNotVisible", static_cast<int32_t>(approximateNumberOfMessagesNotVisible)),
                bsoncxx::builder::basic::kvp("queueErn", queueErn),
                bsoncxx::builder::basic::kvp("redrivePolicy", redrivePolicy.ToDocument()));
    }

    void QueueAttribute::FromDocument(const std::optional<bsoncxx::document::view> &document) {
        if (!document) return;
        for (const auto &field: *document) {
            if (const auto key = field.key(); key == "delaySeconds") delaySeconds = getBsonInt(field);
            else if (key == "maxMessageSize") maxMessageSize = getBsonInt(field);
            else if (key == "messageRetentionPeriod") messageRetentionPeriod = getBsonInt(field);
            else if (key == "receiveMessageWaitTime") receiveMessageWaitTime = getBsonInt(field);
            else if (key == "visibilityTimeout") visibilityTimeout = getBsonInt(field);
            else if (key == "policy") policy = std::string(field.get_string().value);
            else if (key == "redriveAllowPolicy") redriveAllowPolicy = std::string(field.get_string().value);
            else if (key == "approximateNumberOfMessages") approximateNumberOfMessages = getBsonInt(field);
            else if (key == "approximateNumberOfMessagesDelayed") approximateNumberOfMessagesDelayed = getBsonInt(field);
            else if (key == "approximateNumberOfMessagesNotVisible") approximateNumberOfMessagesNotVisible = getBsonInt(field);
            else if (key == "queueErn") queueErn = std::string(field.get_string().value);
            else if (key == "redrivePolicy") {
                const auto view = field.get_document().view();
                redrivePolicy.FromDocument(view);
            }
        }
    }

}// namespace Euclid::Database::Entity::SQS