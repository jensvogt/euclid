// C++ includes
#include <ranges>
#include <set>
#include <thread>

// Mongodb includes
#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <mongocxx/change_stream.hpp>
#include <mongocxx/options/change_stream.hpp>
#include <mongocxx/options/find_one_and_update.hpp>
#include <mongocxx/options/index.hpp>
#include <mongocxx/pipeline.hpp>

// Euclid includes
#include <euclid/core/LogStream.h>
#include <euclid/core/Scheduler.h>
#include <euclid/core/UuidUtils.h>
#include <euclid/database/Database.h>
#include <euclid/database/EventBus.h>

namespace Euclid::Database {

    using namespace bsoncxx::builder::basic;

    namespace {

        constexpr auto kPending = "PENDING";
        constexpr auto kClaimed = "CLAIMED";

    }// namespace

    void EventBus::ensureIndexes() {
        std::call_once(_indexesOnce, [this] {
            _instanceId = Core::UuidUtils::CreateRandomUuid();

            try {
                const auto entry = Database::instance().client();
                const auto db = (*entry)[Database::instance().databaseName()];

                mongocxx::options::index subscriptionOpts;
                subscriptionOpts.unique(true);
                db[SUBSCRIPTION_COLLECTION].create_index(make_document(kvp("moduleType", 1), kvp("eventType", 1)), subscriptionOpts);
                db[EVENT_COLLECTION].create_index(make_document(kvp("targetModule", 1), kvp("status", 1), kvp("visibleAt", 1)));

                // Only external envelopes carry expiresAt, so this expires an abandoned
                // consumer's backlog and leaves module deliveries - which have no such field -
                // untouched.
                mongocxx::options::index expiryOpts;
                expiryOpts.expire_after(std::chrono::seconds(0));
                db[EVENT_COLLECTION].create_index(make_document(kvp("expiresAt", 1)), expiryOpts);

            } catch (const std::exception &e) {
                log_error << "Ensure EventBus indexes failed, error: " << e.what();
            }
        });
    }

    void EventBus::Subscribe(const std::string &moduleType, const std::string &eventType, std::function<bool(const EventEnvelope &)> handler) {

        ensureIndexes();

        {
            std::lock_guard lock(_handlersMutex);
            _handlers[eventType] = std::move(handler);
        }

        try {
            const auto filter = make_document(kvp("moduleType", moduleType), kvp("eventType", eventType));
            const auto update = make_document(kvp("$setOnInsert", make_document(kvp("moduleType", moduleType), kvp("eventType", eventType))));

            mongocxx::options::update opts;
            opts.upsert(true);

            const auto entry = Database::instance().client();
            auto collection = (*entry)[Database::instance().databaseName()][SUBSCRIPTION_COLLECTION];
            collection.update_one(filter.view(), update.view(), opts);

            log_info << "EventBus subscription registered, moduleType: " << moduleType << ", eventType: " << eventType;

        } catch (const std::exception &e) {
            log_error << "EventBus subscribe failed, moduleType: " << moduleType << ", eventType: " << eventType << ", error: " << e.what();
        }
    }

