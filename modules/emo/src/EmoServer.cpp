// C++ includes
#include <chrono>
#include <map>
#include <mutex>
#include <optional>

// Boost includes
#include <boost/beast/core.hpp>

// Euclid includes
#include <EmoServer.h>
#include <euclid/core/Configuration.h>
#include <euclid/core/DateTimeUtils.h>
#include <euclid/core/SystemUtils.h>
#include <euclid/database/entity/eam/User.h>

namespace Euclid::Monitoring {

    namespace beast = boost::beast;
    namespace http = beast::http;

    using Database::Entity::Monitoring::AlignDown;
    using Database::Entity::Monitoring::MetricType;
    using Database::Entity::Monitoring::Resolution;
    using Database::Entity::Monitoring::ResolutionBucket;
    using Database::Entity::Monitoring::ResolutionFromString;

    // ── Helpers ──────────────────────────────────────────────────────────────

    namespace {
        // Looks up the caller identity resolved by EmoServer::Authenticate(), by user ID.
        struct AuthResult {
            std::optional<Database::Entity::EAM::User> user;
            bool tokenExpired{false};
            std::string denialReason;
        };

        constexpr auto kPrunePeriod = std::chrono::hours(24);
        constexpr auto kCpuUsagePeriod = std::chrono::seconds(60);
        // How stale a queue's message counts may get. Short enough that a queue draining is
        // visibly draining, long enough that the scan is rare next to the traffic it replaces
        // (one grouped scan against a write per message on the queue document).
        constexpr auto kQueueCountPeriod = std::chrono::seconds(15);
        // How stale a queue's message counts may get. Short enough that a queue draining is
        // visibly draining, long enough that the scan is rare next to the traffic it replaces
        // (one grouped scan against a write per message on the queue document).
        constexpr auto kBucketCountPeriod = std::chrono::seconds(15);

        constexpr auto kTopicCountPeriod = std::chrono::seconds(15);

        // Rollups run several times per target bucket rather than once, so the bucket currently in
        // progress is always readable, just incomplete. Cheap because each pass only reads the tier
        // above over a short window, and safe because rollups replace buckets instead of appending.
        constexpr auto kHourRollupPeriod = std::chrono::minutes(10);
        constexpr auto kDayRollupPeriod = std::chrono::hours(1);

        // Default retention per tier, in days. Roughly 2000 rows per series in total, against the
        // ~525,000 five-minute rows five years of undownsampled history would cost.
        constexpr long kRawRetentionDays = 7;
        constexpr long kHourRetentionDays = 90;
        constexpr long kDayRetentionDays = 1825;

        // Previous Core::SystemUtils::ReadCpuTimes() reading, so collectCpuUsage() can compute a
        // delta-based percentage rather than a since-boot average. There's only ever one
        // EmoServer per process, same reasoning as the accumulators map above.
        std::mutex &cpuTimesMutex() {
            static std::mutex mutex;
            return mutex;
        }

        std::optional<Core::SystemUtils::CpuTimes> &previousCpuTimes() {
            static std::optional<Core::SystemUtils::CpuTimes> previous;
            return previous;
        }

        std::string accumulatorKey(const std::string &name, const std::string &labelName, const std::string &labelValue) {
            return name + ":" + labelName + ":" + labelValue;
        }

        // Running aggregate of the samples taken for one "name:labelName:labelValue" key during the
        // current averaging period. Fed by handlePushMetrics() (one process pushes one batch per
        // its own push tick) and by collectCpuUsage(), drained by EmoServer::flush(). There's only
        // ever one EmoServer per process, so file-local state (rather than instance members reached
        // through the free-function dispatch table) is simplest.
        struct Accumulator {
            double sum = 0;
            double minValue = 0;
            double maxValue = 0;
            long samples = 0;
            MetricType type = MetricType::GAUGE;
        };

        std::mutex &accumulatorsMutex() {
            static std::mutex mutex;
            return mutex;
        }

        std::map<std::string, Accumulator> &accumulators() {
            static std::map<std::string, Accumulator> accumulators;
            return accumulators;
        }

        // Single entry point for every sample, whichever side it arrives from, so that all of them
        // land in the same bucket-aligned rows that the rollups can then aggregate.
        void recordSample(const std::string &name, const std::string &labelName, const std::string &labelValue,
                          const double value, const MetricType type) {

            std::lock_guard lock(accumulatorsMutex());
            auto &acc = accumulators()[accumulatorKey(name, labelName, labelValue)];
            if (acc.samples == 0) {
                acc.minValue = value;
                acc.maxValue = value;
            } else {
                acc.minValue = std::min(acc.minValue, value);
                acc.maxValue = std::max(acc.maxValue, value);
            }
            acc.sum += value;
            acc.samples++;
            acc.type = type;
        }

