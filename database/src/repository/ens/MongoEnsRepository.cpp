//
// Created by vogje01 on 29/05/2023.
//

// C++ includes
#include <array>
#include <chrono>
#include <map>
#include <thread>

// Euclid includes
#include <euclid/core/Configuration.h>
#include <euclid/core/ContentTypeUtils.h>
#include <euclid/core/CryptoUtils.h>
#include <euclid/core/UuidUtils.h>
#include <euclid/database/repository/ens/MongoEnsRepository.h>

namespace Euclid::Database {

    namespace {

        /**
         * @brief How long a published message lives when its topic has not been given a period of
         * its own.
         *
         * Read per publish rather than once, because Configuration is an in-memory lookup and an
         * operator who changes the setting should not have to restart the module to mean it.
         */
        long defaultRetentionPeriod() {
            const auto configured = Core::Configuration::instance().getOr<int>(
                    "euclid.modules.ens.retention-period", static_cast<int>(Entity::ENS::kDefaultRetentionPeriod));
            return configured > 0 ? configured : Entity::ENS::kDefaultRetentionPeriod;
        }

    }// namespace

    MongoEnsRepository::MongoEnsRepository() {
        ensureIndexes();
    }

    void MongoEnsRepository::ensureIndexes() {

        try {
            auto topicCollection = Database::instance().collection(TOPIC_COLLECTION);

            // Compound on (accountId, namespace, name) rather than name alone - topic names only
            // need to be unique within their own account/namespace, not globally, and the topic's
            // ERN already carries all three (createEnsTopicErn()). The name-only index this
            // replaces meant one account taking the name "orders" stopped every other account and
            // every other namespace from having a topic of that name at all. NOTE: replacing a
            // pre-existing unique index on "name" alone requires dropping that old index first
            // (Mongo won't do this automatically), and will fail if duplicate names already exist
            // across accounts in production data - this is an operational migration step.
            mongocxx::options::index topicNameOpts;
            topicNameOpts.unique(true);
            topicCollection.create_index(make_document(kvp("accountId", 1), kvp("namespace", 1), kvp("name", 1)), topicNameOpts);

            mongocxx::options::index topicErnOpts;
            topicErnOpts.unique(true);
            topicCollection.create_index(make_document(kvp("ern", 1)), topicErnOpts);

            auto subscriptionCollection = Database::instance().collection(SUBSCRIPTION_COLLECTION);
            mongocxx::options::index subscriptionOpts;
            subscriptionOpts.unique(true);
            subscriptionCollection.create_index(make_document(kvp("sourceErn", 1), kvp("type", 1), kvp("targetErn", 1)), subscriptionOpts);
            // Messages were left with only the _id index every collection gets for free, which is
            // to say that every read of one was a scan of all of them. It went unnoticed because
            // the block that should have created them was EQS's, commented out - it named
            // queueErn, receiptHandle and priority, none of which an ENS message has, so it could
            // never have been uncommented as it stood.
            auto messageCollection = Database::instance().collection(MESSAGE_COLLECTION);

            // findMessageById() and upsertMessage() both address a message by this, which is what
            // get-message-attribute and set-message-attribute are built out of - so without it,
            // reading one attribute of one message costs a scan of every message ever published to
            // any topic. On this installation that was 1.2 million documents and 513 ms. Unique
            // because the id identifies the message, which also stops a publish retried after a
            // lost response from storing it a second time.
            mongocxx::options::index messageIdOpts;
            messageIdOpts.unique(true);
            messageCollection.create_index(make_document(kvp("messageId", 1)), messageIdOpts);

            // listMessages() filters on the topic and deleting a topic deletes its messages the
            // same way, so this is what keeps both proportional to one topic's traffic rather than
            // to the collection.
            messageCollection.create_index(make_document(kvp("topicErn", 1)));

            // Retention, enforced by the database. expireAfterSeconds is zero because the moment is
            // already in the document - the field is when the message expires, not when it was
            // published - which is the same shape eqs_message and ees_events use.
            //
            // A TTL index ignores a document that has no such field, so this removes nothing that
            // was published before retention existed. Those topics stay as they are until somebody
            // purges them on purpose; what this stops is the next one filling up the same way.
            mongocxx::options::index expiresAtOpts;
            expiresAtOpts.expire_after(std::chrono::seconds(0));
            messageCollection.create_index(make_document(kvp("expiresAt", 1)), expiresAtOpts);

        } catch (const std::exception &e) {
            log_error << "Ensure ENS indexes failed, error: " << e.what();
        }
    }

