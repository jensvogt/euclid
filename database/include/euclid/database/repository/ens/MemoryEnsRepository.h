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
#include <euclid/database/entity/ens/Message.h>
#include <euclid/database/entity/ens/Topic.h>
#include <euclid/database/repository/ens/IEnsRepository.h>

namespace Euclid::Database {

    /**
     * @brief ENS memory database.
     *
     * Controls all the AwsMock sqss.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class MemoryEnsRepository final : public IEnsRepository {

    public:

        /**
         * @brief Singleton instance
         */
        static MemoryEnsRepository &instance() {
            static MemoryEnsRepository ensDatabase;
            return ensDatabase;
        }

        Entity::ENS::Topic upsertTopic(Entity::ENS::Topic &topic) override {
            std::lock_guard lock(_mutex);
            if (topic.oid.empty()) {
                topic.oid = Core::UuidUtils::CreateRandomUuid();
                _topicStore[topic.oid] = topic;
            }
            return topic;
        }

        //
        // void removeQueueByName(const std::string &name) override {
        //     std::lock_guard lock(_mutex);
        //     std::erase_if(_queueStore, [&name](const auto &kv) {
        //         return kv.second.name == name;
        //     });
        // }
        //
        void deleteTopicByErn(const std::string &ern) override {
            std::lock_guard lock(_mutex);
            std::erase_if(_topicStore, [&ern](const auto &kv) {
                return kv.second.ern == ern;
            });
        }


        std::optional<Entity::ENS::Topic> findTopicByName(const std::string &name) const override {
            std::lock_guard lock(_mutex);
            const auto it = _topicStore.find(name);
            if (it == _topicStore.end()) return std::nullopt;
            return it->second;
        }

        //
        // std::optional<Entity::EQS::Queue> findQueueById(const std::string &id) const override {
        //     std::lock_guard lock(_mutex);
        //     for (const auto &m: _queueStore | std::views::values) {
        //         if (m.oid == id) return m;
        //     }
        //     return std::nullopt;
        // }
        //
        // std::optional<Entity::EQS::Queue> findQueueByErn(const std::string &ern) const override {
        //     std::lock_guard lock(_mutex);
        //     for (const auto &m: _queueStore | std::views::values) {
        //         if (m.ern == ern) return m;
        //     }
        //     return std::nullopt;
        // }
        //
        std::vector<Entity::ENS::Topic> listTopics(const std::string &accountId, const std::string &namespaceName, const std::string &prefix, const long pageSize, const long pageIndex, const std::string &sortColumn) const override {
            std::lock_guard lock(_mutex);
            std::vector<Entity::ENS::Topic> result;
            for (const auto &m: _topicStore | std::views::values) {
                if (m.accountId != accountId) continue;
                if (!namespaceName.empty() && m.namespaceName != namespaceName) continue;
                if (prefix.empty() || m.name.starts_with(prefix)) {
                    result.push_back(m);
                }
            }

            if (sortColumn == "name") {
                std::ranges::sort(result, {}, &Entity::ENS::Topic::name);
            } else if (sortColumn == "ern") {
                std::ranges::sort(result, {}, &Entity::ENS::Topic::ern);
            }

            if (pageSize > 0) {
                const auto offset = std::min<size_t>(std::max<long>(pageIndex, 0) * pageSize, result.size());
                const auto end = std::min<size_t>(offset + pageSize, result.size());
                result = std::vector(result.begin() + static_cast<long>(offset), result.begin() + static_cast<long>(end));
            }
            return result;
        }

        //
        // bool queueExists(const std::string &name) const override {
        //     std::lock_guard lock(_mutex);
        //     return _queueStore.contains(name);
        // }
        //
        long countTopics(const std::string &accountId, const std::string &namespaceName) const override {
            std::lock_guard lock(_mutex);
            return std::ranges::count_if(_topicStore | std::views::values, [&](const auto &m) {
                return m.accountId == accountId && (namespaceName.empty() || m.namespaceName == namespaceName);
            });
        }

        //
        // void clearQueues() override {
        //     std::lock_guard lock(_mutex);
        //     _queueStore.clear();
        // }
        //
        // void upsertMessage(const Entity::EQS::Message &message) override {
        //     std::lock_guard lock(_mutex);
        //     // TODO: fix me
        //     //_messageStore[message.name] = message;
        // }