        std::chrono::seconds retentionOf(const Resolution resolution) {
            const auto &configuration = Core::Configuration::instance();
            switch (resolution) {
                case Resolution::HOUR:
                    return std::chrono::hours(24 * configuration.getOr<long>("euclid.module.emo.retention.hour", kHourRetentionDays));
                case Resolution::DAY:
                    return std::chrono::hours(24 * configuration.getOr<long>("euclid.module.emo.retention.day", kDayRetentionDays));
                default:
                    return std::chrono::hours(24 * configuration.getOr<long>("euclid.module.emo.retention.raw", kRawRetentionDays));
            }
        }

        std::chrono::seconds averagePeriod() {
            return std::chrono::seconds(Core::Configuration::instance().getOr<long>("euclid.module.emo.average-period", 300));
        }
    }// namespace

    static AuthResult authenticate(const request<string_body> &req) {
        const auto auth = EmoServer::Authenticate(req);
        if (!auth.subject.has_value()) {
            return {.user = std::nullopt, .tokenExpired = auth.tokenExpired, .denialReason = auth.denialReason};
        }
        return {.user = Database::RepositoryFactory::instance().eamRepository()->findUserByUserId(*auth.subject)};
    }

    static response<string_body> unauthorized(const request<string_body> &req, const AuthResult &auth) {
        return EmoServer::Unauthorized(req, {.subject = std::nullopt, .tokenExpired = auth.tokenExpired, .denialReason = auth.denialReason});
    }

    // Shared parsing of the name/label/resolution/range filters accepted by "list" and "average".
    static Database::MonitoringQuery parseQuery(const boost::json::value &jv, const long defaultLimit) {

        Database::MonitoringQuery query;
        query.limit = defaultLimit;
        if (!jv.is_object()) return query;

        const auto &obj = jv.as_object();
        if (const auto *v = obj.if_contains("name"); v && v->is_string()) query.name = v->as_string().c_str();
        if (const auto *v = obj.if_contains("labelName"); v && v->is_string()) query.labelName = v->as_string().c_str();
        if (const auto *v = obj.if_contains("labelValue"); v && v->is_string()) query.labelValue = v->as_string().c_str();
        if (const auto *v = obj.if_contains("limit"); v && v->is_number()) query.limit = v->to_number<long>();
        if (const auto *v = obj.if_contains("from"); v && v->is_string()) query.from = Core::DateTimeUtils::FromISO8601(v->as_string().c_str());
        if (const auto *v = obj.if_contains("to"); v && v->is_string()) query.to = Core::DateTimeUtils::FromISO8601(v->as_string().c_str());
        if (const auto *v = obj.if_contains("resolution"); v && v->is_string()) {
            if (const auto resolution = ResolutionFromString(v->as_string().c_str()); resolution != Resolution::UNKNOWN) {
                query.resolution = resolution;
            }
        }
        return query;
    }

    // ── "push-metrics" action handler ──────────────────────────────────────────
    // Receiving end of Core::Monitoring::MetricsPusher, which every metrics-producing module
    // (queues, access) runs to push its own Core::Monitoring::MonitoringCollector samples here on
    // its own schedule, instead of this module polling them - see EmoServer.h for why.
    // Unauthenticated: same trust boundary as "get-metrics" itself, a module-to-module call over
    // a Unix socket that only other local processes can reach.