    void EventBus::Publish(const std::string &eventType, const boost::json::value &payload, const std::string &sourceModule) {

        ensureIndexes();

        try {
            const auto entry = Database::instance().client();
            const auto db = (*entry)[Database::instance().databaseName()];

            // A module type subscribes to everything of a type; an external subscriber may have
            // asked for a subset, so its filter is applied here rather than at delivery. Storing
            // an envelope only to drop it later would make a consumer's backlog depend on how
            // busy the whole installation is instead of on what it asked for.
            const auto &payloadObject = payload.is_object() ? payload.as_object() : boost::json::object{};

            const auto matchesFilter = [&payloadObject](const bsoncxx::document::view &subscription) {
                const auto element = subscription["filter"];
                if (!element || element.type() != bsoncxx::type::k_string) return true;
                const auto text = std::string(element.get_string().value);
                if (text.empty()) return true;
                try {
                    const auto parsed = boost::json::parse(text);
                    if (!parsed.is_object()) return true;
                    for (const auto &[key, expected]: parsed.as_object()) {
                        const auto *actual = payloadObject.if_contains(key);
                        if (actual == nullptr || *actual != expected) return false;
                    }
                } catch (const std::exception &) {
                    return true;
                }
                return true;
            };

            // Account scoping for external subscribers: an event whose payload names an account
            // is only ever stored for subscribers of that account. An event that names none is
            // delivered as before - which is why a module publishing user-visible events should
            // put accountId in the payload.
            const auto *eventAccount = payloadObject.if_contains("accountId");
            const auto matchesAccount = [&](const bsoncxx::document::view &subscription) {
                if (eventAccount == nullptr || !eventAccount->is_string()) return true;
                const auto element = subscription["accountId"];
                if (!element || element.type() != bsoncxx::type::k_string) return true;
                const auto subscriberAccount = std::string(element.get_string().value);
                return subscriberAccount.empty() || subscriberAccount == std::string(eventAccount->as_string().c_str());
            };

            std::set<std::string> targets;
            std::set<std::string> externalTargets;
            const auto filter = make_document(kvp("eventType", eventType));
            for (auto cursor = db[SUBSCRIPTION_COLLECTION].find(filter.view()); auto doc: cursor) {
                const auto target = std::string(doc["moduleType"].get_string().value);
                if (!target.starts_with(kExternalPrefix)) {
                    targets.insert(target);
                    continue;
                }
                if (matchesFilter(doc) && matchesAccount(doc)) externalTargets.insert(target);
            }

            if (targets.empty() && externalTargets.empty()) {
                log_debug << "EventBus publish, no subscribers, eventType: " << eventType;
                return;
            }

            const auto now = std::chrono::system_clock::now();
            const auto payloadJson = boost::json::serialize(payload);
            auto eventCollection = db[EVENT_COLLECTION];

            for (const auto &target: targets) {
                const auto doc = make_document(
                        kvp("eventId", Core::UuidUtils::CreateRandomUuid()),
                        kvp("eventType", eventType),
                        kvp("sourceModule", sourceModule),
                        kvp("targetModule", target),
                        kvp("payload", payloadJson),
                        kvp("status", kPending),
                        kvp("claimedBy", ""),
                        kvp("attempts", static_cast<int64_t>(0)),
                        kvp("visibleAt", bsoncxx::types::b_date{now}),
                        kvp("createdAt", bsoncxx::types::b_date{now}));
                eventCollection.insert_one(doc.view());
            }

            for (const auto &target: externalTargets) {
                const auto doc = make_document(
                        kvp("eventId", Core::UuidUtils::CreateRandomUuid()),
                        kvp("eventType", eventType),
                        kvp("sourceModule", sourceModule),
                        kvp("targetModule", target),
                        kvp("payload", payloadJson),
                        kvp("status", kPending),
                        kvp("claimedBy", ""),
                        kvp("attempts", static_cast<int64_t>(0)),
                        kvp("visibleAt", bsoncxx::types::b_date{now}),
                        kvp("createdAt", bsoncxx::types::b_date{now}),
                        kvp("expiresAt", bsoncxx::types::b_date{now + std::chrono::seconds(kExternalRetentionSeconds)}));
                eventCollection.insert_one(doc.view());
            }

            log_info << "EventBus published, eventType: " << eventType << ", modules: " << targets.size()
                    << ", subscribers: " << externalTargets.size();

        } catch (const std::exception &e) {
            log_error << "EventBus publish failed, eventType: " << eventType << ", error: " << e.what();
        }
    }

    // ── External subscribers ─────────────────────────────────────────────────
    //
    // Everything below works on the same two collections as the module path, with the subscriber
    // name standing where a module type stands - see kExternalPrefix.

    namespace {
        std::string externalTarget(const std::string &subscriber) {
            return std::string(EventBus::kExternalPrefixValue()) + subscriber;
        }
    }// namespace