        Entity::ENS::Message publishMessage(const std::string &messageId, const std::string &ern, const std::string &topicErn, const std::string &body, const std::map<std::string, Entity::COM::Variant> &attributes) override {
            std::lock_guard lock(_mutex);

            Entity::ENS::Message message;
            message.ern = ern;
            message.topicErn = topicErn;
            message.body = body;
            message.size = static_cast<long>(body.size());
            message.messageId = messageId;
            message.contentType = Core::ContentTypeUtils::fromContent(message.body);
            message.attributes = attributes;
            message.md5Attributes = Entity::ENS::Message::ComputeAttributesMd5(attributes);

            for (auto &queue: _topicStore | std::views::values) {
                if (queue.ern == topicErn) {
                    queue.available += 1;
                    queue.size += message.size;
                    queue.modified = std::chrono::system_clock::now();
                    break;
                }
            }

            _messageStore[message.messageId] = message;
            return message;
        }

        //
        // std::vector<Entity::EQS::Message> receiveMessages(const std::string &queueErn, const long maxCount, const long waitTime) override {
        //     const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(waitTime);
        //     const auto weights = Entity::EQS::LoadPriorityWeights();
        //     static constexpr std::array priorityOrder{Entity::EQS::MessagePriority::HIGH, Entity::EQS::MessagePriority::MIDDLE, Entity::EQS::MessagePriority::LOW};
        //
        //     while (true) {
        //         {
        //             std::lock_guard lock(_mutex);
        //
        //             long maxReceiveCount = 0;
        //             std::string deadLetterQueueErn;
        //             for (const auto &queue: _queueStore | std::views::values) {
        //                 if (queue.ern == queueErn) {
        //                     maxReceiveCount = queue.maxReceiveCount;
        //                     deadLetterQueueErn = queue.deadLetterQueueErn;
        //                     break;
        //                 }
        //             }
        //
        //             std::map<Entity::EQS::MessagePriority, long> availableCounts;
        //             for (const auto &message: _messageStore | std::views::values) {
        //                 if (message.queueErn == queueErn && message.status == Entity::EQS::MessageStatus::AVAILABLE) {
        //                     availableCounts[message.priority] += 1;
        //                 }
        //             }
        //             const auto takeCounts = Entity::EQS::ComputeReceiveCounts(maxCount, availableCounts, weights);
        //
        //             std::vector<Entity::EQS::Message> result;
        //             long movedCount = 0;
        //             long movedSize = 0;
        //             for (const auto priority: priorityOrder) {
        //                 const long target = takeCounts.at(priority);
        //                 long taken = 0;
        //                 if (target <= 0) continue;
        //
        //                 for (auto &message: _messageStore | std::views::values) {
        //                     if (taken >= target || static_cast<long>(result.size()) >= maxCount) break;
        //                     if (message.queueErn != queueErn || message.status != Entity::EQS::MessageStatus::AVAILABLE || message.priority != priority) continue;
        //
        //                     message.receivedCount += 1;
        //                     message.modified = std::chrono::system_clock::now();
        //                     message.lastReceived = message.modified;
        //
        //                     if (!deadLetterQueueErn.empty() && message.receivedCount > maxReceiveCount) {
        //                         movedCount += 1;
        //                         movedSize += message.size;
        //                         message.queueErn = deadLetterQueueErn;
        //                         message.status = Entity::EQS::MessageStatus::AVAILABLE;
        //                         message.receivedCount = 0;
        //                         message.receiptHandle.clear();
        //                         log_info << "Message moved to dead letter queue, ern: " << queueErn << ", dlqErn: " << deadLetterQueueErn << ", messageId: " << message.messageId;
        //                         continue;
        //                     }
        //
        //                     message.status = Entity::EQS::MessageStatus::INVISIBLE;
        //                     message.receiptHandle = Core::UuidUtils::CreateRandomUuid();
        //                     result.push_back(message);
        //                     taken += 1;
        //                 }
        //             }
        //
        //             if (movedCount > 0) {
        //                 for (auto &queue: _queueStore | std::views::values) {
        //                     if (queue.ern == queueErn) {
        //                         queue.available -= movedCount;
        //                         queue.size -= movedSize;
        //                         queue.modified = std::chrono::system_clock::now();
        //                     } else if (queue.ern == deadLetterQueueErn) {
        //                         queue.available += movedCount;
        //                         queue.size += movedSize;
        //                         queue.modified = std::chrono::system_clock::now();
        //                     }
        //                 }
        //             }
        //
        //             if (!result.empty()) {
        //                 for (auto &queue: _queueStore | std::views::values) {
        //                     if (queue.ern == queueErn) {
        //                         queue.invisible += static_cast<long>(result.size());
        //                         queue.modified = std::chrono::system_clock::now();
        //                         break;
        //                     }
        //                 }
        //                 return result;
        //             }
        //         }
        //
        //         if (waitTime <= 0 || std::chrono::steady_clock::now() >= deadline) {
        //             return {};
        //         }
        //         std::this_thread::sleep_for(std::chrono::milliseconds(100));
        //     }
        // }
        //
        // void deleteMessage(const std::string &receiptHandle) override {
        //     std::lock_guard lock(_mutex);
        //
        //     const auto it = std::ranges::find_if(_messageStore, [&receiptHandle](const auto &kv) {
        //         return kv.second.receiptHandle == receiptHandle;
        //     });
        //     if (it == _messageStore.end()) {
        //         return;
        //     }
        //     const auto &message = it->second;
        //
        //     for (auto &queue: _queueStore | std::views::values) {
        //         if (queue.ern == message.queueErn) {
        //             queue.size -= message.size;
        //             if (message.status == Entity::EQS::MessageStatus::AVAILABLE) {
        //                 queue.available -= 1;
        //             } else if (message.status == Entity::EQS::MessageStatus::DELAYED) {
        //                 queue.delayed -= 1;
        //             } else if (message.status == Entity::EQS::MessageStatus::INVISIBLE) {
        //                 queue.invisible -= 1;
        //             }
        //             queue.modified = std::chrono::system_clock::now();
        //             break;
        //         }
        //     }
        //
        //     _messageStore.erase(it);
        // }

