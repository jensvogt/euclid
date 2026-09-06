//
// Created by vogje01 on 8/16/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::EQS {

    struct CreateQueueRequest {

        /**
         * @brief Name of the queue
         */
        std::string name;

        /**
         * @brief Visibility in seconds
         */
        long visibility = 30;

        /**
         * @brief Maximal number of reties
         */
        long maxRetries = 3;

        /**
         * @brief Maximal length of a message in bytes
         */
        long maxMessageLength = 1024 * 1024;

        /**
         * @brief Name of the dead letter queue. Can be empty.
         */
        std::string dlqName;

        /**
         * @brief Delay in seconds for any message send to this queue.
         */
        long delay = 0;

        /**
         * @brief Default priority for new messages
         */
        std::string priority = "MIDDLE";

        /**
         * @brief Whether this queue is euclid's own plumbing rather than a user's queue.
         *
         * @par
         * An internal queue is left out of list-queues and the queue count; it behaves like any
         * other in every other respect. For a delivery that has to land somewhere - a bucket
         * subscription's target, say - where the queue is an implementation detail of whoever
         * registered it. See Database::Entity::EQS::Queue::internal.
         */
        bool internal = false;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this request from a JSON string
         */
        [[nodiscard]] static CreateQueueRequest fromJson(const std::string &json) {
            return boost::json::value_to<CreateQueueRequest>(Core::ParseJsonString(json));
        }

        //curl -X POST http://localhost:8080/api/produktmeldungen/datenlieferantId=jvo -H 'Conteype: application/json' -H "Authorization: Bearer eyJ0eXAiOiJKV1QiLCJhbGciOiJIUzI1NiJ9.eyJpYXQiOjE3ODg2ODcyODEsImV4cCI6MTc4ODY5MDg4MSwic3ViIjoianZvIn0.oW0SsBc_M9RTHNrwf7t7PGVCQkxHhenyl1KeaRCVErs"

    private:

        friend CreateQueueRequest tag_invoke(boost::json::value_to_tag<CreateQueueRequest>, boost::json::value const &v) {
            CreateQueueRequest r;
            r.name = Core::GetStringValue(v, "name");
            r.visibility = Core::GetLongValue(v, "visibility");
            r.maxRetries = Core::GetLongValue(v, "maxRetries");
            r.maxMessageLength = Core::GetLongValue(v, "maxMessageLength");
            r.dlqName = Core::GetStringValue(v, "dlqName");
            r.delay = Core::GetLongValue(v, "delay");
            r.priority = Core::GetStringValue(v, "priority");
            r.internal = Core::GetBoolValue(v, "internal");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, CreateQueueRequest const &obj) {
            jv = {
                    {"name", obj.name},
                    {"visibility", obj.visibility},
                    {"maxRetries", obj.maxRetries},
                    {"maxMessageLength", obj.maxMessageLength},
                    {"dlqName", obj.dlqName},
                    {"delay", obj.delay},
                    {"priority", obj.priority},
                    {"internal", obj.internal},
            };
        }
    };
}