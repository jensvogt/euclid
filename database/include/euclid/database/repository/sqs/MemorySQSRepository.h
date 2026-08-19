//
// Created by vogje01 on 5/24/26.
//
#pragma once

// C++ includes
#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <ranges>
#include <set>
#include <thread>
#include <unordered_map>

// Euclid includes
#include <euclid/core/ContentTypeUtils.h>
#include <euclid/core/LogStream.h>
#include <euclid/core/UuidUtils.h>
#include <euclid/database/entity/sqs/Message.h>
#include <euclid/database/entity/sqs/Queue.h>
#include <euclid/database/repository/sqs/ISQSRepository.h>

namespace Euclid::Database {

    /**
     * @brief Sqs memory database.
     *
     * Controls all the AwsMock sqss.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class MemorySQSRepository final : public ISQSRepository {

    public:

        /**
         * @brief Singleton instance
         */
        static MemorySQSRepository &instance() {
            static MemorySQSRepository sqsDatabase;
            return sqsDatabase;
        }

        Entity::SQS::Queue upsertQueue(Entity::SQS::Queue &queue) override {
            std::lock_guard lock(_mutex);
            if (queue.oid.empty()) {
                queue.oid = Core::UuidUtils::CreateRandomUuid();
                _queueStore[queue.oid] = queue;
            }
            return queue;
        }

        void removeQueueByName(const std::string &name) override {
            std::lock_guard lock(_mutex);
            std::erase_if(_queueStore, [&name](const auto &kv) {
                return kv.second.name == name;
            });
        }

        void deleteQueueByErn(const std::string &ern) override {
            std::lock_guard lock(_mutex);
            std::erase_if(_queueStore, [&ern](const auto &kv) {
                return kv.second.ern == ern;
            });
        }

        std::optional<Entity::SQS::Queue> findQueueByName(const std::string &name) const override {
            std::lock_guard lock(_mutex);
            const auto it = _queueStore.find(name);
            if (it == _queueStore.end()) return std::nullopt;
            return it->second;
        }

        std::optional<Entity::SQS::Queue> findQueueById(const std::string &id) const override {
            std::lock_guard lock(_mutex);
            for (const auto &m: _queueStore | std::views::values) {
                if (m.oid == id) return m;
            }
            return std::nullopt;
        }

        std::optional<Entity::SQS::Queue> findQueueByErn(const std::string &ern) const override {
            std::lock_guard lock(_mutex);
            for (const auto &m: _queueStore | std::views::values) {
                if (m.ern == ern) return m;
            }
            return std::nullopt;
        }

        std::vector<Entity::SQS::Queue> listQueues(const std::string &prefix, const long pageSize, const long pageIndex, const std::string &sortColumn) const override {
            std::lock_guard lock(_mutex);
            std::vector<Entity::SQS::Queue> result;
            for (const auto &m: _queueStore | std::views::values) {
                if (prefix.empty() || m.name.starts_with(prefix)) {
                    result.push_back(m);
                }
            }

            if (sortColumn == "name") {
                std::ranges::sort(result, {}, &Entity::SQS::Queue::name);
            } else if (sortColumn == "ern") {
                std::ranges::sort(result, {}, &Entity::SQS::Queue::ern);
            }

            if (pageSize > 0) {
                const auto offset = std::min<size_t>(std::max<long>(pageIndex, 0) * pageSize, result.size());
                const auto end = std::min<size_t>(offset + pageSize, result.size());
                result = std::vector(result.begin() + static_cast<long>(offset), result.begin() + static_cast<long>(end));
            }
            return result;
        }

        bool queueExists(const std::string &name) const override {
            std::lock_guard lock(_mutex);
            return _queueStore.contains(name);
        }

        long countQueues() const override {
            std::lock_guard lock(_mutex);
            return static_cast<long>(_queueStore.size());
        }

        void clearQueues() override {
            std::lock_guard lock(_mutex);
            _queueStore.clear();
        }

        void upsertMessage(const Entity::SQS::Message &message) override {
            std::lock_guard lock(_mutex);
            // TODO: fix me
            //_messageStore[message.name] = message;
        }

        Entity::SQS::Message sendMessage(const std::string &messageId, const std::string &ern, const std::string &queueErn, const std::string &body, const std::map<std::string, Entity::SQS::Variant> &attributes) override {
            std::lock_guard lock(_mutex);

            Entity::SQS::Message message;
            message.ern = ern;
            message.queueErn = queueErn;
            message.body = body;
            message.size = static_cast<long>(body.size());
            message.messageId = messageId;
            message.contentType = Core::ContentTypeUtils::fromContent(message.body);
            message.attributes = attributes;
            message.md5Attributes = Entity::SQS::Message::ComputeAttributesMd5(attributes);

            for (auto &queue: _queueStore | std::views::values) {
                if (queue.ern == queueErn) {
                    message.visibilityTimeout = queue.visibility;
                    queue.available += 1;
                    queue.size += message.size;
                    queue.modified = std::chrono::system_clock::now();
                    break;
                }
            }

            _messageStore[message.messageId] = message;
            return message;
        }

