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

        // Running sum/count of samples taken for one "name:labelName:labelValue" key during the
        // current averaging period. Fed by handlePushMetrics() (one process pushes one batch per
        // its own push tick), drained by EmoServer::flush(). There's only ever one
        // EmoServer per process, so file-local state (rather than instance members reached
        // through the free-function dispatch table) is simplest.
        struct Accumulator {
            double sum = 0;
            long samples = 0;
            bool isRate = false;
        };

        std::mutex &accumulatorsMutex() {
            static std::mutex mutex;
            return mutex;
        }

        std::map<std::string, Accumulator> &accumulators() {
            static std::map<std::string, Accumulator> accumulators;
            return accumulators;
        }
    } // namespace

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

        std::lock_guard lock(accumulatorsMutex());
        for (const auto &v: items->as_array()) {
            if (!v.is_object()) continue;
            const auto &item = v.as_object();

            std::string name, labelName, labelValue;
            double value = 0;
            bool isRate = false;
            if (const auto *p = item.if_contains("name"); p && p->is_string()) name = p->as_string().c_str();
            if (const auto *p = item.if_contains("labelName"); p && p->is_string()) labelName = p->as_string().c_str();
            if (const auto *p = item.if_contains("labelValue"); p && p->is_string()) labelValue = p->as_string().c_str();
            if (const auto *p = item.if_contains("value"); p && p->is_number()) value = p->to_number<double>();
            if (const auto *p = item.if_contains("type"); p && p->is_string()) isRate = p->as_string() == "rate";
            if (name.empty()) continue;

            auto &acc = accumulators()[accumulatorKey(name, labelName, labelValue)];
            acc.sum += value;
            acc.samples++;
            acc.isRate = isRate;
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

        std::string name, labelName, labelValue;
        long limit = 100;
        if (jv.is_object()) {
            const auto &obj = jv.as_object();
            if (const auto *v = obj.if_contains("name"); v && v->is_string()) name = v->as_string().c_str();
            if (const auto *v = obj.if_contains("labelName"); v && v->is_string()) labelName = v->as_string().c_str();
            if (const auto *v = obj.if_contains("labelValue"); v && v->is_string()) labelValue = v->as_string().c_str();
            if (const auto *v = obj.if_contains("limit"); v && v->is_number()) limit = v->to_number<long>();
        }

        const auto rows = Database::RepositoryFactory::instance().emoRepository()->list(name, labelName, labelValue, limit);

        boost::json::array items;
        for (const auto &row: rows) {
            items.push_back(boost::json::object{
                {"name", row.name},
                {"labelName", row.labelName},
                {"labelValue", row.labelValue},
                {"value", row.value},
                {"timestamp", Core::DateTimeUtils::ToISO8601(row.timestamp)}
            });
        }

        return EmoServer::JsonResponse(req, status::ok, boost::json::serialize(boost::json::object{{"items", items}}));
    }

    // ── Request dispatcher ───────────────────────────────────────────────────

    static response<string_body> dispatch(const request<string_body> &req) {

        const auto action = std::string(req["x-euclid-action"]);
        if (action.empty()) {
            return EmoServer::ErrorResponse(req, status::bad_request, "Missing x-euclid-action header");
        }
        log_debug << "Monitoring action=" << action;

        if (action == "list") return handleList(req);
        if (action == "push-metrics") return handlePushMetrics(req);

        return EmoServer::ErrorResponse(req, status::not_found, "Action not implemented: " + action);
    }

    // ── EmoServer ─────────────────────────────────────────────────────

    EmoServer::EmoServer(std::string socketPath, const int threads) : HttpActionServer("Monitoring", std::move(socketPath), threads) {

        auto &scheduler = Core::Scheduler::instance();
        scheduler.Start();

        const auto averagePeriod = std::chrono::seconds(Core::Configuration::instance().getOr<long>("euclid.monitoring.average-period", 300));
        _flushTaskId = scheduler.SchedulePeriodic("monitoring-flush", [] { flush(); },
                                                  std::chrono::duration_cast<std::chrono::milliseconds>(averagePeriod));

        _pruneTaskId = scheduler.SchedulePeriodic("monitoring-prune", [] { prune(); },
                                                  std::chrono::duration_cast<std::chrono::milliseconds>(kPrunePeriod));

#ifdef __linux__
        // collectCpuUsage() reads /proc/stat, which only exists on Linux.
        const auto cpuUsagePeriod = std::chrono::seconds(Core::Configuration::instance().getOr<long>("euclid.monitoring.cpu-usage-period", kCpuUsagePeriod.count()));
        _cpuUsageTaskId = scheduler.SchedulePeriodic("monitoring-cpu-usage", [] { collectCpuUsage(); },
                                                     std::chrono::duration_cast<std::chrono::milliseconds>(cpuUsagePeriod));
#endif
    }

    EmoServer::~EmoServer() {
        auto &scheduler = Core::Scheduler::instance();
        scheduler.Cancel(_flushTaskId);
        scheduler.Cancel(_pruneTaskId);
        scheduler.Cancel(_cpuUsageTaskId);
    }

    void EmoServer::flush() {

        std::map<std::string, Accumulator> snapshot;
        {
            std::lock_guard lock(accumulatorsMutex());
            snapshot.swap(accumulators());
        }

        const auto timestamp = std::chrono::system_clock::now();
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
            row.value = acc.isRate ? acc.sum : acc.sum / static_cast<double>(acc.samples);
            row.timestamp = timestamp;
            repo->insert(row);

            log_debug << "Monitoring flushed, name: " << row.name << ", labelName: " << row.labelName
                    << ", labelValue: " << row.labelValue << ", value: " << row.value;
        }
    }

    void EmoServer::prune() {
        const auto retentionDays = Core::Configuration::instance().getOr<long>("euclid.monitoring.retention", 3);
        const auto cutoff = std::chrono::system_clock::now() - std::chrono::hours(24 * retentionDays);
        Database::RepositoryFactory::instance().emoRepository()->deleteOlderThan(cutoff);
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

        Database::Entity::Monitoring::MonitoringData row;
        row.name = "system-cpu-usage";
        row.labelName = "host";
        row.labelValue = Core::SystemUtils::GetHostName();
        row.value = 100.0 * static_cast<double>(totalDelta - idleDelta) / static_cast<double>(totalDelta);
        row.timestamp = std::chrono::system_clock::now();

        Database::RepositoryFactory::instance().emoRepository()->insert(row);

        log_debug << "Monitoring flushed, name: " << row.name << ", labelName: " << row.labelName
                << ", labelValue: " << row.labelValue << ", value: " << row.value;
    }

    response<string_body> EmoServer::Dispatch(const request<string_body> &req) {
        return dispatch(req);
    }

} // namespace Euclid::Monitoring
