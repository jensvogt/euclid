//
// Created by vogje01 on 29/05/2023.
//

// C++ includes
#include <array>
#include <chrono>
#include <map>
#include <thread>

// Euclid includes
#include <euclid/core/ContentTypeUtils.h>
#include <euclid/core/CryptoUtils.h>
#include <euclid/core/UuidUtils.h>
#include <euclid/database/repository/ens/MongoEnsRepository.h>

namespace Euclid::Database {

    MongoEnsRepository::MongoEnsRepository() {
        ensureIndexes();
    }

    void MongoEnsRepository::ensureIndexes() {

        try {
            const auto entry = Database::instance().client();
            auto topicCollection = (*entry)[Database::instance().databaseName()][TOPIC_COLLECTION];

            mongocxx::options::index topicNameOpts;
            topicNameOpts.unique(true);
            topicCollection.create_index(make_document(kvp("name", 1)), topicNameOpts);

            mongocxx::options::index topicErnOpts;
            topicErnOpts.unique(true);
            topicCollection.create_index(make_document(kvp("ern", 1)), topicErnOpts);

            auto subscriptionCollection = (*entry)[Database::instance().databaseName()][SUBSCRIPTION_COLLECTION];
            mongocxx::options::index subscriptionOpts;
            subscriptionOpts.unique(true);
            subscriptionCollection.create_index(make_document(kvp("sourceErn", 1), kvp("type", 1), kvp("targetErn", 1)), subscriptionOpts);
            //
            // auto messageCollection = (*entry)[Database::instance().databaseName()][MESSAGE_COLLECTION];
            //
            // messageCollection.create_index(make_document(kvp("queueErn", 1), kvp("status", 1), kvp("priority", 1)));
            //
            // // Supports resetExpiredMessages()'s per-status sweeps (INVISIBLE, then DELAYED),
            // // which filter by status alone - the compound index above can't serve that, since
            // // queueErn is its leading field and isn't part of that filter.
            // messageCollection.create_index(make_document(kvp("status", 1)));
            //
            // mongocxx::options::index receiptHandleOpts;
            // receiptHandleOpts.sparse(true);
            // messageCollection.create_index(make_document(kvp("receiptHandle", 1)), receiptHandleOpts);
            //
            // mongocxx::options::index messageIdOpts;
            // messageIdOpts.unique(true);
            // messageCollection.create_index(make_document(kvp("messageId", 1)), messageIdOpts);

        } catch (const std::exception &e) {
            log_error << "Ensure ENS indexes failed, error: " << e.what();
        }
    }

    bool MongoEnsRepository::topicExists(const std::string &name) const {

        try {

            document query{};
            if (!name.empty()) {
                query.append(kvp("name", name));
            }

            const auto entry = Database::instance().client();
            auto topicCollection = (*entry)[Database::instance().databaseName()][TOPIC_COLLECTION];

            const auto result = topicCollection.find_one(query.extract());
            log_trace << "Topic exists, name: " << name << ", exists: " << std::boolalpha << result.has_value();
            return result.has_value();

        } catch (const std::exception &e) {
            log_error << "Topic exists failed, name: " << ", error: " << e.what();
        }
        return false;
    }

    //
    // std::optional<Entity::EQS::Queue> MongoEnsRepository::findQueueById(const std::string &oid) const {
    //
    //     try {
    //
    //         document document;
    //         document.append(kvp("_id", oid));
    //
    //         const auto entry = Database::instance().client();
    //         auto queueCollection = (*entry)[Database::instance().databaseName()][QUEUE_COLLECTION];
    //
    //         if (auto mResult = queueCollection.find_one(document.view())) {
    //             return Entity::EQS::Queue::fromDocument(mResult->view());
    //         }
    //
    //     } catch (const std::exception &e) {
    //         log_error << "Get queues by ID failed, error: " << e.what();
    //     }
    //     return {};
    // }

    std::optional<Entity::ENS::Topic> MongoEnsRepository::findTopicByName(const std::string &name) const {

        try {

            const auto entry = Database::instance().client();
            auto topicCollection = (*entry)[Database::instance().databaseName()][TOPIC_COLLECTION];

            if (auto mResult = topicCollection.find_one(make_document(kvp("name", name)))) {
                return Entity::ENS::Topic::fromDocument(mResult.value());
            }

        } catch (const std::exception &e) {
            log_error << "Get topic by name failed, name: " << name << " error: " << e.what();
        }
        return {};
    }

