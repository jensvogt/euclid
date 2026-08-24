//
// Created by vogje01 on 5/24/26.
//
#pragma once

// C++ includes
#include <algorithm>
#include <array>
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
#include <euclid/database/entity/eqs/Message.h>
#include <euclid/database/entity/eqs/PriorityWeights.h>
#include <euclid/database/entity/eqs/Queue.h>
#include <euclid/database/repository/eqs/IEqsRepository.h>

namespace Euclid::Database {

    /**
     * @brief Sqs memory database.
     *
     * Controls all the AwsMock sqss.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class MemoryEqsRepository final : public IEqsRepository {

    public:

        /**
         * @brief Singleton instance
         */
        static MemoryEqsRepository &instance() {
            static MemoryEqsRepository sqsDatabase;
            return sqsDatabase;
        }

        Entity::EQS::Queue upsertQueue(Entity::EQS::Queue &queue) override {
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

        std::optional<Entity::EQS::Queue> findQueueByName(const std::string &name) const override {
            std::lock_guard lock(_mutex);
            const auto it = _queueStore.find(name);
            if (it == _queueStore.end()) return std::nullopt;
            return it->second;
        }

        std::optional<Entity::EQS::Queue> findQueueById(const std::string &id) const override {
            std::lock_guard lock(_mutex);
            for (const auto &m: _queueStore | std::views::values) {
                if (m.oid == id) return m;
            }
            return std::nullopt;
        }

        std::optional<Entity::EQS::Queue> findQueueByErn(const std::string &ern) const override {
            std::lock_guard lock(_mutex);
            for (const auto &m: _queueStore | std::views::values) {
                if (m.ern == ern) return m;
            }
            return std::nullopt;
        }

        std::vector<Entity::EQS::Queue> listQueues(const std::string &prefix, const long pageSize, const long pageIndex, const std::string &sortColumn) const override {
            std::lock_guard lock(_mutex);
            std::vector<Entity::EQS::Queue> result;
            for (const auto &m: _queueStore | std::views::values) {
                if (prefix.empty() || m.name.starts_with(prefix)) {
                    result.push_back(m);
                }
            }

            if (sortColumn == "name") {
                std::ranges::sort(result, {}, &Entity::EQS::Queue::name);
            } else if (sortColumn == "ern") {
                std::ranges::sort(result, {}, &Entity::EQS::Queue::ern);
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

        void upsertMessage(const Entity::EQS::Message &message) override {
            std::lock_guard lock(_mutex);
            // TODO: fix me
            //_messageStore[message.name] = message;
        }

        Entity::EQS::Message sendMessage(const std::string &messageId, const std::string &ern, const std::string &queueErn, const std::string &body, const std::map<std::string, Entity::EQS::Variant> &attributes, const Entity::EQS::MessagePriority priority) override {
            std::lock_guard lock(_mutex);

            Entity::EQS::Message message;
            message.ern = ern;
            message.queueErn = queueErn;
            message.body = body;
            message.size = static_cast<long>(body.size());
            message.messageId = messageId;
            message.contentType = Core::ContentTypeUtils::fromContent(message.body);
            message.attributes = attributes;
            message.md5Attributes = Entity::EQS::Message::ComputeAttributesMd5(attributes);
            message.priority = priority;

            for (auto &queue: _queueStore | std::views::values) {
                if (queue.ern == queueErn) {
                    message.visibilityTimeout = queue.visibility;
                    if (queue.delay > 0) {
                        message.status = Entity::EQS::MessageStatus::DELAYED;
                        message.delayUntil = std::chrono::system_clock::now() + std::chrono::seconds(queue.delay);
                        queue.delayed += 1;
                    } else {
                        queue.available += 1;
                    }
                    queue.size += message.size;
                    queue.modified = std::chrono::system_clock::now();
                    break;
                }
            }

            _messageStore[message.messageId] = message;
            return message;
        }

        std::vector<Entity::EQS::Message> receiveMessages(const std::string &queueErn, const long maxCount, const long waitTime) override {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(waitTime);
            const auto weights = Entity::EQS::LoadPriorityWeights();
            static constexpr std::array priorityOrder{Entity::EQS::MessagePriority::HIGH, Entity::EQS::MessagePriority::MIDDLE, Entity::EQS::MessagePriority::LOW};

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

                    std::map<Entity::EQS::MessagePriority, long> availableCounts;
                    for (const auto &message: _messageStore | std::views::values) {
                        if (message.queueErn == queueErn && message.status == Entity::EQS::MessageStatus::AVAILABLE) {
                            availableCounts[message.priority] += 1;
                        }
                    }
                    const auto takeCounts = Entity::EQS::ComputeReceiveCounts(maxCount, availableCounts, weights);

                    std::vector<Entity::EQS::Message> result;
                    long movedCount = 0;
                    long movedSize = 0;
                    for (const auto priority: priorityOrder) {
                        const long target = takeCounts.at(priority);
                        long taken = 0;
                        if (target <= 0) continue;

                        for (auto &message: _messageStore | std::views::values) {
                            if (taken >= target || static_cast<long>(result.size()) >= maxCount) break;
                            if (message.queueErn != queueErn || message.status != Entity::EQS::MessageStatus::AVAILABLE || message.priority != priority) continue;

                            message.receivedCount += 1;
                            message.modified = std::chrono::system_clock::now();
                            message.lastReceived = message.modified;

                            if (!deadLetterQueueErn.empty() && message.receivedCount > maxReceiveCount) {
                                movedCount += 1;
                                movedSize += message.size;
                                message.queueErn = deadLetterQueueErn;
                                message.status = Entity::EQS::MessageStatus::AVAILABLE;
                                message.receivedCount = 0;
                                message.receiptHandle.clear();
                                log_info << "Message moved to dead letter queue, ern: " << queueErn << ", dlqErn: " << deadLetterQueueErn << ", messageId: " << message.messageId;
                                continue;
                            }

                            message.status = Entity::EQS::MessageStatus::INVISIBLE;
                            message.receiptHandle = Core::UuidUtils::CreateRandomUuid();
                            result.push_back(message);
                            taken += 1;
                        }
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
                    if (message.status == Entity::EQS::MessageStatus::AVAILABLE) {
                        queue.available -= 1;
                    } else if (message.status == Entity::EQS::MessageStatus::DELAYED) {
                        queue.delayed -= 1;
                    } else if (message.status == Entity::EQS::MessageStatus::INVISIBLE) {
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

        std::optional<Entity::EQS::Message> findMessageByName(const std::string &name) const override {
            std::lock_guard lock(_mutex);
            const auto it = _messageStore.find(name);
            if (it == _messageStore.end()) return std::nullopt;
            return it->second;
        }

        std::optional<Entity::EQS::Message> findMessageById(const std::string &id) const override {
            std::lock_guard lock(_mutex);
            for (const auto &m: _messageStore | std::views::values) {
                if (m.oid == id) return m;
            }
            return std::nullopt;
        }

        std::vector<Entity::EQS::Message> findAllMessages() const override {
            std::lock_guard lock(_mutex);
            std::vector<Entity::EQS::Message> result;
            result.reserve(_messageStore.size());
            for (const auto &m: _messageStore | std::views::values) result.push_back(m);
            return result;
        }

        std::vector<Entity::EQS::Message> listMessages(const std::string &queueErn, const long pageSize, const long pageIndex, const std::string &sortColumn) const override {
            std::lock_guard lock(_mutex);
            std::vector<Entity::EQS::Message> result;
            for (const auto &m: _messageStore | std::views::values) {
                if (m.queueErn == queueErn) {
                    result.push_back(m);
                }
            }

            if (sortColumn == "created") {
                std::ranges::sort(result, {}, &Entity::EQS::Message::created);
            } else if (sortColumn == "size") {
                std::ranges::sort(result, {}, &Entity::EQS::Message::size);
            } else if (sortColumn == "messageId") {
                std::ranges::sort(result, {}, &Entity::EQS::Message::messageId);
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
            std::map<std::string, long> delayedResetCountByQueue;

            for (auto &message: _messageStore | std::views::values) {
                if (message.status == Entity::EQS::MessageStatus::INVISIBLE) {
                    if (now < message.lastReceived + std::chrono::seconds(message.visibilityTimeout)) continue;

                    message.status = Entity::EQS::MessageStatus::AVAILABLE;
                    message.lastReceived = std::chrono::system_clock::time_point{};
                    message.receiptHandle.clear();
                    resetCountByQueue[message.queueErn]++;
                    log_debug << "Message visibility timeout expired, messageId: " << message.messageId << ", queueErn: " << message.queueErn;
                } else if (message.status == Entity::EQS::MessageStatus::DELAYED) {
                    if (now < message.delayUntil) continue;

                    message.status = Entity::EQS::MessageStatus::AVAILABLE;
                    delayedResetCountByQueue[message.queueErn]++;
                    log_debug << "Message delay expired, messageId: " << message.messageId << ", queueErn: " << message.queueErn;
                }
            }

            long resetCount = 0;
            for (const auto &[queueErn, count]: resetCountByQueue) {
                resetCount += count;
                for (auto &queue: _queueStore | std::views::values) {
                    if (queue.ern == queueErn) {
                        queue.invisible -= count;
                        queue.available += count;
                        queue.modified = now;
                        break;
                    }
                }
            }

            for (const auto &[queueErn, count]: delayedResetCountByQueue) {
                resetCount += count;
                for (auto &queue: _queueStore | std::views::values) {
                    if (queue.ern == queueErn) {
                        queue.delayed -= count;
                        queue.available += count;
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
        std::unordered_map<std::string, Entity::EQS::Queue> _queueStore;
        std::unordered_map<std::string, Entity::EQS::Message> _messageStore;
    };

}// namespace Euclid::Database