    void EventBus::SubscribeExternal(const std::string &subscriber, const std::string &eventType,
                                     const boost::json::object &filter, const std::string &accountId) {

        ensureIndexes();

        try {
            const auto target = externalTarget(subscriber);
            const auto now = std::chrono::system_clock::now();

            // Upserted, so re-subscribing with a different filter changes the subscription rather
            // than failing or quietly creating a second one - an application that redeploys with
            // a narrower filter should end up narrower.
            const auto selector = make_document(kvp("moduleType", target), kvp("eventType", eventType));
            const auto update = make_document(
                    kvp("$set", make_document(
                                kvp("filter", boost::json::serialize(filter)),
                                kvp("accountId", accountId),
                                kvp("external", true),
                                kvp("lastSeenAt", bsoncxx::types::b_date{now}))),
                    kvp("$setOnInsert", make_document(
                                kvp("moduleType", target),
                                kvp("eventType", eventType),
                                kvp("createdAt", bsoncxx::types::b_date{now}))));

            mongocxx::options::update opts;
            opts.upsert(true);

            const auto entry = Database::instance().client();
            (*entry)[Database::instance().databaseName()][SUBSCRIPTION_COLLECTION].update_one(selector.view(), update.view(), opts);

            log_info << "EventBus external subscription registered, subscriber: " << subscriber << ", eventType: " << eventType;

        } catch (const std::exception &e) {
            log_error << "EventBus external subscribe failed, subscriber: " << subscriber << ", error: " << e.what();
        }
    }

    long EventBus::UnsubscribeExternal(const std::string &subscriber, const std::string &eventType) {

        ensureIndexes();

        try {
            const auto target = externalTarget(subscriber);
            const auto entry = Database::instance().client();
            const auto db = (*entry)[Database::instance().databaseName()];

            bsoncxx::builder::basic::document selector;
            selector.append(kvp("moduleType", target));
            if (!eventType.empty()) selector.append(kvp("eventType", eventType));

            const auto removed = db[SUBSCRIPTION_COLLECTION].delete_many(selector.view());

            // The events go with the subscription: keeping a backlog for something nobody is
            // listening to any more is what the retention index exists to prevent, and doing it
            // now is better than waiting a week for it.
            bsoncxx::builder::basic::document eventSelector;
            eventSelector.append(kvp("targetModule", target));
            if (!eventType.empty()) eventSelector.append(kvp("eventType", eventType));
            db[EVENT_COLLECTION].delete_many(eventSelector.view());

            const auto count = removed ? static_cast<long>(removed->deleted_count()) : 0;
            log_info << "EventBus external subscription removed, subscriber: " << subscriber << ", count: " << count;
            return count;

        } catch (const std::exception &e) {
            log_error << "EventBus external unsubscribe failed, subscriber: " << subscriber << ", error: " << e.what();
            return 0;
        }
    }

    std::vector<EventBus::Subscription> EventBus::ListSubscriptions(const std::string &subscriber) const {

        std::vector<Subscription> result;
        try {
            const auto entry = Database::instance().client();
            const auto db = (*entry)[Database::instance().databaseName()];

            for (const auto selector = make_document(kvp("moduleType", externalTarget(subscriber)));
                 auto doc: db[SUBSCRIPTION_COLLECTION].find(selector.view())) {

                Subscription subscription;
                subscription.subscriber = subscriber;
                subscription.eventType = std::string(doc["eventType"].get_string().value);
                subscription.accountId = doc["accountId"] && doc["accountId"].type() == bsoncxx::type::k_string
                                                 ? std::string(doc["accountId"].get_string().value)
                                                 : "";
                if (doc["filter"] && doc["filter"].type() == bsoncxx::type::k_string) {
                    try {
                        if (const auto parsed = boost::json::parse(std::string(doc["filter"].get_string().value)); parsed.is_object()) {
                            subscription.filter = parsed.as_object();
                        }
                    } catch (const std::exception &) {
                        // A filter that cannot be parsed is reported as none rather than failing
                        // the listing.
                    }
                }
                if (doc["createdAt"]) subscription.createdAt = system_clock::time_point{doc["createdAt"].get_date().value};
                if (doc["lastSeenAt"]) subscription.lastSeenAt = system_clock::time_point{doc["lastSeenAt"].get_date().value};
                result.push_back(std::move(subscription));
            }

        } catch (const std::exception &e) {
            log_error << "EventBus list subscriptions failed, subscriber: " << subscriber << ", error: " << e.what();
        }
        return result;
    }