    std::optional<Entity::ENS::Topic> MongoEnsRepository::findTopicByErn(const std::string &ern) const {

        try {

            const auto entry = Database::instance().client();
            auto topicCollection = (*entry)[Database::instance().databaseName()][TOPIC_COLLECTION];

            if (auto mResult = topicCollection.find_one(make_document(kvp("ern", ern)))) {
                return Entity::ENS::Topic::fromDocument(mResult.value());
            }

        } catch (const std::exception &e) {
            log_error << "Get topic by ERN failed, ern: " << ern << " error: " << e.what();
        }
        return {};
    }

    std::vector<Entity::ENS::Topic> MongoEnsRepository::listTopics(const std::string &accountId, const std::string &namespaceName, const std::string &prefix, const long pageSize, const long pageIndex, const std::string &sortColumn) const {

        try {

            document filter = {};
            filter.append(kvp("accountId", accountId));
            if (!namespaceName.empty()) {
                filter.append(kvp("namespace", namespaceName));
            }
            if (!prefix.empty()) {
                filter.append(kvp("name", make_document(kvp("$regex", "^" + prefix))));
            }

            mongocxx::options::find opts;
            if (!sortColumn.empty()) {
                opts.sort(make_document(kvp(sortColumn, 1)));
            }
            if (pageSize > 0) {
                opts.limit(pageSize);
                opts.skip(std::max<long>(pageIndex, 0) * pageSize);
            }

            std::vector<Entity::ENS::Topic> topics;
            const auto entry = Database::instance().client();
            auto queueCollection = (*entry)[Database::instance().databaseName()][TOPIC_COLLECTION];

            for (auto queueCursor = queueCollection.find(filter.view(), opts); auto queue: queueCursor) {
                topics.push_back(Entity::ENS::Topic::fromDocument(queue));
            }
            return topics;

        } catch (const std::exception &e) {

            log_error << "Get ENS topics failed, error: " << e.what();
            return {};
        }
    }