    static response<string_body> handlePushMetrics(const request<string_body> &req) {

        boost::json::value jv;
        if (const auto err = EmoServer::ParseJsonBody(req, jv)) return *err;
        if (!jv.is_object()) return EmoServer::ErrorResponse(req, status::bad_request, "Expected a JSON object body");

        const auto &obj = jv.as_object();
        const auto *moduleName = obj.if_contains("module");
        const auto *items = obj.if_contains("items");
        if (!moduleName || !moduleName->is_string() || !items || !items->is_array()) {
            return EmoServer::ErrorResponse(req, status::bad_request, R"(Expected "module" and "items")");
        }

        for (const auto &v: items->as_array()) {
            if (!v.is_object()) continue;
            const auto &item = v.as_object();

            std::string name, labelName, labelValue;
            double value = 0;
            auto type = MetricType::GAUGE;
            if (const auto *p = item.if_contains("name"); p && p->is_string()) name = p->as_string().c_str();
            if (const auto *p = item.if_contains("labelName"); p && p->is_string()) labelName = p->as_string().c_str();
            if (const auto *p = item.if_contains("labelValue"); p && p->is_string()) labelValue = p->as_string().c_str();
            if (const auto *p = item.if_contains("value"); p && p->is_number()) value = p->to_number<double>();
            if (const auto *p = item.if_contains("type"); p && p->is_string()) type = p->as_string() == "rate" ? MetricType::RATE : MetricType::GAUGE;
            if (name.empty()) continue;

            recordSample(name, labelName, labelValue, value, type);
        }

        return EmoServer::JsonResponse(req, status::ok);
    }

    // ── "list" action handler ────────────────────────────────────────────────
    // Admin-only, same pattern as EamServer's handleListUsers - otherwise the data this module
    // writes could never be read back out.

    static response<string_body> handleList(const request<string_body> &req) {

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);
        if (!Database::IsEamAdmin(*Database::RepositoryFactory::instance().eamRepository(), auth.user->userId)) {
            return EmoServer::ErrorResponse(req, status::forbidden, "Administrator privileges required");
        }

        boost::json::value jv;
        if (const auto err = EmoServer::ParseJsonBody(req, jv)) return *err;

        const auto rows = Database::RepositoryFactory::instance().emoRepository()->list(parseQuery(jv, 100));

        boost::json::array items;
        for (const auto &row: rows) {
            items.push_back(boost::json::object{
                    {"name", row.name},
                    {"labelName", row.labelName},
                    {"labelValue", row.labelValue},
                    {"value", row.value},
                    {"minValue", row.minValue},
                    {"maxValue", row.maxValue},
                    {"samples", row.samples},
                    {"type", MetricTypeToString(row.type)},
                    {"resolution", ResolutionToString(row.resolution)},
                    {"timestamp", Core::DateTimeUtils::ToISO8601(row.timestamp)}
            });
        }