    bool MongoEnsRepository::topicExists(const std::string &accountId, const std::string &nameSpace, const std::string &name) const {

        try {

            // All three fields, in the order the unique index names them, so the answer is about
            // the caller's own topic rather than about anyone in the installation holding the name.
            const auto query = make_document(kvp("accountId", accountId), kvp("namespace", nameSpace), kvp("name", name));

            auto topicCollection = Database::instance().collection(TOPIC_COLLECTION);

            const auto result = topicCollection.find_one(query.view());
            log_trace << "Topic exists, accountId: " << accountId << ", namespace: " << nameSpace << ", name: " << name << ", exists: " << std::boolalpha << result.has_value();
            return result.has_value();

        } catch (const std::exception &e) {
            log_error << "Topic exists failed, name: " << name << ", error: " << e.what();
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
    //    //         auto queueCollection = Database::instance().collection(QUEUE_COLLECTION);
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

    std::optional<Entity::ENS::Topic> MongoEnsRepository::findTopicByName(const std::string &accountId, const std::string &nameSpace, const std::string &name) const {

        try {

            auto topicCollection = Database::instance().collection(TOPIC_COLLECTION);

            // A name identifies a topic only together with the account and namespace that own it,
            // so resolving one without them would hand a caller somebody else's topic - which is
            // also how it would have read its messages, since everything downstream works off the
            // ERN this returns.
            const auto filter = make_document(kvp("accountId", accountId), kvp("namespace", nameSpace), kvp("name", name));
            if (auto mResult = topicCollection.find_one(filter.view())) {
                return Entity::ENS::Topic::fromDocument(mResult.value());
            }

        } catch (const std::exception &e) {
            log_error << "Get topic by name failed, name: " << name << " error: " << e.what();
        }
        return {};
    }

    std::optional<Entity::ENS::Topic> MongoEnsRepository::findTopicByErn(const std::string &ern) const {

        try {

            auto topicCollection = Database::instance().collection(TOPIC_COLLECTION);

            if (auto mResult = topicCollection.find_one(make_document(kvp("ern", ern)))) {
                return Entity::ENS::Topic::fromDocument(mResult.value());
            }

        } catch (const std::exception &e) {
            log_error << "Get topic by ERN failed, ern: " << ern << " error: " << e.what();
        }
        return {};
    }

    std::vector<Entity::ENS::Topic> MongoEnsRepository::listTopics(const std::string &accountId, const std::string &namespaceName, const std::string &prefix, const long pageSize, const long pageIndex, const std::string &sortColumn, const std::string &sortDirection) const {

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
                opts.sort(make_document(kvp(sortColumn, sortDirection == "asc" ? 1 : -1)));
            }
            if (pageSize > 0) {
                opts.limit(pageSize);
                opts.skip(std::max<long>(pageIndex, 0) * pageSize);
            }

            std::vector<Entity::ENS::Topic> topics;
            auto queueCollection = Database::instance().collection(TOPIC_COLLECTION);

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

            // Matches the unique index: an upsert filtered on the name alone would find another
            // account's topic of that name and overwrite it, and on the insert path it would race
            // the index into a duplicate-key error instead of creating the caller's own topic.
            const auto filter = make_document(kvp("accountId", topic.accountId), kvp("namespace", topic.nameSpace), kvp("name", topic.name));
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

            auto queueCollection = Database::instance().collection(TOPIC_COLLECTION);

            if (auto result = queueCollection.find_one_and_update(filter.view(), update.view(), opts)) {
                return Entity::ENS::Topic::fromDocument(result->view());
            }

        } catch (const std::exception &e) {
            log_error << "Upsert ENS topic failed, error: " << e.what();
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

            auto subscriptionCollection = Database::instance().collection(SUBSCRIPTION_COLLECTION);

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

            auto subscriptionCollection = Database::instance().collection(SUBSCRIPTION_COLLECTION);

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
            auto subscriptionCollection = Database::instance().collection(SUBSCRIPTION_COLLECTION);

            const auto result = subscriptionCollection.delete_many(make_document(kvp("ern", ern)));
            log_debug << "Subscription deleted, ern: " << ern << ", count: " << result->deleted_count();

        } catch (const std::exception &e) {
            log_error << "Delete subscription failed, ern: " << ern << ", error: " << e.what();
        }
    }

