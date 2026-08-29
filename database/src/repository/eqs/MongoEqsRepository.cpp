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
#include <euclid/database/repository/eqs/MongoEqsRepository.h>

#include <boost/chrono/system_clocks.hpp>

namespace Euclid::Database {

    MongoEqsRepository::MongoEqsRepository() {
        ensureIndexes();
    }

    void MongoEqsRepository::ensureIndexes() {

        try {
            const auto entry = Database::instance().client();
            auto queueCollection = (*entry)[Database::instance().databaseName()][QUEUE_COLLECTION];

            // Compound on (accountId, namespace, name) rather than name alone - queue names only
            // need to be unique within their own account/namespace, not globally. NOTE: replacing
            // a pre-existing unique index on "name" alone requires dropping that old index first
            // (Mongo won't do this automatically), and will fail if duplicate names already exist
            // across accounts in production data - this is an operational migration step.
            mongocxx::options::index queueNameOpts;
            queueNameOpts.unique(true);
            queueCollection.create_index(make_document(kvp("accountId", 1), kvp("namespace", 1), kvp("name", 1)), queueNameOpts);

            mongocxx::options::index queueErnOpts;
            queueErnOpts.unique(true);
            queueCollection.create_index(make_document(kvp("ern", 1)), queueErnOpts);

            auto messageCollection = (*entry)[Database::instance().databaseName()][MESSAGE_COLLECTION];

            messageCollection.create_index(make_document(kvp("queueErn", 1), kvp("status", 1), kvp("priority", 1)));

            // Supports resetExpiredMessages()'s per-status sweeps (INVISIBLE, then DELAYED),
            // which filter by status alone - the compound index above can't serve that, since
            // queueErn is its leading field and isn't part of that filter.
            messageCollection.create_index(make_document(kvp("status", 1)));

            mongocxx::options::index receiptHandleOpts;
            receiptHandleOpts.sparse(true);
            messageCollection.create_index(make_document(kvp("receiptHandle", 1)), receiptHandleOpts);

            mongocxx::options::index messageIdOpts;
            messageIdOpts.unique(true);
            messageCollection.create_index(make_document(kvp("messageId", 1)), messageIdOpts);

        } catch (const std::exception &e) {
            log_error << "Ensure SQS indexes failed, error: " << e.what();
        }
    }

    bool MongoEqsRepository::queueExists(const std::string &name) const {

        try {

            document query{};
            if (!name.empty()) {
                query.append(kvp("name", name));
            }

            const auto entry = Database::instance().client();
            auto queueCollection = (*entry)[Database::instance().databaseName()][QUEUE_COLLECTION];

            const auto result = queueCollection.find_one(query.extract());
            log_trace << "Sqs exists, name: " << name << ", exists: " << std::boolalpha << result.has_value();
            return result.has_value();

        } catch (const std::exception &e) {
            log_error << "Sqs exists failed, name: " << ", error: " << e.what();
        }
        return false;
    }

    std::optional<Entity::EQS::Queue> MongoEqsRepository::findQueueById(const std::string &oid) const {

        try {

            document document;
            document.append(kvp("_id", oid));

            const auto entry = Database::instance().client();
            auto queueCollection = (*entry)[Database::instance().databaseName()][QUEUE_COLLECTION];

            if (auto mResult = queueCollection.find_one(document.view())) {
                return Entity::EQS::Queue::fromDocument(mResult->view());
            }

        } catch (const std::exception &e) {
            log_error << "Get queues by ID failed, error: " << e.what();
        }
        return {};
    }

    std::optional<Entity::EQS::Queue> MongoEqsRepository::findQueueByName(const std::string &name) const {

        try {

            const auto entry = Database::instance().client();
            auto queueCollection = (*entry)[Database::instance().databaseName()][QUEUE_COLLECTION];

            if (auto mResult = queueCollection.find_one(make_document(kvp("name", name)))) {
                return Entity::EQS::Queue::fromDocument(mResult.value());
            }

        } catch (const std::exception &e) {
            log_error << "Get queues by name failed, name: " << name << " error: " << e.what();
        }
        return {};
    }