    Entity::ENS::Topic MongoEnsRepository::upsertTopic(Entity::ENS::Topic &topic) {

        try {

            const auto filter = make_document(kvp("name", topic.name));
            const auto update = make_document(
                    kvp("$set", topic.toDocument()),
                    kvp("$setOnInsert", make_document(
                                kvp("created", bsoncxx::types::b_date{
                                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                                    topic.created.time_since_epoch())
                                    })
                                )),
                    kvp("$currentDate", make_document(
                                kvp("modified", true)
                                )));

            mongocxx::options::find_one_and_update opts;
            opts.upsert(true);
            opts.return_document(mongocxx::options::return_document::k_after);

            const auto entry = Database::instance().client();
            auto queueCollection = (*entry)[Database::instance().databaseName()][TOPIC_COLLECTION];

            if (auto result = queueCollection.find_one_and_update(filter.view(), update.view(), opts)) {
                return Entity::ENS::Topic::fromDocument(result->view());
            }

        } catch (const std::exception &e) {
            log_error << "Upsert SQS queue failed, error: " << e.what();
        }
        return topic;
    }


    Entity::ENS::Subscription MongoEnsRepository::upsertSubscription(Entity::ENS::Subscription &subscription) {

        try {

            const auto filter = make_document(kvp("sourceErn", subscription.sourceErn), kvp("type", subscription.type), kvp("targetErn", subscription.targetErn));
            const auto update = make_document(
                    kvp("$set", subscription.toDocument()),
                    kvp("$setOnInsert", make_document(
                                kvp("created", bsoncxx::types::b_date{
                                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                                    subscription.created.time_since_epoch())
                                    })
                                )),
                    kvp("$currentDate", make_document(
                                kvp("modified", true)
                                )));

            mongocxx::options::find_one_and_update opts;
            opts.upsert(true);
            opts.return_document(mongocxx::options::return_document::k_after);

            const auto entry = Database::instance().client();
            auto subscriptionCollection = (*entry)[Database::instance().databaseName()][SUBSCRIPTION_COLLECTION];

            if (auto result = subscriptionCollection.find_one_and_update(filter.view(), update.view(), opts)) {
                return Entity::ENS::Subscription::fromDocument(result->view());
            }
            throw std::runtime_error("upsert returned no document, sourceErn: " + subscription.sourceErn);

        } catch (const std::exception &e) {
            log_error << "Upsert subscription failed, error: " << e.what();
            throw;
        }
    }

    std::vector<Entity::ENS::Subscription> MongoEnsRepository::listSubscriptionsBySourceErn(const std::string &sourceErn) const {

        std::vector<Entity::ENS::Subscription> subscriptions;
        try {
            const auto filter = make_document(kvp("sourceErn", sourceErn));

            const auto entry = Database::instance().client();
            auto subscriptionCollection = (*entry)[Database::instance().databaseName()][SUBSCRIPTION_COLLECTION];

            for (auto cursor = subscriptionCollection.find(filter.view()); auto doc: cursor) {
                subscriptions.push_back(Entity::ENS::Subscription::fromDocument(doc));
            }

        } catch (const std::exception &e) {
            log_error << "List subscriptions failed, sourceErn: " << sourceErn << ", error: " << e.what();
        }
        return subscriptions;
    }

    void MongoEnsRepository::deleteSubscriptionByErn(const std::string &ern) {

        try {
            const auto entry = Database::instance().client();
            auto subscriptionCollection = (*entry)[Database::instance().databaseName()][SUBSCRIPTION_COLLECTION];

            const auto result = subscriptionCollection.delete_many(make_document(kvp("ern", ern)));
            log_debug << "Subscription deleted, ern: " << ern << ", count: " << result->deleted_count();

        } catch (const std::exception &e) {
            log_error << "Delete subscription failed, ern: " << ern << ", error: " << e.what();
        }
    }

    long MongoEnsRepository::countTopics(const std::string &accountId, const std::string &namespaceName) const {

        try {

            document filter = {};
            filter.append(kvp("accountId", accountId));
            if (!namespaceName.empty()) {
                filter.append(kvp("namespace", namespaceName));
            }

            const auto entry = Database::instance().client();
            auto queueCollection = (*entry)[Database::instance().databaseName()][TOPIC_COLLECTION];

            const int64_t count = queueCollection.count_documents(filter.extract());
            log_trace << "Topic count: " << count;
            return static_cast<int>(count);

        } catch (const std::exception &e) {

            log_error << "Topic count failed, error: " << e.what();
        }
        return -1;
    }

    //
    // void MongoEnsRepository::removeQueueByName(const std::string &name) {
    //
    //     try {
    //         const auto entry = Database::instance().client();
    //         auto queueCollection = (*entry)[Database::instance().databaseName()][QUEUE_COLLECTION];
    //         auto messageCollection = (*entry)[Database::instance().databaseName()][MESSAGE_COLLECTION];
    //
    //         std::vector<std::string> erns;
    //         for (auto cursor = queueCollection.find(make_document(kvp("name", name))); auto doc: cursor) {
    //             if (const auto ernField = doc["ern"]; ernField && ernField.type() == bsoncxx::type::k_string) {
    //                 erns.emplace_back(ernField.get_string().value);
    //             }
    //         }
    //
    //         const auto result = queueCollection.delete_many(make_document(kvp("name", name)));
    //         log_debug << "Sqs deleted, count: " << result->deleted_count();
    //
    //         if (!erns.empty()) {
    //             array ernArray;
    //             for (const auto &ern: erns) ernArray.append(ern);
    //             const auto messageResult = messageCollection.delete_many(make_document(kvp("queueErn", make_document(kvp("$in", ernArray.view())))));
    //             log_debug << "Sqs messages deleted, count: " << messageResult->deleted_count();
    //         }
    //
    //     } catch (const std::exception &e) {
    //
    //         log_error << "Delete queues failed, error: " << e.what();
    //     }
    //
    // }
    //
    void MongoEnsRepository::deleteTopicByErn(const std::string &ern) {

        try {
            const auto entry = Database::instance().client();
            auto topicCollection = (*entry)[Database::instance().databaseName()][TOPIC_COLLECTION];
            auto messageCollection = (*entry)[Database::instance().databaseName()][MESSAGE_COLLECTION];

            const auto result = topicCollection.delete_many(make_document(kvp("ern", ern)));
            log_debug << "END topic deleted, count: " << result->deleted_count();

            const auto messageResult = messageCollection.delete_many(make_document(kvp("topicErn", ern)));
            log_debug << "ENS messages deleted, count: " << messageResult->deleted_count();

        } catch (const std::exception &e) {

            log_error << "Delete topic failed, error: " << e.what();
        }

    }

    //
    // void MongoEnsRepository::clearQueues() {
    //
    //     try {
    //         const auto entry = Database::instance().client();
    //         auto queueCollection = (*entry)[Database::instance().databaseName()][QUEUE_COLLECTION];
    //
    //         const auto result = queueCollection.delete_many({});
    //         log_debug << "All queues deleted, count: " << result->deleted_count();
    //
    //     } catch (const std::exception &e) {
    //         log_error << "Delete all queues queues failed, error: " << e.what();
    //     }
    // }
    //
    // bool MongoEnsRepository::messageExists(const std::string &messageId) const {
    //
    //     try {
    //         const auto query = make_document(
    //                 kvp("messageId", messageId));
    //         const auto entry = Database::instance().client();
    //         auto messageCollection = (*entry)[Database::instance().databaseName()][MESSAGE_COLLECTION];
    //
    //         const auto result = messageCollection.find_one(query.view());
    //         return result.has_value();
    //     } catch (const std::exception &e) {
    //         log_error << "Message exists failed, messageId: " << messageId << ", error: " << e.what();
    //     }
    //     return false;
    // }

    std::optional<Entity::ENS::Message> MongoEnsRepository::findMessageById(const std::string &messageId) const {

        try {
            const auto query = make_document(kvp("messageId", messageId));
            const auto entry = Database::instance().client();
            auto messageCollection = (*entry)[Database::instance().databaseName()][MESSAGE_COLLECTION];

            if (auto mResult = messageCollection.find_one(query.view())) {
                Entity::ENS::Message message;
                message.FromDocument(mResult->view());
                return message;
            }
        } catch (const std::exception &e) {
            log_error << "Get message by ID failed, error: " << e.what();
        }
        return {};
    }

    //
    // std::optional<Entity::EQS::Message> MongoEnsRepository::findMessageByName(const std::string &messageId) const {
    //
    //     try {
    //         const auto query = make_document(
    //                 kvp("messageId", messageId));
    //         const auto entry = Database::instance().client();
    //         auto messageCollection = (*entry)[Database::instance().databaseName()][MESSAGE_COLLECTION];
    //
    //         if (auto mResult = messageCollection.find_one(query.view())) {
    //             Entity::EQS::Message message;
    //             message.FromDocument(mResult->view());
    //             return message;
    //         }
    //     } catch (const std::exception &e) {
    //         log_error << "Get message by messageId failed, error: " << e.what();
    //     }
    //     return {};
    // }
    //
    // std::vector<Entity::EQS::Message> MongoEnsRepository::findAllMessages() const {
    //
    //     try {
    //         std::vector<Entity::EQS::Message> messages;
    //         const auto entry = Database::instance().client();
    //         auto messageCollection = (*entry)[Database::instance().databaseName()][MESSAGE_COLLECTION];
    //
    //         for (auto cursor = messageCollection.find({}); auto doc: cursor) {
    //             Entity::EQS::Message message;
    //             message.FromDocument(doc);
    //             messages.push_back(std::move(message));
    //         }
    //         return messages;
    //     } catch (const std::exception &e) {
    //         log_error << "Get all messages failed, error: " << e.what();
    //     }
    //     return {};
    // }

    std::vector<Entity::ENS::Message> MongoEnsRepository::listMessages(const std::string &topicErn, const long pageSize, const long pageIndex, const std::string &sortColumn) const {

        std::vector<Entity::ENS::Message> messages;
        try {
            const auto filter = make_document(kvp("topicErn", topicErn));

            mongocxx::options::find opts;
            if (!sortColumn.empty()) {
                opts.sort(make_document(kvp(sortColumn, 1)));
            }
            if (pageSize > 0) {
                opts.limit(pageSize);
                opts.skip(std::max<long>(pageIndex, 0) * pageSize);
            }

            const auto entry = Database::instance().client();
            auto messageCollection = (*entry)[Database::instance().databaseName()][MESSAGE_COLLECTION];

            for (auto cursor = messageCollection.find(filter.view(), opts); auto doc: cursor) {
                Entity::ENS::Message message;
                message.FromDocument(doc);
                messages.push_back(std::move(message));
            }

        } catch (const std::exception &e) {
            log_error << "List messages failed, topicErn: " << topicErn << ", error: " << e.what();
        }
        return messages;
    }

    void MongoEnsRepository::upsertMessage(const Entity::ENS::Message &message) {

        try {
            const auto filter = make_document(kvp("messageId", message.messageId));
            const auto update = make_document(
                    kvp("$set", message.ToDocument()),
                    kvp("$setOnInsert", make_document(
                                kvp("created", bsoncxx::types::b_date{std::chrono::duration_cast<std::chrono::milliseconds>(message.created.time_since_epoch())}))),
                    kvp("$currentDate", make_document(
                                kvp("modified", true))));

            mongocxx::options::update opts;
            opts.upsert(true);

            const auto entry = Database::instance().client();
            auto messageCollection = (*entry)[Database::instance().databaseName()][MESSAGE_COLLECTION];

            messageCollection.update_one(filter.view(), update.view(), opts);

        } catch (const std::exception &e) {
            log_error << "Upsert message failed, error: " << e.what();
        }
    }

    Entity::ENS::Message MongoEnsRepository::publishMessage(const std::string &messageId, const std::string &ern, const std::string &topicErn, const std::string &body, const std::map<std::string, Entity::COM::Variant> &attributes) {

        Entity::ENS::Message message;
        message.ern = ern;
        message.topicErn = topicErn;
        message.body = body;
        message.size = static_cast<long>(body.size());
        message.messageId = messageId;
        message.md5Body = Core::CryptoUtils::md5Sum(message.body);
        message.contentType = Core::ContentTypeUtils::fromContent(message.body);
        message.attributes = attributes;
        message.md5Attributes = Entity::ENS::Message::ComputeAttributesMd5(attributes);
        // TOdo: fix status
        //        message.status = Entity::ENS::MessageStatus::AVAILABLE;

        try {

            const auto entry = Database::instance().client();
            auto queueCollection = (*entry)[Database::instance().databaseName()][TOPIC_COLLECTION];
            auto messageCollection = (*entry)[Database::instance().databaseName()][MESSAGE_COLLECTION];

            const auto queueFilter = make_document(kvp("ern", topicErn));
            if (auto queueResult = queueCollection.find_one(queueFilter.view())) {
                const auto queue = Entity::ENS::Topic::fromDocument(queueResult->view());

                const auto update = make_document(
                        kvp("$inc", make_document(
                                    kvp("size", static_cast<int64_t>(message.size)),
                                    kvp("available", static_cast<int64_t>(1)))),
                        kvp("$currentDate", make_document(
                                    kvp("modified", true))));
                queueCollection.update_one(queueFilter.view(), update.view());
            }

            messageCollection.insert_one(message.ToDocument());
            log_info << "Message sent, ern: " << ern << ", messageId: " << message.messageId;

        } catch (const std::exception &e) {
            log_error << "Send message failed, ern: " << ern << ", error: " << e.what();
        }
        return message;
    }

    //
    // std::vector<Entity::EQS::Message> MongoEnsRepository::receiveMessages(const std::string &queueErn, const long maxCount, const long waitTime) {
    //
    //     std::vector<Entity::EQS::Message> result;
    //     const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(waitTime);
    //     const auto weights = Entity::EQS::LoadPriorityWeights();
    //     static constexpr std::array priorityOrder{Entity::EQS::MessagePriority::HIGH, Entity::EQS::MessagePriority::MIDDLE, Entity::EQS::MessagePriority::LOW};
    //
    //     try {
    //         long maxReceiveCount = 0;
    //         std::string deadLetterQueueErn;
    //         {
    //             const auto entry = Database::instance().client();
    //             auto queueCollection = (*entry)[Database::instance().databaseName()][QUEUE_COLLECTION];
    //             if (const auto queueResult = queueCollection.find_one(make_document(kvp("ern", queueErn)))) {
    //                 const auto queue = Entity::EQS::Queue::fromDocument(queueResult->view());
    //                 maxReceiveCount = queue.maxReceiveCount;
    //                 deadLetterQueueErn = queue.deadLetterQueueErn;
    //             }
    //         }
    //
    //         while (true) {
    //             // Acquire a pool entry for this polling attempt only, so the connection is
    //             // not held checked-out for the whole long-poll wait/sleep below.
    //             const auto entry = Database::instance().client();
    //             auto queueCollection = (*entry)[Database::instance().databaseName()][QUEUE_COLLECTION];
    //             auto messageCollection = (*entry)[Database::instance().databaseName()][MESSAGE_COLLECTION];
    //
    //             std::map<Entity::EQS::MessagePriority, long> availableCounts;
    //             for (const auto priority: priorityOrder) {
    //                 availableCounts[priority] = messageCollection.count_documents(make_document(
    //                         kvp("queueErn", queueErn),
    //                         kvp("status", MessageStatusToString(Entity::EQS::MessageStatus::AVAILABLE)),
    //                         kvp("priority", Entity::EQS::MessagePriorityToString(priority))));
    //             }
    //             const auto takeCounts = Entity::EQS::ComputeReceiveCounts(maxCount, availableCounts, weights);
    //
    //             for (const auto priority: priorityOrder) {
    //                 const long target = takeCounts.at(priority);
    //                 long taken = 0;
    //
    //                 while (taken < target && static_cast<long>(result.size()) < maxCount) {
    //                     const auto receiptHandle = Core::UuidUtils::CreateRandomUuid();
    //                     const auto filter = make_document(
    //                             kvp("queueErn", queueErn),
    //                             kvp("status", MessageStatusToString(Entity::EQS::MessageStatus::AVAILABLE)),
    //                             kvp("priority", Entity::EQS::MessagePriorityToString(priority)));
    //                     const auto update = make_document(
    //                             kvp("$set", make_document(
    //                                         kvp("status", MessageStatusToString(Entity::EQS::MessageStatus::INVISIBLE)),
    //                                         kvp("receiptHandle", receiptHandle))),
    //                             kvp("$inc", make_document(
    //                                         kvp("receivedCount", 1))),
    //                             kvp("$currentDate", make_document(
    //                                         kvp("modified", true),
    //                                         kvp("lastReceived", true)
    //                                         )));
    //
    //                     mongocxx::options::find_one_and_update opts;
    //                     opts.return_document(mongocxx::options::return_document::k_after);
    //
    //                     const auto claimed = messageCollection.find_one_and_update(filter.view(), update.view(), opts);
    //                     if (!claimed) break;
    //
    //                     Entity::EQS::Message message;
    //                     message.FromDocument(claimed->view());
    //
    //                     // Move to dead letter queue if existing
    //                     if (!deadLetterQueueErn.empty() && message.receivedCount > maxReceiveCount) {
    //                         const auto moveUpdate = make_document(
    //                                 kvp("$set", make_document(
    //                                             kvp("queueErn", deadLetterQueueErn),
    //                                             kvp("status", MessageStatusToString(Entity::EQS::MessageStatus::AVAILABLE)),
    //                                             kvp("receivedCount", 0),
    //                                             kvp("receiptHandle", ""))),
    //                                 kvp("$currentDate", make_document(
    //                                             kvp("modified", true))));
    //                         messageCollection.update_one(make_document(kvp("messageId", message.messageId)).view(), moveUpdate.view());
    //
    //                         const auto sourceUpdate = make_document(
    //                                 kvp("$inc", make_document(
    //                                             kvp("size", static_cast<int64_t>(-message.size)),
    //                                             kvp("available", static_cast<int64_t>(-1)))),
    //                                 kvp("$currentDate", make_document(
    //                                             kvp("modified", true))));
    //                         queueCollection.update_one(make_document(kvp("ern", queueErn)).view(), sourceUpdate.view());
    //
    //                         const auto targetUpdate = make_document(
    //                                 kvp("$inc", make_document(
    //                                             kvp("size", static_cast<int64_t>(message.size)),
    //                                             kvp("available", static_cast<int64_t>(1)))),
    //                                 kvp("$currentDate", make_document(
    //                                             kvp("modified", true))));
    //                         queueCollection.update_one(make_document(kvp("ern", deadLetterQueueErn)).view(), targetUpdate.view());
    //
    //                         log_info << "Message moved to dead letter queue, ern: " << queueErn << ", dlqErn: " << deadLetterQueueErn << ", messageId: " << message.messageId;
    //                         continue;
    //                     }
    //
    //                     result.push_back(message);
    //                     taken += 1;
    //                 }
    //             }
    //
    //             // Update queue counters
    //             if (!result.empty()) {
    //                 const auto queueFilter = make_document(kvp("ern", queueErn));
    //                 const auto queueUpdate = make_document(
    //                         kvp("$inc", make_document(
    //                                     kvp("invisible", static_cast<int64_t>(result.size())),
    //                                     kvp("available", -static_cast<int64_t>(result.size())))),
    //                         kvp("$currentDate", make_document(
    //                                     kvp("modified", true))));
    //                 queueCollection.update_one(queueFilter.view(), queueUpdate.view());
    //                 log_debug << "Messages received, ern: " << queueErn << ", count: " << result.size();
    //                 return result;
    //             }
    //
    //             if (waitTime <= 0 || std::chrono::steady_clock::now() >= deadline) {
    //                 return result;
    //             }
    //             std::this_thread::sleep_for(std::chrono::milliseconds(100));
    //         }
    //     } catch (const std::exception &e) {
    //         log_error << "Receive messages failed, ern: " << queueErn << ", error: " << e.what();
    //     }
    //     return result;
    // }
    //
    // void MongoEnsRepository::deleteMessage(const std::string &receiptHandle) {
    //
    //     try {
    //         const auto filter = make_document(
    //                 kvp("receiptHandle", receiptHandle));
    //
    //         const auto entry = Database::instance().client();
    //         auto queueCollection = (*entry)[Database::instance().databaseName()][QUEUE_COLLECTION];
    //         auto messageCollection = (*entry)[Database::instance().databaseName()][MESSAGE_COLLECTION];
    //
    //         Entity::EQS::Message message;
    //         if (auto mResult = messageCollection.find_one(filter.view())) {
    //             message.FromDocument(mResult->view());
    //         }
    //
    //         const auto result = messageCollection.delete_many(filter.view());
    //         log_debug << "Message deleted, count: " << result->deleted_count();
    //
    //         if (result && result->deleted_count() > 0 && !message.queueErn.empty()) {
    //             const auto queueFilter = make_document(kvp("ern", message.queueErn));
    //             const auto update = make_document(
    //                     kvp("$inc", make_document(
    //                                 kvp("size", static_cast<int64_t>(-message.size)),
    //                                 kvp("available", static_cast<int64_t>(message.status == Entity::EQS::MessageStatus::AVAILABLE ? -1 : 0)),
    //                                 kvp("delayed", static_cast<int64_t>(message.status == Entity::EQS::MessageStatus::DELAYED ? -1 : 0)),
    //                                 kvp("invisible", static_cast<int64_t>(message.status == Entity::EQS::MessageStatus::INVISIBLE ? -1 : 0)))),
    //                     kvp("$currentDate", make_document(
    //                                 kvp("modified", true))));
    //             queueCollection.update_one(queueFilter.view(), update.view());
    //         }
    //     } catch (const std::exception &e) {
    //         log_error << "Delete message failed, error: " << e.what();
    //     }
    // }

    void MongoEnsRepository::purgeTopic(const std::string &topicErn) {

        try {
            const auto filter = make_document(kvp("topicErn", topicErn));

            const auto entry = Database::instance().client();
            auto queueCollection = (*entry)[Database::instance().databaseName()][TOPIC_COLLECTION];
            auto messageCollection = (*entry)[Database::instance().databaseName()][MESSAGE_COLLECTION];

            const auto result = messageCollection.delete_many(filter.view());
            log_debug << "Topic purged, ern: " << topicErn << ", count: " << result->deleted_count();

            const auto topicFilter = make_document(kvp("ern", topicErn));
            const auto update = make_document(
                    kvp("$set", make_document(
                                kvp("size", static_cast<int64_t>(0)),
                                kvp("available", static_cast<int64_t>(0)))),
                    kvp("$currentDate", make_document(
                                kvp("modified", true))));
            queueCollection.update_one(topicFilter.view(), update.view());
        } catch (const std::exception &e) {
            log_error << "Purge topic failed, ern: " << topicErn << ", error: " << e.what();
        }
    }

    void MongoEnsRepository::purgeAllTopics(const std::string &region, const std::string &accountId, const std::string &nameSpace) {

        try {
            const std::string marker = nameSpace.empty() ? ":" + region + ":" + accountId + ":" : ":" + region + ":" + accountId + ":" + nameSpace + ":";

            const auto entry = Database::instance().client();
            auto topicCollection = (*entry)[Database::instance().databaseName()][TOPIC_COLLECTION];
            auto messageCollection = (*entry)[Database::instance().databaseName()][MESSAGE_COLLECTION];

            array ernArray;
            long topicCount = 0;
            for (auto queueCursor = topicCollection.find({}); auto queue: queueCursor) {
                if (const auto entity = Entity::ENS::Topic::fromDocument(queue); entity.ern.find(marker) != std::string::npos) {
                    ernArray.append(entity.ern);
                    ++topicCount;
                }
            }

            if (topicCount == 0) {
                log_debug << "No topics found, region: " << region << ", accountId: " << accountId;
                return;
            }

            const auto queueFilter = make_document(kvp("ern", make_document(kvp("$in", ernArray.view()))));

            const auto messageResult = messageCollection.delete_many(make_document(kvp("queueErn", make_document(kvp("$in", ernArray.view())))));
            log_debug << "Topics purged, region: " << region << ", accountId: " << accountId << ", topicCount: " << topicCount << ", messageCount: " << messageResult->deleted_count();

            const auto update = make_document(
                    kvp("$set", make_document(
                                kvp("size", static_cast<int64_t>(0)),
                                kvp("available", static_cast<int64_t>(0)))),
                    kvp("$currentDate", make_document(
                                kvp("modified", true))));
            topicCollection.update_many(queueFilter.view(), update.view());
        } catch
        (
            const std::exception &e
        ) {
            log_error << "Purge all topics failed, region: " << region << ", accountId: " << accountId << ", error: " << e.what();
        }
    }

    long MongoEnsRepository::countMessages() const {

        try {
            const auto entry = Database::instance().client();
            auto messageCollection = (*entry)[Database::instance().databaseName()][MESSAGE_COLLECTION];

            return messageCollection.count_documents({});
        } catch (const std::exception &e) {
            log_error << "Count messages failed, error: " << e.what();
        }
        return -1;
    }

    long MongoEnsRepository::countMessages(const std::string &topicErn) const {

        try {
            const auto filter = make_document(kvp("topicErn", topicErn));
            const auto entry = Database::instance().client();
            auto messageCollection = (*entry)[Database::instance().databaseName()][MESSAGE_COLLECTION];

            return messageCollection.count_documents(filter.view());
        } catch (const std::exception &e) {
            log_error << "Count messages failed, ern: " << topicErn << ", error: " << e.what();
        }
        return -1;
    }

    //
    // void MongoEnsRepository::clearMessages() {
    //
    //     try {
    //         const auto entry = Database::instance().client();
    //         auto messageCollection = (*entry)[Database::instance().databaseName()][MESSAGE_COLLECTION];
    //
    //         const auto result = messageCollection.delete_many({});
    //         log_debug << "All messages deleted, count: " << result->deleted_count();
    //     } catch (const std::exception &e) {
    //         log_error << "Delete all messages failed, error: " << e.what();
    //     }
    // }
    //
    // long MongoEnsRepository::resetExpiredMessages() {
    //
    //     long resetCount = 0;
    //     try {
    //         const auto now = std::chrono::system_clock::now();
    //         std::map<std::string, long> resetCountByQueue;
    //         std::map<std::string, long> delayedResetCountByQueue;
    //
    //         const auto entry = Database::instance().client();
    //         auto queueCollection = (*entry)[Database::instance().databaseName()][QUEUE_COLLECTION];
    //         auto messageCollection = (*entry)[Database::instance().databaseName()][MESSAGE_COLLECTION];
    //
    //         const auto filter = make_document(kvp("status", MessageStatusToString(Entity::EQS::MessageStatus::INVISIBLE)));
    //         for (auto cursor = messageCollection.find(filter.view()); auto doc: cursor) {
    //             Entity::EQS::Message message;
    //             message.FromDocument(doc);
    //             if (now < message.lastReceived + std::chrono::seconds(message.visibilityTimeout)) continue;
    //
    //             const auto resetFilter = make_document(
    //                     kvp("_id", bsoncxx::oid{message.oid}),
    //                     kvp("status", MessageStatusToString(Entity::EQS::MessageStatus::INVISIBLE)));
    //             const auto update = make_document(
    //                     kvp("$set", make_document(
    //                                 kvp("status", MessageStatusToString(Entity::EQS::MessageStatus::AVAILABLE)),
    //                                 kvp("receiptHandle", ""))),
    //                     kvp("$currentDate", make_document(
    //                                 kvp("modified", true),
    //                                 kvp("lastReceived", true))));
    //             const auto updateResult = messageCollection.update_one(resetFilter.view(), update.view());
    //
    //             // Message was concurrently deleted or already reset since the cursor read it; skip counting.
    //             if (!updateResult || updateResult->modified_count() == 0) continue;
    //
    //             resetCountByQueue[message.queueErn]++;
    //             resetCount++;
    //             log_debug << "Message visibility timeout expired, messageId: " << message.messageId << ", queueErn: " << message.queueErn;
    //         }
    //
    //         const auto delayedFilter = make_document(kvp("status", MessageStatusToString(Entity::EQS::MessageStatus::DELAYED)));
    //         for (auto cursor = messageCollection.find(delayedFilter.view()); auto doc: cursor) {
    //             Entity::EQS::Message message;
    //             message.FromDocument(doc);
    //             if (now < message.delayUntil) continue;
    //
    //             const auto resetFilter = make_document(
    //                     kvp("_id", bsoncxx::oid{message.oid}),
    //                     kvp("status", MessageStatusToString(Entity::EQS::MessageStatus::DELAYED)));
    //             const auto update = make_document(
    //                     kvp("$set", make_document(
    //                                 kvp("status", MessageStatusToString(Entity::EQS::MessageStatus::AVAILABLE)))),
    //                     kvp("$currentDate", make_document(
    //                                 kvp("modified", true))));
    //             const auto updateResult = messageCollection.update_one(resetFilter.view(), update.view());
    //
    //             // Message was concurrently deleted or already reset since the cursor read it; skip counting.
    //             if (!updateResult || updateResult->modified_count() == 0) continue;
    //
    //             delayedResetCountByQueue[message.queueErn]++;
    //             resetCount++;
    //             log_debug << "Message delay expired, messageId: " << message.messageId << ", queueErn: " << message.queueErn;
    //         }
    //
    //         for (const auto &[queueErn, count]: resetCountByQueue) {
    //             const auto queueUpdate = make_document(
    //                     kvp("$inc", make_document(
    //                                 kvp("invisible", -static_cast<int64_t>(count)),
    //                                 kvp("available", static_cast<int64_t>(count)))),
    //                     kvp("$currentDate", make_document(
    //                                 kvp("modified", true))));
    //             queueCollection.update_one(make_document(kvp("ern", queueErn)).view(), queueUpdate.view());
    //         }
    //
    //         for (const auto &[queueErn, count]: delayedResetCountByQueue) {
    //             const auto queueUpdate = make_document(
    //                     kvp("$inc", make_document(
    //                                 kvp("delayed", -static_cast<int64_t>(count)),
    //                                 kvp("available", static_cast<int64_t>(count)))),
    //                     kvp("$currentDate", make_document(
    //                                 kvp("modified", true))));
    //             queueCollection.update_one(make_document(kvp("ern", queueErn)).view(), queueUpdate.view());
    //         }
    //
    //         if (resetCount > 0)
    //             log_info << "Reset expired messages, count: " << resetCount;
    //
    //     } catch (const std::exception &e) {
    //         log_error << "Reset expired messages failed, error: " << e.what();
    //     }
    //     return resetCount;
    // }

}// namespace Euclid::Database