    long MongoEnsRepository::countTopics(const std::string &accountId, const std::string &namespaceName, const std::string &prefix) const {

        try {

            document filter = {};
            filter.append(kvp("accountId", accountId));
            if (!namespaceName.empty()) {
                filter.append(kvp("namespace", namespaceName));
            }
            if (!prefix.empty()) {
                filter.append(kvp("name", make_document(kvp("$regex", "^" + prefix))));
            }

            auto queueCollection = Database::instance().collection(TOPIC_COLLECTION);

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
    //    //         auto queueCollection = Database::instance().collection(QUEUE_COLLECTION);
    //         auto messageCollection = Database::instance().collection(MESSAGE_COLLECTION);
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
            auto topicCollection = Database::instance().collection(TOPIC_COLLECTION);
            auto messageCollection = Database::instance().collection(MESSAGE_COLLECTION);

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
    //    //         auto queueCollection = Database::instance().collection(QUEUE_COLLECTION);
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
    //    //         auto messageCollection = Database::instance().collection(MESSAGE_COLLECTION);
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
            auto messageCollection = Database::instance().collection(MESSAGE_COLLECTION);

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
    //    //         auto messageCollection = Database::instance().collection(MESSAGE_COLLECTION);
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
    //    //         auto messageCollection = Database::instance().collection(MESSAGE_COLLECTION);
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

    std::vector<Entity::ENS::Message> MongoEnsRepository::listMessages(const std::string &topicErn, const long pageSize, const long pageIndex, const std::string &sortColumn, const std::string &sortDirection) const {

        std::vector<Entity::ENS::Message> messages;
        try {
            const auto filter = make_document(kvp("topicErn", topicErn));

            mongocxx::options::find opts;
            if (!sortColumn.empty()) {
                opts.sort(make_document(kvp(sortColumn, sortDirection == "asc" ? 1 : -1)));
            }
            if (pageSize > 0) {
                opts.limit(pageSize);
                opts.skip(std::max<long>(pageIndex, 0) * pageSize);
            }

            auto messageCollection = Database::instance().collection(MESSAGE_COLLECTION);

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

            auto messageCollection = Database::instance().collection(MESSAGE_COLLECTION);

            messageCollection.update_one(filter.view(), update.view(), opts);

        } catch (const std::exception &e) {
            log_error << "Upsert message failed, error: " << e.what();
        }
    }

    std::vector<Entity::ENS::Message> MongoEnsRepository::listHeldMessages(const std::string &topicErn, const long limit) const {

        std::vector<Entity::ENS::Message> messages;
        try {

            mongocxx::options::find opts;
            opts.sort(make_document(kvp("created", 1)));
            if (limit > 0) opts.limit(limit);

            const auto messageCollection = Database::instance().collection(MESSAGE_COLLECTION);
            const auto filter = make_document(kvp("topicErn", topicErn), kvp("status", Entity::ENS::kStatusHeld));

            for (auto cursor = messageCollection.find(filter.view(), opts); const auto &document: cursor) {
                Entity::ENS::Message message;
                message.FromDocument(document);
                messages.push_back(message);
            }

        } catch (const std::exception &e) {
            log_error << "List held messages failed, topicErn: " << topicErn << ", error: " << e.what();
            throw;
        }
        return messages;
    }

    long MongoEnsRepository::countHeldMessages(const std::string &topicErn) const {

        try {
            const auto messageCollection = Database::instance().collection(MESSAGE_COLLECTION);
            return static_cast<long>(messageCollection.count_documents(
                    make_document(kvp("topicErn", topicErn), kvp("status", Entity::ENS::kStatusHeld)).view()));

        } catch (const std::exception &e) {
            log_error << "Count held messages failed, topicErn: " << topicErn << ", error: " << e.what();
        }
        return 0;
    }

    void MongoEnsRepository::markMessageDelivered(const std::string &messageId) {

        try {
            const auto messageCollection = Database::instance().collection(MESSAGE_COLLECTION);
            const auto update = make_document(
                    kvp("$set", make_document(kvp("status", Entity::ENS::kStatusPublished))),
                    kvp("$currentDate", make_document(kvp("modified", true))));

            messageCollection.update_one(make_document(kvp("messageId", messageId)).view(), update.view());

        } catch (const std::exception &e) {
            log_error << "Mark message delivered failed, messageId: " << messageId << ", error: " << e.what();
            throw;
        }
    }

    void MongoEnsRepository::recordResend(const std::string &topicErn, const long count) {

        if (count <= 0) return;

        try {
            const auto topicCollection = Database::instance().collection(TOPIC_COLLECTION);
            const auto update = make_document(
                    kvp("$inc", make_document(kvp("resend", static_cast<int64_t>(count)))),
                    kvp("$currentDate", make_document(kvp("modified", true))));

            topicCollection.update_one(make_document(kvp("ern", topicErn)).view(), update.view());

        } catch (const std::exception &e) {
            log_error << "Record resend failed, topicErn: " << topicErn << ", error: " << e.what();
        }
    }

    Entity::ENS::Message MongoEnsRepository::publishMessage(const std::string &messageId, const std::string &ern, const std::string &topicErn, const std::string &body, const std::map<std::string, Entity::COM::Variant> &attributes, const std::string &priority) {

        Entity::ENS::Message message;
        message.ern = ern;
        message.topicErn = topicErn;
        message.body = body;
        message.size = static_cast<long>(body.size());
        message.messageId = messageId;
        message.contentType = Core::ContentTypeUtils::fromContent(message.body);
        message.attributes = attributes;
        message.priority = priority;

        // Decided here rather than by the caller, because this is where the topic is read: a
        // message published to a stopped topic is stored held, and the caller learns not to fan it
        // out from the status it gets back.
        message.status = Entity::ENS::kStatusPublished;

        try {

            auto queueCollection = Database::instance().collection(TOPIC_COLLECTION);
            auto messageCollection = Database::instance().collection(MESSAGE_COLLECTION);

            const auto queueFilter = make_document(kvp("ern", topicErn));
            if (auto queueResult = queueCollection.find_one(queueFilter.view())) {
                const auto queue = Entity::ENS::Topic::fromDocument(queueResult->view());

                // The whole of what retention costs a publish: one date on the document, taken from
                // the topic this read has already fetched. A topic that has not been given a period
                // of its own follows the installation's, rather than having frozen a copy of it.
                const auto retention = queue.retentionPeriod > 0 ? queue.retentionPeriod : defaultRetentionPeriod();
                message.expiresAt = std::chrono::system_clock::now() + std::chrono::seconds(retention);

                if (!queue.delivering) message.status = Entity::ENS::kStatusHeld;

                const auto update = make_document(
                        kvp("$inc", make_document(
                                    kvp("size", static_cast<int64_t>(message.size)),
                                    kvp("available", static_cast<int64_t>(1)))),
                        kvp("$currentDate", make_document(
                                    kvp("modified", true))));
                queueCollection.update_one(queueFilter.view(), update.view());
            }

            messageCollection.insert_one(message.ToDocument());
            log_debug << "Message sent, ern: " << ern << ", messageId: " << message.messageId;

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
    //    //             auto queueCollection = Database::instance().collection(QUEUE_COLLECTION);
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
    //    //             auto queueCollection = Database::instance().collection(QUEUE_COLLECTION);
    //             auto messageCollection = Database::instance().collection(MESSAGE_COLLECTION);
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
    //    //         auto queueCollection = Database::instance().collection(QUEUE_COLLECTION);
    //         auto messageCollection = Database::instance().collection(MESSAGE_COLLECTION);
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

            auto queueCollection = Database::instance().collection(TOPIC_COLLECTION);
            auto messageCollection = Database::instance().collection(MESSAGE_COLLECTION);

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
            auto topicCollection = Database::instance().collection(TOPIC_COLLECTION);
            auto messageCollection = Database::instance().collection(MESSAGE_COLLECTION);

            // Filters on the topic's own region/accountId/namespace fields, rather than the
            // previous full scan matching a substring of the ERN: those fields are what the
            // uniqueness of a topic is defined in terms of, and an empty nameSpace means "every
            // namespace of the account" here - which a filter on the field itself cannot say, so
            // it is only appended when one was given.
            document scopeFilter{};
            scopeFilter.append(kvp("region", region), kvp("accountId", accountId));
            if (!nameSpace.empty()) {
                scopeFilter.append(kvp("namespace", nameSpace));
            }

            array ernArray;
            long topicCount = 0;
            for (auto topicCursor = topicCollection.find(scopeFilter.extract()); auto topic: topicCursor) {
                ernArray.append(Entity::ENS::Topic::fromDocument(topic).ern);
                ++topicCount;
            }

            if (topicCount == 0) {
                log_debug << "No topics found, region: " << region << ", accountId: " << accountId;
                return;
            }

            const auto queueFilter = make_document(kvp("ern", make_document(kvp("$in", ernArray.view()))));

            // topicErn, not queueErn: an ENS message has no queueErn at all, so the purge this
            // replaces matched nothing and left every message of every purged topic behind.
            const auto messageResult = messageCollection.delete_many(make_document(kvp("topicErn", make_document(kvp("$in", ernArray.view())))));
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
            auto messageCollection = Database::instance().collection(MESSAGE_COLLECTION);

            return messageCollection.count_documents({});
        } catch (const std::exception &e) {
            log_error << "Count messages failed, error: " << e.what();
        }
        return -1;
    }

    long MongoEnsRepository::countMessages(const std::string &topicErn) const {

        try {
            const auto filter = make_document(kvp("topicErn", topicErn));
            auto messageCollection = Database::instance().collection(MESSAGE_COLLECTION);

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
    //    //         auto messageCollection = Database::instance().collection(MESSAGE_COLLECTION);
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
    //    //         auto queueCollection = Database::instance().collection(QUEUE_COLLECTION);
    //         auto messageCollection = Database::instance().collection(MESSAGE_COLLECTION);
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


    void MongoEnsRepository::recountTopics() {

        try {
            auto messageCollection = Database::instance().collection(MESSAGE_COLLECTION);
            auto topicCollection = Database::instance().collection(TOPIC_COLLECTION);

            struct Counts {
                long messages{};
                long size{};
            };
            std::unordered_map<std::string, Counts> counted;

            // Grouped, counted and summed through the neutral path, so a recount does not need a
            // server to evaluate a pipeline - see Collection::group_count.
            for (const auto &group: messageCollection.group_count({}, {"topicErn"}, "size")) {
                counted[group.key[0]] = {.messages = group.count, .size = group.sum};
            }

            // "send" and "resend" are deliberately not touched - they are lifetime totals, and
            // recomputing them from the messages still stored would make them fall on every purge.
            long topics = 0;
            for (auto cursor = topicCollection.find({}); auto doc: cursor) {
                const auto ernField = doc["ern"];
                if (!ernField || ernField.type() != bsoncxx::type::k_string) continue;
                const auto ern = std::string(ernField.get_string().value);

                const auto it = counted.find(ern);
                const Counts counts = it != counted.end() ? it->second : Counts{};
                topicCollection.update_one(make_document(kvp("ern", ern)).view(),
                                           make_document(kvp("$set", make_document(
                                                                     kvp("available", static_cast<int64_t>(counts.messages)),
                                                                     kvp("size", static_cast<int64_t>(counts.size)))))
                                           .view());
                ++topics;
            }

            log_debug << "Topic counts recounted, topics: " << topics;

        } catch (const std::exception &e) {
            log_error << "Topic recount failed, error: " << e.what();
        }
    }

}// namespace Euclid::Database