    std::vector<EventEnvelope> EventBus::ClaimEvents(const std::string &subscriber, const long maxEvents, const std::chrono::seconds visibility) {

        ensureIndexes();

        std::vector<EventEnvelope> claimed;
        try {
            const auto target = externalTarget(subscriber);
            const auto now = std::chrono::system_clock::now();

            const auto entry = Database::instance().client();
            const auto db = (*entry)[Database::instance().databaseName()];
            auto eventCollection = db[EVENT_COLLECTION];

            // The same atomic claim the module path makes, one event at a time: find-and-update
            // is what makes two instances of one subscriber safe to run at once.
            const auto selector = make_document(
                    kvp("targetModule", target),
                    kvp("$or", bsoncxx::builder::basic::make_array(
                                make_document(kvp("status", kPending)),
                                make_document(kvp("status", kClaimed), kvp("visibleAt", make_document(kvp("$lte", bsoncxx::types::b_date{now})))))));
            const auto update = make_document(
                    kvp("$set", make_document(
                                kvp("status", kClaimed),
                                kvp("claimedBy", subscriber),
                                kvp("visibleAt", bsoncxx::types::b_date{now + visibility}))),
                    kvp("$inc", make_document(kvp("attempts", static_cast<int64_t>(1)))));

            mongocxx::options::find_one_and_update opts;
            opts.sort(make_document(kvp("createdAt", 1)));
            opts.return_document(mongocxx::options::return_document::k_after);

            for (long i = 0; i < std::max(1L, maxEvents); ++i) {
                const auto result = eventCollection.find_one_and_update(selector.view(), update.view(), opts);
                if (!result) break;

                const auto view = result->view();
                EventEnvelope envelope;
                envelope.eventId = std::string(view["eventId"].get_string().value);
                envelope.eventType = std::string(view["eventType"].get_string().value);
                envelope.sourceModule = std::string(view["sourceModule"].get_string().value);
                envelope.attempts = static_cast<long>(view["attempts"].get_int64().value);
                envelope.createdAt = system_clock::time_point{view["createdAt"].get_date().value};
                try {
                    envelope.payload = boost::json::parse(std::string(view["payload"].get_string().value));
                } catch (const std::exception &) {
                    envelope.payload = boost::json::value{};
                }
                claimed.push_back(std::move(envelope));
            }

            if (!claimed.empty()) {
                db[SUBSCRIPTION_COLLECTION].update_many(
                        make_document(kvp("moduleType", target)).view(),
                        make_document(kvp("$set", make_document(kvp("lastSeenAt", bsoncxx::types::b_date{now})))).view());
            }

        } catch (const std::exception &e) {
            log_error << "EventBus claim failed, subscriber: " << subscriber << ", error: " << e.what();
        }
        return claimed;
    }

    bool EventBus::AckEvent(const std::string &subscriber, const std::string &eventId) {

        try {
            const auto entry = Database::instance().client();
            const auto result = (*entry)[Database::instance().databaseName()][EVENT_COLLECTION].delete_one(
                    make_document(kvp("targetModule", externalTarget(subscriber)), kvp("eventId", eventId)).view());
            return result && result->deleted_count() > 0;

        } catch (const std::exception &e) {
            log_error << "EventBus ack failed, subscriber: " << subscriber << ", eventId: " << eventId << ", error: " << e.what();
            return false;
        }
    }

    long EventBus::CountEvents(const std::string &subscriber) const {

        try {
            const auto entry = Database::instance().client();
            return static_cast<long>((*entry)[Database::instance().databaseName()][EVENT_COLLECTION].count_documents(
                    make_document(kvp("targetModule", externalTarget(subscriber))).view()));

        } catch (const std::exception &e) {
            log_error << "EventBus count failed, subscriber: " << subscriber << ", error: " << e.what();
            return 0;
        }
    }