        std::vector<Entity::SQS::Message> receiveMessages(const std::string &queueErn, const long maxCount, const long waitTime) override {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(waitTime);

            while (true) {
                {
                    std::lock_guard lock(_mutex);

                    long maxReceiveCount = 0;
                    std::string deadLetterQueueErn;
                    for (const auto &queue: _queueStore | std::views::values) {
                        if (queue.ern == queueErn) {
                            maxReceiveCount = queue.maxReceiveCount;
                            deadLetterQueueErn = queue.deadLetterQueueErn;
                            break;
                        }
                    }

                    std::vector<Entity::SQS::Message> result;
                    long movedCount = 0;
                    long movedSize = 0;
                    for (auto &message: _messageStore | std::views::values) {
                        if (static_cast<long>(result.size()) >= maxCount) break;
                        if (message.queueErn != queueErn || message.status != Entity::SQS::MessageStatus::AVAILABLE) continue;

                        message.receivedCount += 1;
                        message.modified = std::chrono::system_clock::now();
                        message.lastReceived = message.modified;

                        if (!deadLetterQueueErn.empty() && message.receivedCount > maxReceiveCount) {
                            movedCount += 1;
                            movedSize += message.size;
                            message.queueErn = deadLetterQueueErn;
                            message.status = Entity::SQS::MessageStatus::AVAILABLE;
                            message.receivedCount = 0;
                            message.receiptHandle.clear();
                            log_info << "Message moved to dead letter queue, ern: " << queueErn << ", dlqErn: " << deadLetterQueueErn << ", messageId: " << message.messageId;
                            continue;
                        }

                        message.status = Entity::SQS::MessageStatus::INVISIBLE;
                        message.receiptHandle = Core::UuidUtils::CreateRandomUuid();
                        result.push_back(message);
                    }

                    if (movedCount > 0) {
                        for (auto &queue: _queueStore | std::views::values) {
                            if (queue.ern == queueErn) {
                                queue.available -= movedCount;
                                queue.size -= movedSize;
                                queue.modified = std::chrono::system_clock::now();
                            } else if (queue.ern == deadLetterQueueErn) {
                                queue.available += movedCount;
                                queue.size += movedSize;
                                queue.modified = std::chrono::system_clock::now();
                            }
                        }
                    }

                    if (!result.empty()) {
                        for (auto &queue: _queueStore | std::views::values) {
                            if (queue.ern == queueErn) {
                                queue.invisible += static_cast<long>(result.size());
                                queue.modified = std::chrono::system_clock::now();
                                break;
                            }
                        }
                        return result;
                    }
                }

                if (waitTime <= 0 || std::chrono::steady_clock::now() >= deadline) {
                    return {};
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }

        void deleteMessage(const std::string &receiptHandle) override {
            std::lock_guard lock(_mutex);

            const auto it = std::ranges::find_if(_messageStore, [&receiptHandle](const auto &kv) {
                return kv.second.receiptHandle == receiptHandle;
            });
            if (it == _messageStore.end()) {
                return;
            }
            const auto &message = it->second;

            for (auto &queue: _queueStore | std::views::values) {
                if (queue.ern == message.queueErn) {
                    queue.size -= message.size;
                    if (message.status == Entity::SQS::MessageStatus::AVAILABLE) {
                        queue.delayed -= 1;
                    } else if (message.status == Entity::SQS::MessageStatus::DELAYED) {
                        queue.delayed -= 1;
                    } else if (message.status == Entity::SQS::MessageStatus::INVISIBLE) {
                        queue.invisible -= 1;
                    }
                    queue.modified = std::chrono::system_clock::now();
                    break;
                }
            }

            _messageStore.erase(it);
        }

        void purgeQueue(const std::string &queueErn) override {
            std::lock_guard lock(_mutex);
            std::erase_if(_messageStore, [&queueErn](const auto &kv) {
                return kv.second.queueErn == queueErn;
            });

            for (auto &queue: _queueStore | std::views::values) {
                if (queue.ern == queueErn) {
                    queue.size = 0;
                    queue.available = 0;
                    queue.delayed = 0;
                    queue.invisible = 0;
                    queue.modified = std::chrono::system_clock::now();
                    break;
                }
            }
        }

        void purgeAllQueues(const std::string &region, const std::string &accountId) override {
            std::lock_guard lock(_mutex);
            const std::string marker = ":" + region + ":" + accountId + ":";

            std::set<std::string> ernsToPurge;
            for (auto &queue: _queueStore | std::views::values) {
                if (queue.ern.find(marker) == std::string::npos) {
                    continue;
                }
                ernsToPurge.insert(queue.ern);
                queue.size = 0;
                queue.available = 0;
                queue.delayed = 0;
                queue.invisible = 0;
                queue.modified = std::chrono::system_clock::now();
            }

            std::erase_if(_messageStore, [&ernsToPurge](const auto &kv) {
                return ernsToPurge.contains(kv.second.queueErn);
            });
        }

        std::optional<Entity::SQS::Message> findMessageByName(const std::string &name) const override {
            std::lock_guard lock(_mutex);
            const auto it = _messageStore.find(name);
            if (it == _messageStore.end()) return std::nullopt;
            return it->second;
        }

        std::optional<Entity::SQS::Message> findMessageById(const std::string &id) const override {
            std::lock_guard lock(_mutex);
            for (const auto &m: _messageStore | std::views::values) {
                if (m.oid == id) return m;
            }
            return std::nullopt;
        }

        std::vector<Entity::SQS::Message> findAllMessages() const override {
            std::lock_guard lock(_mutex);
            std::vector<Entity::SQS::Message> result;
            result.reserve(_messageStore.size());
            for (const auto &m: _messageStore | std::views::values) result.push_back(m);
            return result;
        }

        std::vector<Entity::SQS::Message> listMessages(const std::string &queueErn, const long pageSize, const long pageIndex, const std::string &sortColumn) const override {
            std::lock_guard lock(_mutex);
            std::vector<Entity::SQS::Message> result;
            for (const auto &m: _messageStore | std::views::values) {
                if (m.queueErn == queueErn) {
                    result.push_back(m);
                }
            }

            if (sortColumn == "created") {
                std::ranges::sort(result, {}, &Entity::SQS::Message::created);
            } else if (sortColumn == "size") {
                std::ranges::sort(result, {}, &Entity::SQS::Message::size);
            } else if (sortColumn == "messageId") {
                std::ranges::sort(result, {}, &Entity::SQS::Message::messageId);
            }

            if (pageSize > 0) {
                const auto offset = std::min<size_t>(std::max<long>(pageIndex, 0) * pageSize, result.size());
                const auto end = std::min<size_t>(offset + pageSize, result.size());
                result = std::vector(result.begin() + static_cast<long>(offset), result.begin() + static_cast<long>(end));
            }
            return result;
        }

        bool messageExists(const std::string &name) const override {
            std::lock_guard lock(_mutex);
            return _messageStore.contains(name);
        }

        long countMessages() const override {
            std::lock_guard lock(_mutex);
            return _messageStore.size();
        }

        long countMessages(const std::string &queueErn) const override {
            std::lock_guard lock(_mutex);
            return std::ranges::count_if(_messageStore | std::views::values, [&queueErn](const auto &message) {
                return message.queueErn == queueErn;
            });
        }

        void clearMessages() override {
            std::lock_guard lock(_mutex);
            _messageStore.clear();
        }

        long resetExpiredMessages() override {
            std::lock_guard lock(_mutex);

            const auto now = std::chrono::system_clock::now();
            std::map<std::string, long> resetCountByQueue;

            for (auto &message: _messageStore | std::views::values) {
                if (message.status != Entity::SQS::MessageStatus::INVISIBLE) continue;
                if (now < message.lastReceived + std::chrono::seconds(message.visibilityTimeout)) continue;

                message.status = Entity::SQS::MessageStatus::AVAILABLE;
                message.lastReceived = std::chrono::system_clock::time_point{};
                message.receiptHandle.clear();
                resetCountByQueue[message.queueErn]++;
                log_debug << "Message visibility timeout expired, messageId: " << message.messageId << ", queueErn: " << message.queueErn;
            }

            long resetCount = 0;
            for (const auto &[queueErn, count]: resetCountByQueue) {
                resetCount += count;
                for (auto &queue: _queueStore | std::views::values) {
                    if (queue.ern == queueErn) {
                        queue.invisible -= count;
                        queue.modified = now;
                        break;
                    }
                }
            }

            if (resetCount > 0)
                log_info << "Reset expired messages, count: " << resetCount;
            return resetCount;
        }

    private:

        mutable std::mutex _mutex;
        std::unordered_map<std::string, Entity::SQS::Queue> _queueStore;
        std::unordered_map<std::string, Entity::SQS::Message> _messageStore;
    };

}// namespace Euclid::Database