// C++ includes
#include <set>
#include <thread>

// Mongodb includes
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

            std::set<std::string> targets;
            const auto filter = make_document(kvp("eventType", eventType));
            for (auto cursor = db[SUBSCRIPTION_COLLECTION].find(filter.view()); auto doc: cursor) {
                targets.insert(std::string(doc["moduleType"].get_string().value));
            }

            if (targets.empty()) {
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

            log_info << "EventBus published, eventType: " << eventType << ", targets: " << targets.size();

        } catch (const std::exception &e) {
            log_error << "EventBus publish failed, eventType: " << eventType << ", error: " << e.what();
        }
    }

    void EventBus::Start(const std::string &moduleType, const std::chrono::milliseconds pollInterval) {

        ensureIndexes();

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