        return EmoServer::JsonResponse(req, status::ok, boost::json::serialize(boost::json::object{{"items", items}}));
    }

    // Return the sample-weighted average of one metric over the requested resolution and range,
    // not regarding labelName/labelValue.
    static response<string_body> handleAverage(const request<string_body> &req) {

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) return unauthorized(req, auth);
        if (!Database::IsEamAdmin(*Database::RepositoryFactory::instance().eamRepository(), auth.user->userId)) {
            return EmoServer::ErrorResponse(req, status::forbidden, "Administrator privileges required");
        }

        boost::json::value jv;
        if (const auto err = EmoServer::ParseJsonBody(req, jv)) return *err;

        const auto result = Database::RepositoryFactory::instance().emoRepository()->average(parseQuery(jv, 0));

        return EmoServer::JsonResponse(req, status::ok, boost::json::serialize(boost::json::object{{"average", result}}));
    }

    // ── Request dispatcher ───────────────────────────────────────────────────

    static response<string_body> dispatch(const request<string_body> &req) {

        const auto action = std::string(req["x-euclid-action"]);
        if (action.empty()) {
            return EmoServer::ErrorResponse(req, status::bad_request, "Missing x-euclid-action header");
        }
        log_debug << "Monitoring action=" << action;

        if (action == "list") return handleList(req);
        if (action == "average") return handleAverage(req);
        if (action == "push-metrics") return handlePushMetrics(req);

        return EmoServer::ErrorResponse(req, status::not_found, "Action not implemented: " + action);
    }

    // ── EmoServer ─────────────────────────────────────────────────────

    EmoServer::EmoServer(std::string socketPath, const int threads) : HttpActionServer("Monitoring", std::move(socketPath), threads) {

        auto &scheduler = Core::Scheduler::instance();
        scheduler.Start();

        _flushTaskId = scheduler.SchedulePeriodic("monitoring-flush", [] { flush(); },
                                                  std::chrono::duration_cast<std::chrono::milliseconds>(averagePeriod()));

        _pruneTaskId = scheduler.SchedulePeriodic("monitoring-prune", [] { prune(); },
                                                  std::chrono::duration_cast<std::chrono::milliseconds>(kPrunePeriod));

        // RAW is rolled into HOUR and HOUR into DAY - never RAW straight into DAY, so the daily
        // pass reads 24 rows per series rather than 288.
        const auto hourRollupPeriod = std::chrono::seconds(Core::Configuration::instance().getOr<long>("euclid.module.emo.rollup-hour-period", std::chrono::seconds(kHourRollupPeriod).count()));
        _hourRollupTaskId = scheduler.SchedulePeriodic("monitoring-rollup-hour", [] { rollup(Resolution::RAW, Resolution::HOUR); },
                                                       std::chrono::duration_cast<std::chrono::milliseconds>(hourRollupPeriod));

        const auto dayRollupPeriod = std::chrono::seconds(Core::Configuration::instance().getOr<long>("euclid.module.emo.rollup-day-period", std::chrono::seconds(kDayRollupPeriod).count()));
        _dayRollupTaskId = scheduler.SchedulePeriodic("monitoring-rollup-day", [] { rollup(Resolution::HOUR, Resolution::DAY); },
                                                      std::chrono::duration_cast<std::chrono::milliseconds>(dayRollupPeriod));

        // Recount mein counters
        const auto queueCountPeriod = std::chrono::seconds(Core::Configuration::instance().getOr<long>("euclid.module.emo.queue-count-period", kQueueCountPeriod.count()));
        _queueCountsTaskId = scheduler.SchedulePeriodic("monitoring-queue-counts",
                                                        [] { Database::RepositoryFactory::instance().eqsRepository()->recountQueues(); },
                                                        std::chrono::duration_cast<std::chrono::milliseconds>(queueCountPeriod));
        // Its own id, not _queueCountsTaskId: assigning both to one member left the queue task
        // unreferenced, so nothing could cancel it and the destructor cancelled the bucket task twice.
        const auto bucketCountPeriod = std::chrono::seconds(Core::Configuration::instance().getOr<long>("euclid.module.emo.bucket-count-period", kBucketCountPeriod.count()));
        _bucketCountsTaskId = scheduler.SchedulePeriodic("monitoring-bucket-counts",
                                                         [] { Database::RepositoryFactory::instance().esmRepository()->recountBuckets(); },
                                                         std::chrono::duration_cast<std::chrono::milliseconds>(bucketCountPeriod));
        const auto topicCountPeriod = std::chrono::seconds(Core::Configuration::instance().getOr<long>("euclid.module.emo.topic-count-period", kTopicCountPeriod.count()));
        _topicCountsTaskId = scheduler.SchedulePeriodic("monitoring-topic-counts",
                                                        [] { Database::RepositoryFactory::instance().ensRepository()->recountTopics(); },
                                                        std::chrono::duration_cast<std::chrono::milliseconds>(topicCountPeriod));

#ifdef __linux__
        // collectCpuUsage() reads /proc/stat, which only exists on Linux.
        const auto cpuUsagePeriod = std::chrono::seconds(Core::Configuration::instance().getOr<long>("euclid.module.emo.cpu-usage-period", kCpuUsagePeriod.count()));
        _cpuUsageTaskId = scheduler.SchedulePeriodic("monitoring-cpu-usage", [] { collectCpuUsage(); },
                                                     std::chrono::duration_cast<std::chrono::milliseconds>(cpuUsagePeriod));

        // Same period as the CPU figure, and the same reason: they are read together on a
        // dashboard and drifting apart in age would make them hard to compare.
        _memoryUsageTaskId = scheduler.SchedulePeriodic("monitoring-memory-usage", [] { collectMemoryUsage(); },
                                                        std::chrono::duration_cast<std::chrono::milliseconds>(cpuUsagePeriod));
#endif
    }

    EmoServer::~EmoServer() {
        auto &scheduler = Core::Scheduler::instance();
        scheduler.Cancel(_flushTaskId);
        scheduler.Cancel(_pruneTaskId);
        scheduler.Cancel(_hourRollupTaskId);
        scheduler.Cancel(_dayRollupTaskId);
        scheduler.Cancel(_queueCountsTaskId);
        scheduler.Cancel(_bucketCountsTaskId);
        scheduler.Cancel(_topicCountsTaskId);
        scheduler.Cancel(_cpuUsageTaskId);
    }

    void EmoServer::flush() {

        std::map<std::string, Accumulator> snapshot;
        {
            std::lock_guard lock(accumulatorsMutex());
            snapshot.swap(accumulators());
        }

        // The samples just drained cover the period that ended roughly now, so the bucket they
        // belong to is the one before this firing. Aligning half a period back rather than a whole
        // one lands in the right bucket whether the scheduler fires slightly early or slightly late.
        const auto period = averagePeriod();
        const auto bucket = AlignDown(std::chrono::system_clock::now() - period / 2, period);
        const auto expiresAt = bucket + retentionOf(Resolution::RAW);
        const auto repo = Database::RepositoryFactory::instance().emoRepository();

        for (const auto &[key, acc]: snapshot) {
            if (acc.samples <= 0) continue;

            // key is "name:labelName:labelValue" - names/labels are plain kebab-case strings, so
            // splitting on the first two colons is unambiguous.
            const auto firstColon = key.find(':');
            const auto secondColon = key.find(':', firstColon + 1);
            if (firstColon == std::string::npos || secondColon == std::string::npos) continue;

            Database::Entity::Monitoring::MonitoringData row;
            row.name = key.substr(0, firstColon);
            row.labelName = key.substr(firstColon + 1, secondColon - firstColon - 1);
            row.labelValue = key.substr(secondColon + 1);
            // Rate metrics: sum of per-tick, per-instance occurrence counts = total over the
            // period. Gauge metrics: mean across every tick and instance sampled.
            row.value = acc.type == MetricType::RATE ? acc.sum : acc.sum / static_cast<double>(acc.samples);
            row.minValue = acc.minValue;
            row.maxValue = acc.maxValue;
            row.samples = acc.samples;
            row.type = acc.type;
            row.resolution = Resolution::RAW;
            row.timestamp = bucket;
            row.expiresAt = expiresAt;
            repo->upsert(row);

            log_debug << "Monitoring flushed, name: " << row.name << ", labelName: " << row.labelName
                    << ", labelValue: " << row.labelValue << ", value: " << row.value;
        }
    }

    void EmoServer::rollup(const Resolution from, const Resolution to) {

        // Re-aggregate the bucket in progress plus the one before it, so a firing that was late,
        // or one that follows a restart, repairs the gap instead of leaving a hole. The rollup
        // replaces whole buckets, so overlapping windows cost a little work and change nothing.
        const auto now = std::chrono::system_clock::now();
        const auto windowStart = AlignDown(now, ResolutionBucket(to)) - ResolutionBucket(to);

        Database::RepositoryFactory::instance().emoRepository()->rollup(from, to, windowStart, now, retentionOf(to));
    }

    void EmoServer::prune() {
        // On MongoDB the TTL index on expiresAt has usually already removed these; this stays as
        // the portable path for backends without one, and as a backstop if the TTL monitor is off.
        Database::RepositoryFactory::instance().emoRepository()->deleteExpired(std::chrono::system_clock::now());
    }

    void EmoServer::collectCpuUsage() {

        const auto current = Core::SystemUtils::ReadCpuTimes();
        if (!current.has_value()) {
            log_warning << "Monitoring cpu-usage collection failed, /proc/stat not readable";
            return;
        }

        std::lock_guard lock(cpuTimesMutex());
        const auto previous = previousCpuTimes();
        previousCpuTimes() = current;

        // The first call only primes previousCpuTimes() - a percentage needs a delta between two
        // readings.
        if (!previous.has_value()) return;

        const auto totalDelta = current->total - previous->total;
        const auto idleDelta = current->idle - previous->idle;
        if (totalDelta == 0) return;

        // Recorded as a sample rather than written straight out, so CPU usage is bucket-aligned
        // and carries a sample count and a min/max like every other metric, and so the rollups
        // can aggregate it at all.
        const auto usage = 100.0 * static_cast<double>(totalDelta - idleDelta) / static_cast<double>(totalDelta);
        recordSample("system-cpu-usage", "host", Core::SystemUtils::GetHostName(), usage, MetricType::GAUGE);
    }

    void EmoServer::collectMemoryUsage() {

        // The machine's memory, labelled by host, beside the machine's CPU. The per-module
        // "euclid-memory-usage-percent" is a different question - what one process holds - and
        // summing or averaging those never answered this one.
        const auto usage = Core::SystemUtils::ReadSystemMemoryUsagePercent();
        if (!usage.has_value()) {
            log_warning << "Monitoring memory-usage collection failed, /proc/meminfo not readable";
            return;
        }

        recordSample("system-memory-usage-percent", "host", Core::SystemUtils::GetHostName(), *usage, MetricType::GAUGE);
    }

    response<string_body> EmoServer::Dispatch(const request<string_body> &req) {
        return dispatch(req);
    }

}// namespace Euclid::Monitoring