        void purgeTopic(const std::string &topicErn) override {
            std::lock_guard lock(_mutex);
            std::erase_if(_messageStore, [&topicErn](const auto &kv) {
                return kv.second.topicErn == topicErn;
            });

            for (auto &queue: _topicStore | std::views::values) {
                if (queue.ern == topicErn) {
                    queue.size = 0;
                    queue.available = 0;
                    queue.modified = std::chrono::system_clock::now();
                    break;
                }
            }
        }

        void purgeAllTopics(const std::string &region, const std::string &accountId, const std::string &nameSpace) override {
            std::lock_guard lock(_mutex);
            const std::string marker = ":" + region + ":" + accountId + ":";

            std::set<std::string> ernsToPurge;
            for (auto &queue: _topicStore | std::views::values) {
                if (queue.ern.find(marker) == std::string::npos) {
                    continue;
                }
                ernsToPurge.insert(queue.ern);
                queue.size = 0;
                queue.available = 0;
                queue.modified = std::chrono::system_clock::now();
            }

            std::erase_if(_messageStore, [&ernsToPurge](const auto &kv) {
                return ernsToPurge.contains(kv.second.topicErn);
            });
        }

        //
        // std::optional<Entity::EQS::Message> findMessageByName(const std::string &name) const override {
        //     std::lock_guard lock(_mutex);
        //     const auto it = _messageStore.find(name);
        //     if (it == _messageStore.end()) return std::nullopt;
        //     return it->second;
        // }
        //
        // std::optional<Entity::EQS::Message> findMessageById(const std::string &id) const override {
        //     std::lock_guard lock(_mutex);
        //     for (const auto &m: _messageStore | std::views::values) {
        //         if (m.oid == id) return m;
        //     }
        //     return std::nullopt;
        // }
        //
        // std::vector<Entity::EQS::Message> findAllMessages() const override {
        //     std::lock_guard lock(_mutex);
        //     std::vector<Entity::EQS::Message> result;
        //     result.reserve(_messageStore.size());
        //     for (const auto &m: _messageStore | std::views::values) result.push_back(m);
        //     return result;
        // }