    // A module's subscriptions are the ones it registered on this startup, and nothing else. They
    // outlive the process that wrote them, so an event type a module used to handle and no longer
    // does would otherwise keep being fanned out to it forever - deliveries nothing will ever
    // claim, and after an event type is renamed, deliveries in a shape the module can no longer
    // read. Removing them here means a rename needs no migration: the old name stops being
    // subscribed the first time the new binary starts.
    void EventBus::pruneStaleSubscriptions(const std::string &moduleType) {

        std::vector<std::string> handled;
        {
            std::lock_guard lock(_handlersMutex);
            handled.reserve(_handlers.size());
            for (const auto &eventType: _handlers | std::views::keys) handled.push_back(eventType);
        }

        // A process that registered no handler at all says nothing about what it wants, and must
        // not be read as saying it wants nothing.
        if (handled.empty()) return;

        try {
            bsoncxx::builder::basic::array current;
            for (const auto &eventType: handled) current.append(eventType);

            const auto selector = make_document(
                    kvp("moduleType", moduleType),
                    kvp("eventType", make_document(kvp("$nin", current))));

            const auto entry = Database::instance().client();
            if (const auto removed = (*entry)[Database::instance().databaseName()][SUBSCRIPTION_COLLECTION].delete_many(selector.view());
                removed && removed->deleted_count() > 0) {
                log_info << "EventBus removed stale subscriptions, moduleType: " << moduleType << ", count: " << removed->deleted_count();
            }

        } catch (const std::exception &e) {
            log_error << "EventBus prune failed, moduleType: " << moduleType << ", error: " << e.what();
        }
    }

    void EventBus::Start(const std::string &moduleType, const std::chrono::milliseconds pollInterval) {

        ensureIndexes();
        pruneStaleSubscriptions(moduleType);

        auto &scheduler = Core::Scheduler::instance();
        scheduler.Start();
        scheduler.SchedulePeriodic("eventbus-poll-" + moduleType, [this, moduleType] { pollOnce(moduleType); }, pollInterval);
        scheduler.SchedulePeriodic("eventbus-reap", [this] { reap(); }, std::chrono::seconds(10));

        std::thread([this, moduleType] { watchLoop(moduleType); }).detach();

        log_info << "EventBus started, moduleType: " << moduleType << ", instanceId: " << _instanceId;
    }

    void EventBus::watchLoop(const std::string &moduleType) {

        while (true) {
            try {
                const auto entry = Database::instance().client();
                auto eventCollection = (*entry)[Database::instance().databaseName()][EVENT_COLLECTION];

                // Server-side filter: only inserts targeting this moduleType wake this watcher up,
                // so many module types can share the collection without waking each other for
                // irrelevant traffic. Deliberately insert-only (not update) - reap()'s status reset
                // is covered by the next safety-net sweep instead, which keeps this filter simple
                // and avoids needing full_document(updateLookup) for update events.
                mongocxx::pipeline pipeline;
                pipeline.match(make_document(
                        kvp("operationType", "insert"),
                        kvp("fullDocument.targetModule", moduleType)));

                mongocxx::options::change_stream options;
                options.max_await_time(std::chrono::seconds(30));

                auto stream = eventCollection.watch(pipeline, options);
                log_info << "EventBus change stream opened, moduleType: " << moduleType;

                // Catches anything inserted in the gap between a previous stream dying (or this
                // being the first connection ever) and this watch() call taking effect.
                pollOnce(moduleType);

                // begin() == end() just means "no notification within max_await_time" - it does
                // NOT mean the stream died, so the fix is to keep re-checking the *same* stream
                // (calling begin() again), not to tear it down and reconnect from scratch. Only a
                // thrown exception (an actual network/cursor error) falls through to the reconnect
                // below. Runs until that happens - this loop has no other exit.
                for (auto it = stream.begin();; it = stream.begin()) {
                    for (; it != stream.end(); ++it) {
                        pollOnce(moduleType);// pure wake-up signal - the claim re-reads status from the DB itself
                    }
                }

            } catch (const std::exception &e) {
                log_error << "EventBus change stream failed, moduleType: " << moduleType << ", error: " << e.what() << ", reconnecting";
            }

            std::this_thread::sleep_for(kWatchReconnectDelay);
        }
    }