    std::optional<Entity::EQS::Queue> MongoEqsRepository::findQueueByErn(const std::string &ern) const {

        try {

            const auto entry = Database::instance().client();
            auto queueCollection = (*entry)[Database::instance().databaseName()][QUEUE_COLLECTION];

            if (auto mResult = queueCollection.find_one(make_document(kvp("ern", ern)))) {
                return Entity::EQS::Queue::fromDocument(mResult.value());
            }

        } catch (const std::exception &e) {
            log_error << "Get queues by ERN failed, ern: " << ern << " error: " << e.what();
        }
        return {};
    }

    std::vector<Entity::EQS::Queue> MongoEqsRepository::listQueues(const std::string &accountId, const std::string &namespaceName, const std::string &prefix, const long pageSize, const long pageIndex, const std::string &sortColumn, const std::string &sortDirection) const {

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

            std::vector<Entity::EQS::Queue> queues;
            const auto entry = Database::instance().client();
            auto queueCollection = (*entry)[Database::instance().databaseName()][QUEUE_COLLECTION];

            for (auto queueCursor = queueCollection.find(filter.view(), opts); auto queue: queueCursor) {
                queues.push_back(Entity::EQS::Queue::fromDocument(queue));
            }
            return queues;

        } catch (const std::exception &e) {

            log_error << "Get EQS queues failed, error: " << e.what();
            return {};
        }
    }

    Entity::EQS::Queue MongoEqsRepository::upsertQueue(Entity::EQS::Queue &queue) {

        try {

            const auto filter = make_document(kvp("accountId", queue.accountId), kvp("namespace", queue.nameSpace), kvp("name", queue.name));
            const auto update = make_document(
                    kvp("$set", queue.toDocument()),
                    kvp("$setOnInsert", make_document(
                                kvp("created", bsoncxx::types::b_date{
                                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                                    queue.created.time_since_epoch())
                                    })
                                )),
                    kvp("$currentDate", make_document(
                                kvp("modified", true)
                                )));

            mongocxx::options::find_one_and_update opts;
            opts.upsert(true);
            opts.return_document(mongocxx::options::return_document::k_after);

            const auto entry = Database::instance().client();
            auto queueCollection = (*entry)[Database::instance().databaseName()][QUEUE_COLLECTION];

            if (auto result = queueCollection.find_one_and_update(filter.view(), update.view(), opts)) {
                return Entity::EQS::Queue::fromDocument(result->view());
            }
            throw std::runtime_error("upsert returned no document, name: " + queue.name);

        } catch (const std::exception &e) {
            log_error << "Upsert EQS queue failed, error: " << e.what();
            throw;
        }
    }

    long MongoEqsRepository::countQueues(const std::string &accountId, const std::string &namespaceName, const std::string &prefix) const {

        try {

            document filter = {};
            filter.append(kvp("accountId", accountId));
            if (!namespaceName.empty()) {
                filter.append(kvp("namespace", namespaceName));
            }
            if (!prefix.empty()) {
                filter.append(kvp("name", make_document(kvp("$regex", "^" + prefix))));
            }

            const auto entry = Database::instance().client();
            auto queueCollection = (*entry)[Database::instance().databaseName()][QUEUE_COLLECTION];

            const int64_t count = queueCollection.count_documents(filter.extract());
            log_trace << "Service state: " << std::boolalpha << count;
            return static_cast<int>(count);

        } catch (const std::exception &e) {

            log_error << "Service exists failed, error: " << e.what();
        }
        return -1;
    }

    void MongoEqsRepository::removeQueueByName(const std::string &name) {

        try {
            const auto entry = Database::instance().client();
            auto queueCollection = (*entry)[Database::instance().databaseName()][QUEUE_COLLECTION];
            auto messageCollection = (*entry)[Database::instance().databaseName()][MESSAGE_COLLECTION];

            std::vector<std::string> erns;
            for (auto cursor = queueCollection.find(make_document(kvp("name", name))); auto doc: cursor) {
                if (const auto ernField = doc["ern"]; ernField && ernField.type() == bsoncxx::type::k_string) {
                    erns.emplace_back(ernField.get_string().value);
                }
            }

            const auto result = queueCollection.delete_many(make_document(kvp("name", name)));
            log_debug << "EQS deleted, count: " << result->deleted_count();

            if (!erns.empty()) {
                array ernArray;
                for (const auto &ern: erns) ernArray.append(ern);
                const auto messageResult = messageCollection.delete_many(make_document(kvp("queueErn", make_document(kvp("$in", ernArray.view())))));
                log_debug << "EQS messages deleted, count: " << messageResult->deleted_count();
            }

        } catch (const std::exception &e) {

            log_error << "Delete queues failed, error: " << e.what();
        }

    }

    void MongoEqsRepository::deleteQueueByErn(const std::string &ern) {

        try {
            const auto entry = Database::instance().client();
            auto queueCollection = (*entry)[Database::instance().databaseName()][QUEUE_COLLECTION];
            auto messageCollection = (*entry)[Database::instance().databaseName()][MESSAGE_COLLECTION];

            const auto result = queueCollection.delete_many(make_document(kvp("ern", ern)));
            log_debug << "EQS deleted, count: " << result->deleted_count();

            const auto messageResult = messageCollection.delete_many(make_document(kvp("queueErn", ern)));
            log_debug << "EQS messages deleted, count: " << messageResult->deleted_count();

        } catch (const std::exception &e) {

            log_error << "Delete queues failed, error: " << e.what();
        }

    }

    void MongoEqsRepository::clearQueues() {

        try {
            const auto entry = Database::instance().client();
            auto queueCollection = (*entry)[Database::instance().databaseName()][QUEUE_COLLECTION];

            const auto result = queueCollection.delete_many({});
            log_debug << "All queues deleted, count: " << result->deleted_count();

        } catch (const std::exception &e) {
            log_error << "Delete all queues queues failed, error: " << e.what();
        }
    }

    bool MongoEqsRepository::messageExists(const std::string &messageId) const {

        try {
            const auto query = make_document(
                    kvp("messageId", messageId));
            const auto entry = Database::instance().client();
            auto messageCollection = (*entry)[Database::instance().databaseName()][MESSAGE_COLLECTION];

            const auto result = messageCollection.find_one(query.view());
            return result.has_value();
        } catch (const std::exception &e) {
            log_error << "Message exists failed, messageId: " << messageId << ", error: " << e.what();
        }
        return false;
    }

    std::optional<Entity::EQS::Message> MongoEqsRepository::findMessageById(const std::string &oid) const {

        try {
            const auto query = make_document(
                    kvp("_id", oid));
            const auto entry = Database::instance().client();
            auto messageCollection = (*entry)[Database::instance().databaseName()][MESSAGE_COLLECTION];

            if (auto mResult = messageCollection.find_one(query.view())) {
                Entity::EQS::Message message;
                message.FromDocument(mResult->view());
                return message;
            }
        } catch (const std::exception &e) {
            log_error << "Get message by ID failed, error: " << e.what();
        }
        return {};
    }

    std::optional<Entity::EQS::Message> MongoEqsRepository::findMessageByName(const std::string &messageId) const {

        try {
            const auto query = make_document(
                    kvp("messageId", messageId));
            const auto entry = Database::instance().client();
            auto messageCollection = (*entry)[Database::instance().databaseName()][MESSAGE_COLLECTION];

            if (auto mResult = messageCollection.find_one(query.view())) {
                Entity::EQS::Message message;
                message.FromDocument(mResult->view());
                return message;
            }
        } catch (const std::exception &e) {
            log_error << "Get message by messageId failed, error: " << e.what();
        }
        return {};
    }

    std::vector<Entity::EQS::Message> MongoEqsRepository::findAllMessages() const {

        try {
            std::vector<Entity::EQS::Message> messages;
            const auto entry = Database::instance().client();
            auto messageCollection = (*entry)[Database::instance().databaseName()][MESSAGE_COLLECTION];

            for (auto cursor = messageCollection.find({}); auto doc: cursor) {
                Entity::EQS::Message message;
                message.FromDocument(doc);
                messages.push_back(std::move(message));
            }
            return messages;
        } catch (const std::exception &e) {
            log_error << "Get all messages failed, error: " << e.what();
        }
        return {};
    }

    std::vector<Entity::EQS::Message> MongoEqsRepository::listMessages(const std::string &queueErn, const long pageSize, const long pageIndex, const std::string &sortColumn, const std::string &sortDirection) const {

        std::vector<Entity::EQS::Message> messages;
        try {
            const auto filter = make_document(kvp("queueErn", queueErn));

            mongocxx::options::find opts;
            if (!sortColumn.empty()) {
                opts.sort(make_document(kvp(sortColumn, sortDirection == "asc" ? 1 : -1)));
            }
            if (pageSize > 0) {
                opts.limit(pageSize);
                opts.skip(std::max<long>(pageIndex, 0) * pageSize);
            }

            const auto entry = Database::instance().client();
            auto messageCollection = (*entry)[Database::instance().databaseName()][MESSAGE_COLLECTION];

            for (auto cursor = messageCollection.find(filter.view(), opts); auto doc: cursor) {
                Entity::EQS::Message message;
                message.FromDocument(doc);
                messages.push_back(std::move(message));
            }

        } catch (const std::exception &e) {
            log_error << "List messages failed, queueErn: " << queueErn << ", error: " << e.what();
        }
        return messages;
    }

    void MongoEqsRepository::upsertMessage(const Entity::EQS::Message &message) {

        try {
            const auto filter = make_document(kvp("messageId", message.messageId));
            const auto update = make_document(
                    kvp("$set", message.ToDocument()),
                    kvp("$setOnInsert", make_document(
                                kvp("created", bsoncxx::types::b_date{
                                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                                    message.created.time_since_epoch())
                                    }))),
                    kvp("$currentDate", make_document(kvp("modified", true))));

            mongocxx::options::update opts;
            opts.upsert(true);

            const auto entry = Database::instance().client();
            auto messageCollection = (*entry)[Database::instance().databaseName()][MESSAGE_COLLECTION];

            messageCollection.update_one(filter.view(), update.view(), opts);

        } catch (const std::exception &e) {
            log_error << "Upsert message failed, error: " << e.what();
        }
    }

    Entity::EQS::Message MongoEqsRepository::sendMessage(const std::string &messageId, const std::string &ern, const std::string &queueErn, const std::string &body, const std::map<std::string, Entity::COM::Variant> &attributes, const Entity::EQS::MessagePriority priority) {

        Entity::EQS::Message message;
        message.ern = ern;
        message.queueErn = queueErn;
        message.body = body;
        message.size = static_cast<long>(body.size());
        message.messageId = messageId;
        message.md5Body = Core::CryptoUtils::md5Sum(message.body);
        message.contentType = Core::ContentTypeUtils::fromContent(message.body);
        message.attributes = attributes;
        message.md5Attributes = Entity::EQS::Message::ComputeAttributesMd5(attributes);
        message.status = Entity::EQS::MessageStatus::AVAILABLE;
        message.priority = priority;
        message.created = std::chrono::system_clock::now();
        message.modified = std::chrono::system_clock::now();

        try {

            const auto entry = Database::instance().client();
            auto queueCollection = (*entry)[Database::instance().databaseName()][QUEUE_COLLECTION];
            auto messageCollection = (*entry)[Database::instance().databaseName()][MESSAGE_COLLECTION];

            const auto queueFilter = make_document(kvp("ern", queueErn));
            if (auto queueResult = queueCollection.find_one(queueFilter.view())) {
                const auto queue = Entity::EQS::Queue::fromDocument(queueResult->view());
                message.visibilityTimeout = queue.visibility;

                if (queue.delay > 0) {
                    message.status = Entity::EQS::MessageStatus::DELAYED;
                    message.delayUntil = std::chrono::system_clock::now() + std::chrono::seconds(queue.delay);
                }

                const auto update = make_document(
                        kvp("$inc", make_document(
                                    kvp("size", static_cast<int64_t>(message.size)),
                                    kvp("available", static_cast<int64_t>(queue.delay > 0 ? 0 : 1)),
                                    kvp("delayed", static_cast<int64_t>(queue.delay > 0 ? 1 : 0)))),
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

    std::vector<Entity::EQS::Message> MongoEqsRepository::receiveMessages(const std::string &queueErn, const long maxCount, const long waitTime) {

        std::vector<Entity::EQS::Message> result;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(waitTime);
        const auto weights = Entity::EQS::LoadPriorityWeights();
        static constexpr std::array priorityOrder{Entity::EQS::MessagePriority::HIGH, Entity::EQS::MessagePriority::MIDDLE, Entity::EQS::MessagePriority::LOW};

        try {
            long maxReceiveCount = 0;
            std::string deadLetterQueueErn;
            {
                const auto entry = Database::instance().client();
                auto queueCollection = (*entry)[Database::instance().databaseName()][QUEUE_COLLECTION];
                if (const auto queueResult = queueCollection.find_one(make_document(kvp("ern", queueErn)))) {
                    const auto queue = Entity::EQS::Queue::fromDocument(queueResult->view());
                    maxReceiveCount = queue.maxReceiveCount;
                    deadLetterQueueErn = queue.deadLetterQueueErn;
                }
            }

            while (true) {
                // Acquire a pool entry for this polling attempt only, so the connection is
                // not held checked-out for the whole long-poll wait/sleep below.
                const auto entry = Database::instance().client();
                auto queueCollection = (*entry)[Database::instance().databaseName()][QUEUE_COLLECTION];
                auto messageCollection = (*entry)[Database::instance().databaseName()][MESSAGE_COLLECTION];

                std::map<Entity::EQS::MessagePriority, long> availableCounts;
                for (const auto priority: priorityOrder) {
                    availableCounts[priority] = messageCollection.count_documents(make_document(
                            kvp("queueErn", queueErn),
                            kvp("status", MessageStatusToString(Entity::EQS::MessageStatus::AVAILABLE)),
                            kvp("priority", Entity::EQS::MessagePriorityToString(priority))));
                }
                const auto takeCounts = Entity::EQS::ComputeReceiveCounts(maxCount, availableCounts, weights);

                for (const auto priority: priorityOrder) {
                    const long target = takeCounts.at(priority);
                    long taken = 0;

                    while (taken < target && static_cast<long>(result.size()) < maxCount) {
                        const auto receiptHandle = Core::UuidUtils::CreateRandomUuid();
                        const auto filter = make_document(
                                kvp("queueErn", queueErn),
                                kvp("status", MessageStatusToString(Entity::EQS::MessageStatus::AVAILABLE)),
                                kvp("priority", Entity::EQS::MessagePriorityToString(priority)));
                        const auto update = make_document(
                                kvp("$set", make_document(
                                            kvp("status", MessageStatusToString(Entity::EQS::MessageStatus::INVISIBLE)),
                                            kvp("receiptHandle", receiptHandle))),
                                kvp("$inc", make_document(
                                            kvp("receivedCount", 1))),
                                kvp("$currentDate", make_document(
                                            kvp("modified", true),
                                            kvp("lastReceived", true)
                                            )));

                        mongocxx::options::find_one_and_update opts;
                        opts.return_document(mongocxx::options::return_document::k_after);

                        const auto claimed = messageCollection.find_one_and_update(filter.view(), update.view(), opts);
                        if (!claimed) break;

                        Entity::EQS::Message message;
                        message.FromDocument(claimed->view());

                        // Move to dead letter queue if existing
                        if (!deadLetterQueueErn.empty() && message.receivedCount > maxReceiveCount) {
                            const auto moveUpdate = make_document(
                                    kvp("$set", make_document(
                                                kvp("queueErn", deadLetterQueueErn),
                                                kvp("status", MessageStatusToString(Entity::EQS::MessageStatus::AVAILABLE)),
                                                kvp("receivedCount", 0),
                                                kvp("receiptHandle", ""))),
                                    kvp("$currentDate", make_document(
                                                kvp("modified", true))));
                            messageCollection.update_one(make_document(kvp("messageId", message.messageId)).view(), moveUpdate.view());

                            const auto sourceUpdate = make_document(
                                    kvp("$inc", make_document(
                                                kvp("size", static_cast<int64_t>(-message.size)),
                                                kvp("available", static_cast<int64_t>(-1)))),
                                    kvp("$currentDate", make_document(
                                                kvp("modified", true))));
                            queueCollection.update_one(make_document(kvp("ern", queueErn)).view(), sourceUpdate.view());

                            const auto targetUpdate = make_document(
                                    kvp("$inc", make_document(
                                                kvp("size", static_cast<int64_t>(message.size)),
                                                kvp("available", static_cast<int64_t>(1)))),
                                    kvp("$currentDate", make_document(
                                                kvp("modified", true))));
                            queueCollection.update_one(make_document(kvp("ern", deadLetterQueueErn)).view(), targetUpdate.view());

                            log_debug << "Message moved to dead letter queue, ern: " << queueErn << ", dlqErn: " << deadLetterQueueErn << ", messageId: " << message.messageId;
                            continue;
                        }

                        result.push_back(message);
                        taken += 1;
                    }
                }

                // Update queue counters
                if (!result.empty()) {
                    const auto queueFilter = make_document(kvp("ern", queueErn));
                    const auto queueUpdate = make_document(
                            kvp("$inc", make_document(
                                        kvp("invisible", static_cast<int64_t>(result.size())),
                                        kvp("available", -static_cast<int64_t>(result.size())))),
                            kvp("$currentDate", make_document(
                                        kvp("modified", true))));
                    queueCollection.update_one(queueFilter.view(), queueUpdate.view());
                    log_debug << "Messages received, ern: " << queueErn << ", count: " << result.size();
                    return result;
                }

                if (waitTime <= 0 || std::chrono::steady_clock::now() >= deadline) {
                    return result;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        } catch (const std::exception &e) {
            log_error << "Receive messages failed, ern: " << queueErn << ", error: " << e.what();
        }
        return result;
    }

    void MongoEqsRepository::deleteMessage(const std::string &receiptHandle) {

        try {
            const auto filter = make_document(
                    kvp("receiptHandle", receiptHandle));

            const auto entry = Database::instance().client();
            auto queueCollection = (*entry)[Database::instance().databaseName()][QUEUE_COLLECTION];
            auto messageCollection = (*entry)[Database::instance().databaseName()][MESSAGE_COLLECTION];

            Entity::EQS::Message message;
            if (auto mResult = messageCollection.find_one(filter.view())) {
                message.FromDocument(mResult->view());
            }

            const auto result = messageCollection.delete_many(filter.view());
            log_debug << "Message deleted, count: " << result->deleted_count();

            if (result && result->deleted_count() > 0 && !message.queueErn.empty()) {
                const auto queueFilter = make_document(kvp("ern", message.queueErn));
                const auto update = make_document(
                        kvp("$inc", make_document(
                                    kvp("size", static_cast<int64_t>(-message.size)),
                                    kvp("available", static_cast<int64_t>(message.status == Entity::EQS::MessageStatus::AVAILABLE ? -1 : 0)),
                                    kvp("delayed", static_cast<int64_t>(message.status == Entity::EQS::MessageStatus::DELAYED ? -1 : 0)),
                                    kvp("invisible", static_cast<int64_t>(message.status == Entity::EQS::MessageStatus::INVISIBLE ? -1 : 0)))),
                        kvp("$currentDate", make_document(
                                    kvp("modified", true))));
                queueCollection.update_one(queueFilter.view(), update.view());
            }
        } catch (const std::exception &e) {
            log_error << "Delete message failed, error: " << e.what();
        }
    }

    void MongoEqsRepository::deleteMessageById(const std::string &messageId) {

        try {
            const auto filter = make_document(
                    kvp("messageId", messageId));

            const auto entry = Database::instance().client();
            auto queueCollection = (*entry)[Database::instance().databaseName()][QUEUE_COLLECTION];
            auto messageCollection = (*entry)[Database::instance().databaseName()][MESSAGE_COLLECTION];

            Entity::EQS::Message message;
            if (auto mResult = messageCollection.find_one(filter.view())) {
                message.FromDocument(mResult->view());
            }

            const auto result = messageCollection.delete_many(filter.view());
            log_debug << "Message deleted, count: " << result->deleted_count();

            if (result && result->deleted_count() > 0 && !message.queueErn.empty()) {
                const auto queueFilter = make_document(kvp("ern", message.queueErn));
                const auto update = make_document(
                        kvp("$inc", make_document(
                                    kvp("size", static_cast<int64_t>(-message.size)),
                                    kvp("available", static_cast<int64_t>(message.status == Entity::EQS::MessageStatus::AVAILABLE ? -1 : 0)),
                                    kvp("delayed", static_cast<int64_t>(message.status == Entity::EQS::MessageStatus::DELAYED ? -1 : 0)),
                                    kvp("invisible", static_cast<int64_t>(message.status == Entity::EQS::MessageStatus::INVISIBLE ? -1 : 0)))),
                        kvp("$currentDate", make_document(
                                    kvp("modified", true))));
                queueCollection.update_one(queueFilter.view(), update.view());
            }
        } catch (const std::exception &e) {
            log_error << "Delete message by ID failed, messageId: " << messageId << ", error: " << e.what();
        }
    }

    void MongoEqsRepository::purgeQueue(const std::string &queueErn) {

        try {
            const auto filter = make_document(
                    kvp("queueErn", queueErn));

            const auto entry = Database::instance().client();
            auto queueCollection = (*entry)[Database::instance().databaseName()][QUEUE_COLLECTION];
            auto messageCollection = (*entry)[Database::instance().databaseName()][MESSAGE_COLLECTION];

            const auto result = messageCollection.delete_many(filter.view());
            log_debug << "Queue purged, ern: " << queueErn << ", count: " << result->deleted_count();

            const auto queueFilter = make_document(kvp("ern", queueErn));
            const auto update = make_document(
                    kvp("$set", make_document(
                                kvp("size", static_cast<int64_t>(0)),
                                kvp("available", static_cast<int64_t>(0)),
                                kvp("delayed", static_cast<int64_t>(0)),
                                kvp("invisible", static_cast<int64_t>(0)))),
                    kvp("$currentDate", make_document(
                                kvp("modified", true))));
            queueCollection.update_one(queueFilter.view(), update.view());
        } catch (const std::exception &e) {
            log_error << "Purge queue failed, ern: " << queueErn << ", error: " << e.what();
        }
    }

    void MongoEqsRepository::purgeAllQueues(const std::string &region, const std::string &accountId) {

        try {
            const auto entry = Database::instance().client();
            auto queueCollection = (*entry)[Database::instance().databaseName()][QUEUE_COLLECTION];
            auto messageCollection = (*entry)[Database::instance().databaseName()][MESSAGE_COLLECTION];

            // Filters on the entity's own region/accountId fields now that they exist, rather
            // than the previous full-scan-and-substring-match-on-ERN workaround.
            array ernArray;
            long queueCount = 0;
            const auto scopeFilter = make_document(kvp("region", region), kvp("accountId", accountId));
            for (auto queueCursor = queueCollection.find(scopeFilter.view()); auto queue: queueCursor) {
                ernArray.append(Entity::EQS::Queue::fromDocument(queue).ern);
                ++queueCount;
            }

            if (queueCount == 0) {
                log_debug << "No queues found, region: " << region << ", accountId: " << accountId;
                return;
            }

            const auto queueFilter = make_document(kvp("ern", make_document(kvp("$in", ernArray.view()))));

            const auto messageResult = messageCollection.delete_many(make_document(kvp("queueErn", make_document(kvp("$in", ernArray.view())))));
            log_debug << "Queues purged, region: " << region << ", accountId: " << accountId << ", queueCount: " << queueCount << ", messageCount: " << messageResult->deleted_count();

            const auto update = make_document(
                    kvp("$set", make_document(
                                kvp("size", static_cast<int64_t>(0)),
                                kvp("available", static_cast<int64_t>(0)),
                                kvp("delayed", static_cast<int64_t>(0)),
                                kvp("invisible", static_cast<int64_t>(0)))),
                    kvp("$currentDate", make_document(
                                kvp("modified", true))));
            queueCollection.update_many(queueFilter.view(), update.view());
        } catch (const std::exception &e) {
            log_error << "Purge all queues failed, region: " << region << ", accountId: " << accountId << ", error: " << e.what();
        }
    }

    long MongoEqsRepository::countMessages() const {

        try {
            const auto entry = Database::instance().client();
            auto messageCollection = (*entry)[Database::instance().databaseName()][MESSAGE_COLLECTION];

            return messageCollection.count_documents({});
        } catch (const std::exception &e) {
            log_error << "Count messages failed, error: " << e.what();
        }
        return -1;
    }

    long MongoEqsRepository::countMessages(const std::string &queueErn) const {

        try {
            const auto filter = make_document(kvp("queueErn", queueErn));
            const auto entry = Database::instance().client();
            auto messageCollection = (*entry)[Database::instance().databaseName()][MESSAGE_COLLECTION];

            return messageCollection.count_documents(filter.view());
        } catch (const std::exception &e) {
            log_error << "Count messages failed, ern: " << queueErn << ", error: " << e.what();
        }
        return -1;
    }

    void MongoEqsRepository::clearMessages() {

        try {
            const auto entry = Database::instance().client();
            auto messageCollection = (*entry)[Database::instance().databaseName()][MESSAGE_COLLECTION];

            const auto result = messageCollection.delete_many({});
            log_debug << "All messages deleted, count: " << result->deleted_count();
        } catch (const std::exception &e) {
            log_error << "Delete all messages failed, error: " << e.what();
        }
    }

    long MongoEqsRepository::resetExpiredMessages() {

        long resetCount = 0;
        try {
            const auto now = std::chrono::system_clock::now();
            std::map<std::string, long> resetCountByQueue;
            std::map<std::string, long> delayedResetCountByQueue;

            const auto entry = Database::instance().client();
            auto queueCollection = (*entry)[Database::instance().databaseName()][QUEUE_COLLECTION];
            auto messageCollection = (*entry)[Database::instance().databaseName()][MESSAGE_COLLECTION];

            const auto filter = make_document(kvp("status", MessageStatusToString(Entity::EQS::MessageStatus::INVISIBLE)));
            for (auto cursor = messageCollection.find(filter.view()); auto doc: cursor) {
                Entity::EQS::Message message;
                message.FromDocument(doc);
                if (now < message.lastReceived + std::chrono::seconds(message.visibilityTimeout)) continue;

                const auto resetFilter = make_document(
                        kvp("_id", bsoncxx::oid{message.oid}),
                        kvp("status", MessageStatusToString(Entity::EQS::MessageStatus::INVISIBLE)));
                const auto update = make_document(
                        kvp("$set", make_document(
                                    kvp("status", MessageStatusToString(Entity::EQS::MessageStatus::AVAILABLE)),
                                    kvp("receiptHandle", ""))),
                        kvp("$currentDate", make_document(
                                    kvp("modified", true),
                                    kvp("lastReceived", true))));
                const auto updateResult = messageCollection.update_one(resetFilter.view(), update.view());

                // Message was concurrently deleted or already reset since the cursor read it; skip counting.
                if (!updateResult || updateResult->modified_count() == 0) continue;

                resetCountByQueue[message.queueErn]++;
                resetCount++;
                log_debug << "Message visibility timeout expired, messageId: " << message.messageId << ", queueErn: " << message.queueErn;
            }

            const auto delayedFilter = make_document(kvp("status", MessageStatusToString(Entity::EQS::MessageStatus::DELAYED)));
            for (auto cursor = messageCollection.find(delayedFilter.view()); auto doc: cursor) {
                Entity::EQS::Message message;
                message.FromDocument(doc);
                if (now < message.delayUntil) continue;

                const auto resetFilter = make_document(
                        kvp("_id", bsoncxx::oid{message.oid}),
                        kvp("status", MessageStatusToString(Entity::EQS::MessageStatus::DELAYED)));
                const auto update = make_document(
                        kvp("$set", make_document(
                                    kvp("status", MessageStatusToString(Entity::EQS::MessageStatus::AVAILABLE)))),
                        kvp("$currentDate", make_document(
                                    kvp("modified", true))));
                const auto updateResult = messageCollection.update_one(resetFilter.view(), update.view());

                // Message was concurrently deleted or already reset since the cursor read it; skip counting.
                if (!updateResult || updateResult->modified_count() == 0) continue;

                delayedResetCountByQueue[message.queueErn]++;
                resetCount++;
                log_debug << "Message delay expired, messageId: " << message.messageId << ", queueErn: " << message.queueErn;
            }

            for (const auto &[queueErn, count]: resetCountByQueue) {
                const auto queueUpdate = make_document(
                        kvp("$inc", make_document(
                                    kvp("invisible", -static_cast<int64_t>(count)),
                                    kvp("available", static_cast<int64_t>(count)))),
                        kvp("$currentDate", make_document(
                                    kvp("modified", true))));
                queueCollection.update_one(make_document(kvp("ern", queueErn)).view(), queueUpdate.view());
            }

            for (const auto &[queueErn, count]: delayedResetCountByQueue) {
                const auto queueUpdate = make_document(
                        kvp("$inc", make_document(
                                    kvp("delayed", -static_cast<int64_t>(count)),
                                    kvp("available", static_cast<int64_t>(count)))),
                        kvp("$currentDate", make_document(
                                    kvp("modified", true))));
                queueCollection.update_one(make_document(kvp("ern", queueErn)).view(), queueUpdate.view());
            }

            if (resetCount > 0)
                log_debug << "Reset expired messages, count: " << resetCount;

        } catch (const std::exception &e) {
            log_error << "Reset expired messages failed, error: " << e.what();
        }
        return resetCount;
    }

}// namespace Euclid::Database