        std::vector<Entity::ENS::Message> listMessages(const std::string &topicErn, const long pageSize, const long pageIndex, const std::string &sortColumn) const override {
            std::lock_guard lock(_mutex);
            std::vector<Entity::ENS::Message> result;
            for (const auto &m: _messageStore | std::views::values) {
                if (m.topicErn == topicErn) {
                    result.push_back(m);
                }
            }

            if (sortColumn == "created") {
                std::ranges::sort(result, {}, &Entity::ENS::Message::created);
            } else if (sortColumn == "size") {
                std::ranges::sort(result, {}, &Entity::ENS::Message::size);
            } else if (sortColumn == "messageId") {
                std::ranges::sort(result, {}, &Entity::ENS::Message::messageId);
            }

            if (pageSize > 0) {
                const auto offset = std::min<size_t>(std::max<long>(pageIndex, 0) * pageSize, result.size());
                const auto end = std::min<size_t>(offset + pageSize, result.size());
                result = std::vector(result.begin() + static_cast<long>(offset), result.begin() + static_cast<long>(end));
            }
            return result;
        }

        //
        // bool messageExists(const std::string &name) const override {
        //     std::lock_guard lock(_mutex);
        //     return _messageStore.contains(name);
        // }

        long countMessages() const override {
            std::lock_guard lock(_mutex);
            return static_cast<long>(_messageStore.size());
        }

        long countMessages(const std::string &topicErn) const override {
            std::lock_guard lock(_mutex);
            return std::ranges::count_if(_messageStore | std::views::values, [&topicErn](const auto &message) {
                return message.topicErn == topicErn;
            });
        }

        //
        // void clearMessages() override {
        //     std::lock_guard lock(_mutex);
        //     _messageStore.clear();
        // }
        //
        // long resetExpiredMessages() override {
        //     std::lock_guard lock(_mutex);
        //
        //     const auto now = std::chrono::system_clock::now();
        //     std::map<std::string, long> resetCountByQueue;
        //     std::map<std::string, long> delayedResetCountByQueue;
        //
        //     for (auto &message: _messageStore | std::views::values) {
        //         if (message.status == Entity::EQS::MessageStatus::INVISIBLE) {
        //             if (now < message.lastReceived + std::chrono::seconds(message.visibilityTimeout)) continue;
        //
        //             message.status = Entity::EQS::MessageStatus::AVAILABLE;
        //             message.lastReceived = std::chrono::system_clock::time_point{};
        //             message.receiptHandle.clear();
        //             resetCountByQueue[message.queueErn]++;
        //             log_debug << "Message visibility timeout expired, messageId: " << message.messageId << ", queueErn: " << message.queueErn;
        //         } else if (message.status == Entity::EQS::MessageStatus::DELAYED) {
        //             if (now < message.delayUntil) continue;
        //
        //             message.status = Entity::EQS::MessageStatus::AVAILABLE;
        //             delayedResetCountByQueue[message.queueErn]++;
        //             log_debug << "Message delay expired, messageId: " << message.messageId << ", queueErn: " << message.queueErn;
        //         }
        //     }
        //
        //     long resetCount = 0;
        //     for (const auto &[queueErn, count]: resetCountByQueue) {
        //         resetCount += count;
        //         for (auto &queue: _queueStore | std::views::values) {
        //             if (queue.ern == queueErn) {
        //                 queue.invisible -= count;
        //                 queue.available += count;
        //                 queue.modified = now;
        //                 break;
        //             }
        //         }
        //     }
        //
        //     for (const auto &[queueErn, count]: delayedResetCountByQueue) {
        //         resetCount += count;
        //         for (auto &queue: _queueStore | std::views::values) {
        //             if (queue.ern == queueErn) {
        //                 queue.delayed -= count;
        //                 queue.available += count;
        //                 queue.modified = now;
        //                 break;
        //             }
        //         }
        //     }
        //
        //     if (resetCount > 0)
        //         log_info << "Reset expired messages, count: " << resetCount;
        //     return resetCount;
        // }

    private:

        mutable std::mutex _mutex;
        std::unordered_map<std::string, Entity::ENS::Topic> _topicStore;
        std::unordered_map<std::string, Entity::ENS::Message> _messageStore;
    };

}// namespace Euclid::Database