    void EventBus::pollOnce(const std::string &moduleType) {

        try {
            const auto entry = Database::instance().client();
            auto eventCollection = (*entry)[Database::instance().databaseName()][EVENT_COLLECTION];

            for (int claimed = 0; claimed < kBatchSize; ++claimed) {

                const auto now = std::chrono::system_clock::now();
                const auto filter = make_document(kvp("targetModule", moduleType), kvp("status", kPending));
                const auto update = make_document(
                        kvp("$set", make_document(
                                    kvp("status", kClaimed),
                                    kvp("claimedBy", _instanceId),
                                    kvp("visibleAt", bsoncxx::types::b_date{now + kVisibilityTimeout}))),
                        kvp("$inc", make_document(kvp("attempts", static_cast<int64_t>(1)))));

                const auto sort = make_document(kvp("createdAt", 1));
                mongocxx::options::find_one_and_update opts;
                opts.sort(sort.view());
                opts.return_document(mongocxx::options::return_document::k_after);

                const auto result = eventCollection.find_one_and_update(filter.view(), update.view(), opts);
                if (!result) break;// nothing PENDING left this tick

                const auto view = result->view();
                EventEnvelope envelope;
                envelope.eventId = std::string(view["eventId"].get_string().value);
                envelope.eventType = std::string(view["eventType"].get_string().value);
                envelope.sourceModule = std::string(view["sourceModule"].get_string().value);
                envelope.attempts = view["attempts"].get_int64().value;
                envelope.createdAt = system_clock::time_point{view["createdAt"].get_date().value};
                try {
                    envelope.payload = boost::json::parse(std::string(view["payload"].get_string().value));
                } catch (const std::exception &e) {
                    log_error << "EventBus payload parse failed, eventId: " << envelope.eventId << ", error: " << e.what();
                }

                std::function<bool(const EventEnvelope &)> handler;
                {
                    std::lock_guard lock(_handlersMutex);
                    if (const auto it = _handlers.find(envelope.eventType); it != _handlers.end()) handler = it->second;
                }

                bool ok = false;
                if (!handler) {
                    log_warning << "EventBus no local handler for eventType: " << envelope.eventType << ", acking to avoid poison delivery";
                    ok = true;
                } else {
                    try {
                        ok = handler(envelope);
                    } catch (const std::exception &e) {
                        log_error << "EventBus handler threw, eventType: " << envelope.eventType << ", error: " << e.what();
                        ok = false;
                    }
                }

                if (ok) {
                    eventCollection.delete_one(make_document(kvp("_id", view["_id"].get_oid())).view());
                } else if (envelope.attempts >= kMaxAttempts) {
                    moveToDlq(view, "max attempts exceeded");
                } else {
                    eventCollection.update_one(
                            make_document(kvp("_id", view["_id"].get_oid())).view(),
                            make_document(kvp("$set", make_document(kvp("status", kPending), kvp("visibleAt", bsoncxx::types::b_date{now})))).view());
                }
            }

        } catch (const std::exception &e) {
            log_error << "EventBus poll failed, moduleType: " << moduleType << ", error: " << e.what();
        }
    }

    void EventBus::reap() {

        try {
            const auto now = std::chrono::system_clock::now();
            const auto entry = Database::instance().client();
            auto eventCollection = (*entry)[Database::instance().databaseName()][EVENT_COLLECTION];

            const auto filter = make_document(kvp("status", kClaimed), kvp("visibleAt", make_document(kvp("$lte", bsoncxx::types::b_date{now}))));
            const auto update = make_document(kvp("$set", make_document(kvp("status", kPending))));

            if (const auto result = eventCollection.update_many(filter.view(), update.view()); result && result->modified_count() > 0) {
                log_warning << "EventBus reaped stuck deliveries, count: " << result->modified_count();
            }

        } catch (const std::exception &e) {
            log_error << "EventBus reap failed, error: " << e.what();
        }
    }

    void EventBus::moveToDlq(const bsoncxx::document::view &doc, const std::string &reason) {

        try {
            const auto entry = Database::instance().client();
            const auto db = (*entry)[Database::instance().databaseName()];

            document dlqDoc;
            for (const auto &field: doc) dlqDoc.append(kvp(field.key(), field.get_value()));
            dlqDoc.append(kvp("dlqReason", reason));

            db[DLQ_COLLECTION].insert_one(dlqDoc.view());
            db[EVENT_COLLECTION].delete_one(make_document(kvp("_id", doc["_id"].get_oid())).view());

            log_error << "EventBus moved delivery to DLQ, eventId: " << std::string(doc["eventId"].get_string().value) << ", reason: " << reason;

        } catch (const std::exception &e) {
            log_error << "EventBus moveToDlq failed, error: " << e.what();
        }
    }

}// namespace Euclid::Database