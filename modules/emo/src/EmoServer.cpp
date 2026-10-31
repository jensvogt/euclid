// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// C++ includes
#include <atomic>
#include <chrono>
#include <filesystem>
#include <cmath>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <tuple>
#include <vector>
#if defined(__linux__)
#include <cerrno>
#include <sys/syscall.h>
#include <unistd.h>
#endif

// Boost includes
#include <boost/beast/core.hpp>

// Euclid includes
#include <AlertEvaluator.h>
#include <EmoServer.h>
#include <euclid/core/Configuration.h>
#include <euclid/core/DateTimeUtils.h>
#include <euclid/core/DirUtils.h>
#include <euclid/database/EventBus.h>
#include <euclid/database/PeriodicClaim.h>
#include <euclid/core/SystemUtils.h>
#include <euclid/database/entity/eam/User.h>

namespace Euclid::Monitoring {

    namespace beast = boost::beast;
    namespace http = beast::http;

    using Database::Entity::Monitoring::AlertComparisonFromString;
    using Database::Entity::Monitoring::AlertStatisticFromString;
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

        // Slower than the rest: these move at the pace a database grows, and dbStats is a
        // round trip per tick that tells you nothing new a second later.
        constexpr auto kDatabaseSizePeriod = std::chrono::seconds(300);

        // Hourly: a walk of ESM's directory is minutes of disk work - see collectDiskUsage() - and
        // a data directory grows by the day, not by the minute.
        constexpr auto kDiskUsagePeriod = std::chrono::seconds(3600);
        // How often a walk is asked for, and how soon after a start the first time - see where the
        // task is scheduled for why these are not the period.
        constexpr auto kDiskUsageTick = std::chrono::seconds(300);
        constexpr auto kDiskUsageFirstTick = std::chrono::seconds(60);

        // A walk in progress, so a slow one is never joined by the next, and the module stopping,
        // so a walk does not hold the shutdown for the minutes it takes.
        std::atomic s_diskWalkRunning{false};
        std::atomic s_stopping{false};

        // Where the modules keep their files: every directory directly in euclid.data-dir, by its
        // name, and any module whose own data-dir is somewhere else.
        std::map<std::string, std::filesystem::path> moduleDataDirectories() {
            const auto &configuration = Core::Configuration::instance();
            const std::filesystem::path root = configuration.getOr<std::string>("euclid.data-dir", "/usr/local/euclid/data");

            std::map<std::string, std::filesystem::path> directories;
            std::error_code ec;
            for (const auto &entry: std::filesystem::directory_iterator(root, ec)) {
                if (std::error_code typeError; entry.is_directory(typeError) && !entry.is_symlink(typeError)) {
                    directories[entry.path().filename().string()] = entry.path();
                }
            }

            const auto rootText = std::filesystem::weakly_canonical(root, ec).string();
            for (const auto &module: configuration.getKeys("euclid.modules")) {
                const auto configured = configuration.getOr<std::string>("euclid.modules." + module + ".data-dir", "");
                if (configured.empty()) continue;
                const auto resolved = std::filesystem::weakly_canonical(configured, ec);
                // Inside the root it is already counted, under its directory's name.
                if (resolved.string().starts_with(rootText + "/") || resolved.string() == rootText) continue;
                directories[module] = resolved;
            }
            return directories;
        }

#if defined(__linux__)
        // Idle I/O priority for the calling thread: the walk only gets the disk when nothing else
        // wants it, so a busy ESM is not slowed down to measure it. The kernel's own numbers,
        // since glibc wraps none of this.
        void lowerIoPriority() {
            constexpr int kIoprioWhoProcess = 1;
            constexpr int kIoprioClassIdle = 3;
            constexpr int kIoprioClassShift = 13;
            if (::syscall(SYS_ioprio_set, kIoprioWhoProcess, 0, kIoprioClassIdle << kIoprioClassShift) != 0) {
                log_debug << "Monitoring disk-usage could not lower its I/O priority, errno: " << errno;
            }
        }
#endif
        // How long the counters may carry the drift that nothing else can correct.
        //
        // Queues every five minutes rather than hourly, though: their counters are not only a
        // figure on a dashboard. The autoscaler sizes a pool from a queue's backlog, and a
        // short-polling listener asks the count before it fetches - so a counter that drifted to
        // zero once left 796,171 messages unread and their pool scaled down to one for the best part
        // of an hour. Twelve passes an hour is still a twentieth of what four a minute cost.

        // How often each pool's instance count and load is sampled. Shorter than EMO's averaging
        // bucket on purpose: a ramp that happens inside one bucket should leave more than one
        // sample in it, or the max the rollups keep is the only trace of it.
        constexpr auto kModuleInstancePeriod = std::chrono::seconds(15);

        // Rollups run several times per target bucket rather than once, so the bucket currently in
        // progress is always readable, just incomplete. Cheap because each pass only reads the tier
        // above over a short window, and safe because rollups replace buckets instead of appending.
        constexpr auto kHourRollupPeriod = std::chrono::minutes(10);
        constexpr auto kDayRollupPeriod = std::chrono::hours(1);

        // Default retention per tier, in days. Roughly 2000 rows per series in total, against the
        // ~525,000 five-minute rows five years of unsampled history would cost.
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

        // What identifies a series while it is being accumulated. A struct rather than the packed
        // "name:labelName:labelValue" string this used to be: flush() had to split that back apart
        // to write a row, which was already delicate and is not possible at all once a label map
        // with arbitrary values is in it.
        struct SeriesKey {
            std::string name;
            std::map<std::string, std::string> labels;

            bool operator<(const SeriesKey &other) const {
                return std::tie(name, labels) < std::tie(other.name, other.labels);
            }
        };

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

        std::map<SeriesKey, Accumulator> &accumulators() {
            static std::map<SeriesKey, Accumulator> accumulators;
            return accumulators;
        }

        // Single entry point for every sample, whichever side it arrives from, so that all of them
        // land in the same bucket-aligned rows that the rollups can then aggregate.
        void recordSample(const std::string &name, const std::map<std::string, std::string> &labels, const double value, const MetricType type) {

            std::lock_guard lock(accumulatorsMutex());
            auto &acc = accumulators()[SeriesKey{.name = name, .labels = labels}];
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

        // The one-dimensional form, which is every metric euclid records about itself: a service
        // time by method, an instance count by module. An empty label name records the series with
        // no dimensions at all rather than one called "".
        void recordSample(const std::string &name, const std::string &labelName, const std::string &labelValue, const double value, const MetricType type) {
            std::map<std::string, std::string> labels;
            if (!labelName.empty()) labels[labelName] = labelValue;
            recordSample(name, labels, value, type);
        }

        std::chrono::seconds retentionOf(const Resolution resolution) {
            const auto &configuration = Core::Configuration::instance();
            switch (resolution) {
                case Resolution::HOUR:
                    return std::chrono::hours(24 * configuration.getOr<long>("euclid.modules.emo.retention.hour", kHourRetentionDays));
                case Resolution::DAY:
                    return std::chrono::hours(24 * configuration.getOr<long>("euclid.modules.emo.retention.day", kDayRetentionDays));
                default:
                    return std::chrono::hours(24 * configuration.getOr<long>("euclid.modules.emo.retention.raw", kRawRetentionDays));
            }
        }

        std::chrono::seconds averagePeriod() {
            return std::chrono::seconds(Core::Configuration::instance().getOr<long>("euclid.modules.emo.average-period", 300));
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
        if (const auto *v = obj.if_contains("labels"); v && v->is_object()) {
            for (const auto &label: v->as_object()) {
                if (label.value().is_string()) query.labels[std::string(label.key())] = label.value().as_string().c_str();
            }
        }
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
            std::map<std::string, std::string> labels;
            double value = 0;
            auto type = MetricType::GAUGE;
            if (const auto *p = item.if_contains("name"); p && p->is_string()) name = p->as_string().c_str();
            if (const auto *p = item.if_contains("labelName"); p && p->is_string()) labelName = p->as_string().c_str();
            if (const auto *p = item.if_contains("labelValue"); p && p->is_string()) labelValue = p->as_string().c_str();
            // Dimensions as a map, which is how anything with more than one sends them - a
            // Micrometer meter, a Prometheus series. Values are taken as strings whatever they
            // arrived as, since that is what a dimension is; a number here would otherwise split
            // one series into two on nothing but its JSON spelling.
            if (const auto *p = item.if_contains("labels"); p && p->is_object()) {
                for (const auto &label: p->as_object()) {
                    if (label.value().is_string()) labels[std::string(label.key())] = label.value().as_string().c_str();
                    else if (label.value().is_int64()) labels[std::string(label.key())] = std::to_string(label.value().as_int64());
                    else if (label.value().is_double()) labels[std::string(label.key())] = std::to_string(label.value().as_double());
                }
            }
            if (const auto *p = item.if_contains("value"); p && p->is_number()) value = p->to_number<double>();
            if (const auto *p = item.if_contains("type"); p && p->is_string()) type = p->as_string() == "rate" ? MetricType::RATE : MetricType::GAUGE;
            if (name.empty()) continue;

            // Both spellings in one item is not a conflict to resolve: the pair is simply one more
            // dimension, and a pusher that sends both means both.
            if (!labelName.empty()) labels[labelName] = labelValue;

            recordSample(name, labels, value, type);
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
            boost::json::object labels;
            for (const auto &[label, value]: row.labels) labels[label] = value;

            // The map, plus the first pair flattened out beside it. The flat pair is what every
            // reader written before the map asks for - the RUI graphs a series per labelValue -
            // and for the one-dimensional metrics euclid records about itself the two say exactly
            // the same thing.
            items.push_back(boost::json::object{
                    {"name", row.name},
                    {"labels", labels},
                    {"labelName", row.labelName()},
                    {"labelValue", row.labelValue()},
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

    // ── Dashboard action handlers ────────────────────────────────────────────
    //
    // Authenticated but deliberately *not* admin-only, unlike "list" and "average" above. Those
    // read everything this module has recorded about the installation, which is an administrator's
    // business; these read and write one user's own saved layouts, and a feature called "save my
    // dashboard" that only administrators can use is not the feature.
    //
    // The owner is always auth.user->userId - the subject the gateway authenticated - and never a
    // field of the request body. That is the whole of the access control here: there is no way to
    // name another user, so there is no way to read or overwrite their dashboards, and no
    // per-request permission check to get wrong.

    namespace {
        // Shared preamble: authenticate, parse, and pull out "name".
        struct DashboardRequest {
            std::optional<response<string_body> > failure;
            std::string userId;
            std::string name;
            boost::json::value body;
        };
    }

    static DashboardRequest readDashboardRequest(const request<string_body> &req, const bool requireName) {

        DashboardRequest result;

        const auto auth = authenticate(req);
        if (!auth.user.has_value()) {
            result.failure = unauthorized(req, auth);
            return result;
        }
        result.userId = auth.user->userId;

        if (const auto err = EmoServer::ParseJsonBody(req, result.body)) {
            result.failure = *err;
            return result;
        }

        if (result.body.is_object()) {
            if (const auto *v = result.body.as_object().if_contains("name"); v && v->is_string()) {
                result.name = v->as_string().c_str();
            }
        }
        if (requireName && result.name.empty()) {
            result.failure = EmoServer::ErrorResponse(req, status::bad_request, R"(Expected a non-empty "name")");
        }
        return result;
    }

    static response<string_body> handleSaveDashboard(const request<string_body> &req) {

        auto parsed = readDashboardRequest(req, true);
        if (parsed.failure) return *parsed.failure;

        const auto *layout = parsed.body.is_object() ? parsed.body.as_object().if_contains("dashboard") : nullptr;
        if (!layout || !layout->is_object()) {
            return EmoServer::ErrorResponse(req, status::bad_request, R"(Expected a "dashboard" object)");
        }

        auto &repository = *Database::RepositoryFactory::instance().emoRepository();

        // Keeps the original "created" when there already is one, so that re-saving a layout does
        // not make it look new. Everything else about the row is replaced.
        const auto now = std::chrono::system_clock::now();
        const auto existing = repository.getDashboard(parsed.userId, parsed.name);

        Database::Entity::Monitoring::Dashboard dashboard;
        dashboard.userId = parsed.userId;
        dashboard.name = parsed.name;
        // Re-serialised from the parsed value rather than taken as a slice of the request body:
        // what is stored is then known to be exactly one JSON object, whatever surrounded it.
        dashboard.document = boost::json::serialize(*layout);
        dashboard.created = existing ? existing->created : now;
        dashboard.modified = now;

        repository.upsertDashboard(dashboard);

        return EmoServer::JsonResponse(req, status::ok, boost::json::serialize(boost::json::object{
                                               {"name", dashboard.name},
                                               {"userId", dashboard.userId},
                                               {"modified", Core::DateTimeUtils::ToISO8601(dashboard.modified)}}));
    }

    static response<string_body> handleGetDashboard(const request<string_body> &req) {

        auto parsed = readDashboardRequest(req, true);
        if (parsed.failure) return *parsed.failure;

        const auto dashboard = Database::RepositoryFactory::instance().emoRepository()->getDashboard(parsed.userId, parsed.name);
        if (!dashboard) {
            return EmoServer::ErrorResponse(req, status::not_found, "No dashboard called '" + parsed.name + "'");
        }

        // Parsed back into the response rather than embedded as a string, so a client reads the
        // layout as JSON exactly as it sent it. A row whose text somehow will not parse is
        // reported rather than passed on as a string the client would mistake for a layout.
        boost::system::error_code ec;
        const auto layout = boost::json::parse(dashboard->document, ec);
        if (ec || !layout.is_object()) {
            return EmoServer::ErrorResponse(req, status::internal_server_error, "Stored dashboard '" + parsed.name + "' is not readable");
        }

        return EmoServer::JsonResponse(req, status::ok, boost::json::serialize(boost::json::object{
                                               {"name", dashboard->name},
                                               {"dashboard", layout},
                                               {"created", Core::DateTimeUtils::ToISO8601(dashboard->created)},
                                               {"modified", Core::DateTimeUtils::ToISO8601(dashboard->modified)}}));
    }

    static response<string_body> handleListDashboards(const request<string_body> &req) {

        auto parsed = readDashboardRequest(req, false);
        if (parsed.failure) return *parsed.failure;

        const auto dashboards = Database::RepositoryFactory::instance().emoRepository()->listDashboards(parsed.userId);

        boost::json::array items;
        for (const auto &dashboard: dashboards) {
            // Names and times only. The layouts are in the rows, but a chooser needs a list of
            // names and sending every panel of every dashboard to draw one is a waste of a wire.
            items.push_back(boost::json::object{
                    {"name", dashboard.name},
                    {"created", Core::DateTimeUtils::ToISO8601(dashboard.created)},
                    {"modified", Core::DateTimeUtils::ToISO8601(dashboard.modified)}});
        }

        return EmoServer::JsonResponse(req, status::ok, boost::json::serialize(boost::json::object{
                                               {"userId", parsed.userId},
                                               {"items", items}}));
    }

    static response<string_body> handleDeleteDashboard(const request<string_body> &req) {

        auto parsed = readDashboardRequest(req, true);
        if (parsed.failure) return *parsed.failure;

        if (Database::RepositoryFactory::instance().emoRepository()->deleteDashboard(parsed.userId, parsed.name) == 0) {
            return EmoServer::ErrorResponse(req, status::not_found, "No dashboard called '" + parsed.name + "'");
        }

        return EmoServer::JsonResponse(req, status::ok);
    }

    // ── Alerting ─────────────────────────────────────────────────────────────
    //
    // Rules are written through the actions below and judged in flush(), on the rows it has just
    // written; a series that stops arriving is found by sweepAlerts(). A change of state is written
    // to emo_alert_state, recorded in emo_alert_history, and only then published on the event bus
    // as emo.alert.raised or emo.alert.cleared - publish only on a change, so a hundred periods of
    // CRITICAL are one event, and an escalation from WARNING to CRITICAL is another.
    //
    // Who changed a threshold is EAD's: these are ordinary actions, so Dispatch() audits them.
    // The alerts themselves are not - an alert has no caller and no command, and the audit trail's
    // whole value is that it answers who did something.

    namespace {

        using Database::Entity::Monitoring::AlertTransition;

        // Rows a rule may match before it is refused or stops taking new series. One state per
        // matched series, so a rule with no selector on a metric labelled per instance is a state
        // per instance - worth an outright refusal rather than discovering it in the database.
        constexpr long kDefaultMaxSeriesPerRule = 200;
        constexpr long kDefaultAlertHistoryRetentionDays = 90;

        // Held by whatever reads states and writes them back - the evaluation in flush(), the
        // no-data sweep - and by whatever removes them - a rule updated or deleted. Without it an
        // evaluation that read a rule just before it was deleted writes its states back after the
        // delete, and a rule created again under the same id inherits them. One process is all
        // there is to exclude: EMO runs a single instance.
        std::mutex &alertMutex() {
            static std::mutex mutex;
            return mutex;
        }

        long maxSeriesPerRule() {
            return Core::Configuration::instance().getOr<long>("euclid.modules.emo.alert.max-series-per-rule", kDefaultMaxSeriesPerRule);
        }

        std::chrono::seconds alertHistoryRetention() {
            return std::chrono::hours(24 * Core::Configuration::instance().getOr<long>("euclid.modules.emo.alert.history-retention",
                                                                                     kDefaultAlertHistoryRetentionDays));
        }

        boost::json::object labelsJson(const std::map<std::string, std::string> &labels) {
            boost::json::object object;
            for (const auto &[name, value]: labels) object[name] = value;
            return object;
        }

        boost::json::value optionalNumber(const std::optional<double> &value) {
            return value ? boost::json::value(*value) : boost::json::value(nullptr);
        }

        std::string isoOrEmpty(const std::chrono::system_clock::time_point &time) {
            return time == std::chrono::system_clock::time_point{} ? std::string() : Core::DateTimeUtils::ToISO8601(time);
        }

        boost::json::object ruleJson(const AlertRule &rule) {
            return boost::json::object{
                    {"ruleId", rule.ruleId},
                    {"name", rule.name},
                    {"labels", labelsJson(rule.labels)},
                    {"resolution", ResolutionToString(rule.resolution)},
                    {"statistic", AlertStatisticToString(rule.statistic)},
                    {"comparison", AlertComparisonToString(rule.comparison)},
                    {"warning", optionalNumber(rule.warning)},
                    {"critical", optionalNumber(rule.critical)},
                    {"forPeriods", rule.forPeriods},
                    {"clearPeriods", rule.clearPeriods},
                    {"noDataAfterPeriods", rule.noDataAfterPeriods},
                    {"enabled", rule.enabled},
                    {"description", rule.description},
                    {"accountId", rule.accountId},
                    {"namespace", rule.nameSpace},
                    {"created", isoOrEmpty(rule.created)},
                    {"modified", isoOrEmpty(rule.modified)},
            };
        }

        boost::json::object transitionJson(const AlertTransition &transition) {
            return boost::json::object{
                    {"ruleId", transition.ruleId},
                    {"name", transition.name},
                    {"labels", labelsJson(transition.labels)},
                    {"previous", AlertLevelToString(transition.previous)},
                    {"state", AlertLevelToString(transition.level)},
                    {"severity", AlertSeverity(transition.level)},
                    {"value", transition.value},
                    {"threshold", optionalNumber(transition.threshold)},
                    {"statistic", AlertStatisticToString(transition.statistic)},
                    {"reason", transition.reason},
                    {"timestamp", Core::DateTimeUtils::ToISO8601(transition.timestamp)},
                    {"accountId", transition.accountId},
                    {"namespace", transition.nameSpace},
            };
        }

        // Records one change and announces it. Called only once the state it describes has been
        // written, so a subscriber is never told of a state a restarted EMO does not know it is in.
        // A history row that cannot be written is said and not waited on: the event is the part
        // somebody is acting on.
        void announceAlert(const AlertRule &rule, const AlertState &state, const AlertChange &change,
                           const std::chrono::system_clock::time_point when, const std::string &reason = {}) {

            AlertTransition transition;
            transition.accountId = rule.accountId;
            transition.nameSpace = rule.nameSpace;
            transition.ruleId = rule.ruleId;
            transition.name = rule.name;
            transition.labels = state.labels;
            transition.labelKey = state.labelKey;
            transition.previous = change.previous;
            transition.level = change.level;
            transition.value = change.value;
            transition.threshold = change.threshold;
            transition.statistic = rule.statistic;
            transition.reason = reason;
            transition.timestamp = when;
            transition.expiresAt = when + alertHistoryRetention();

            const auto repository = Database::RepositoryFactory::instance().emoRepository();
            if (!repository->insertAlertTransition(transition)) {
                log_error << "Alert history not recorded, ruleId: " << rule.ruleId << ", series: " << state.labelKey;
            }

            const bool cleared = change.level == AlertLevel::OK;
            if (cleared) {
                log_info << "Alert cleared, ruleId: " << rule.ruleId << ", metric: " << rule.name << ", series: " << state.labelKey
                         << ", was: " << AlertLevelToString(change.previous) << ", value: " << change.value
                         << (reason.empty() ? "" : ", reason: " + reason);
            } else {
                log_warning << "Alert raised, ruleId: " << rule.ruleId << ", metric: " << rule.name << ", series: " << state.labelKey
                            << ", severity: " << AlertSeverity(change.level) << ", was: " << AlertLevelToString(change.previous)
                            << ", value: " << change.value;
            }

            // Top-level fields, because a subscription filter matches a payload field by equality:
            // "severity" and "name" up here are what let a subscriber take only the critical ones,
            // or only one metric's.
            boost::json::object payload{
                    {"ruleId", rule.ruleId},
                    {"name", rule.name},
                    {"labels", labelsJson(state.labels)},
                    {"severity", AlertSeverity(change.level)},
                    {"previous", AlertSeverity(change.previous)},
                    {"value", change.value},
                    {"threshold", optionalNumber(change.threshold)},
                    {"statistic", AlertStatisticToString(rule.statistic)},
                    {"comparison", AlertComparisonToString(rule.comparison)},
                    {"since", Core::DateTimeUtils::ToISO8601(state.since)},
                    {"accountId", rule.accountId},
                    {"namespace", rule.nameSpace},
            };
            if (!reason.empty()) payload["reason"] = reason;

            try {
                Database::EventBus::instance().Publish(cleared ? "emo.alert.cleared" : "emo.alert.raised", payload, "emo");
            } catch (const std::exception &e) {
                log_error << "Alert event not published, ruleId: " << rule.ruleId << ", series: " << state.labelKey << ", error: " << e.what();
            }
        }

        // Ends every series of a rule that is about to stop being judged - deleted, disabled, or
        // pointed at other series. Each one not OK is cleared, said and recorded first: otherwise a
        // subscriber that was told CRITICAL is never told anything again, and waits on a rule that
        // no longer exists.
        void retireAlertStates(const AlertRule &rule, const std::string &reason) {

            const auto repository = Database::RepositoryFactory::instance().emoRepository();
            const auto now = std::chrono::system_clock::now();
            for (auto &state: repository->listAlertStates(rule.accountId, rule.ruleId)) {
                if (state.level == AlertLevel::OK) continue;
                const AlertChange change{.previous = state.level, .level = AlertLevel::OK, .value = state.lastValue};
                state.level = AlertLevel::OK;
                state.since = now;
                announceAlert(rule, state, change, now, reason);
            }
            repository->deleteAlertStates(rule.accountId, rule.ruleId);
        }

        // How many series a selector matches now, from the last two periods' rows.
        long matchingSeries(const AlertRule &rule) {
            Database::MonitoringQuery query;
            query.name = rule.name;
            query.labels = rule.labels;
            query.resolution = Resolution::RAW;
            query.from = std::chrono::system_clock::now() - 2 * averagePeriod();
            std::set<std::string> keys;
            for (const auto &row: Database::RepositoryFactory::instance().emoRepository()->list(query)) keys.insert(row.labelKey());
            return static_cast<long>(keys.size());
        }

        // Every alert action is an administrator's, as "list" and "average" are: a rule reads the
        // installation's metrics, and its states say what they are.
        struct AlertRequest {
            std::optional<response<string_body> > failure;
            std::string accountId;
            std::string nameSpace;
            boost::json::object body;
        };

        AlertRequest readAlertRequest(const request<string_body> &req) {

            AlertRequest result;
            const auto auth = authenticate(req);
            if (!auth.user.has_value()) {
                result.failure = unauthorized(req, auth);
                return result;
            }
            if (!Database::IsEamAdmin(*Database::RepositoryFactory::instance().eamRepository(), auth.user->userId)) {
                result.failure = EmoServer::ErrorResponse(req, status::forbidden, "Administrator privileges required");
                return result;
            }

            boost::json::value jv;
            if (const auto err = EmoServer::ParseJsonBody(req, jv)) {
                result.failure = *err;
                return result;
            }
            if (jv.is_object()) result.body = jv.as_object();
            else if (!jv.is_null()) {
                result.failure = EmoServer::ErrorResponse(req, status::bad_request, "Expected a JSON object body");
                return result;
            }

            // The account the session is working in, as every other module scopes by, falling back
            // to the user's own for a caller that sends none.
            result.accountId = std::string(req["x-euclid-account-id"]);
            if (result.accountId.empty()) result.accountId = auth.user->accountId;
            result.nameSpace = std::string(req["x-euclid-namespace"]);
            return result;
        }

        std::string stringField(const boost::json::object &body, const char *field) {
            const auto *v = body.if_contains(field);
            return v && v->is_string() ? std::string(v->as_string()) : std::string();
        }

        // Sets on the rule whatever the body carries, and only that - the whole of a create, the
        // changed fields of an update. A threshold given as null is removed. Returns what is wrong
        // with a field, or empty.
        std::string applyRuleFields(const boost::json::object &body, AlertRule &rule) {

            const auto wholeNumber = [&](const char *field, long &target) -> std::string {
                const auto *v = body.if_contains(field);
                if (!v) return {};
                if (!v->is_number()) return std::string("\"") + field + "\" must be a whole number";
                const auto number = v->to_number<double>();
                if (number != std::floor(number)) return std::string("\"") + field + "\" must be a whole number";
                target = static_cast<long>(number);
                return {};
            };
            const auto threshold = [&](const char *field, std::optional<double> &target) -> std::string {
                const auto *v = body.if_contains(field);
                if (!v) return {};
                if (v->is_null()) {
                    target.reset();
                    return {};
                }
                if (!v->is_number()) return std::string("\"") + field + "\" must be a number or null";
                target = v->to_number<double>();
                return {};
            };

            if (const auto *v = body.if_contains("name")) {
                if (!v->is_string()) return R"("name" must be a string)";
                rule.name = v->as_string().c_str();
            }
            if (const auto *v = body.if_contains("labels")) {
                if (!v->is_object()) return R"("labels" must be an object of strings)";
                rule.labels.clear();
                for (const auto &label: v->as_object()) {
                    if (!label.value().is_string()) return R"("labels" must be an object of strings)";
                    rule.labels[std::string(label.key())] = label.value().as_string().c_str();
                }
            }
            if (const auto *v = body.if_contains("resolution")) {
                if (!v->is_string()) return R"("resolution" must be a string)";
                rule.resolution = ResolutionFromString(v->as_string().c_str());
            }
            if (const auto *v = body.if_contains("statistic")) {
                if (!v->is_string()) return R"("statistic" must be a string)";
                rule.statistic = AlertStatisticFromString(v->as_string().c_str());
            }
            if (const auto *v = body.if_contains("comparison")) {
                if (!v->is_string()) return R"("comparison" must be a string)";
                rule.comparison = AlertComparisonFromString(v->as_string().c_str());
            }
            if (auto error = threshold("warning", rule.warning); !error.empty()) return error;
            if (auto error = threshold("critical", rule.critical); !error.empty()) return error;
            if (auto error = wholeNumber("forPeriods", rule.forPeriods); !error.empty()) return error;
            if (auto error = wholeNumber("clearPeriods", rule.clearPeriods); !error.empty()) return error;
            if (auto error = wholeNumber("noDataAfterPeriods", rule.noDataAfterPeriods); !error.empty()) return error;
            if (const auto *v = body.if_contains("enabled")) {
                if (!v->is_bool()) return R"("enabled" must be true or false)";
                rule.enabled = v->as_bool();
            }
            if (const auto *v = body.if_contains("description")) {
                if (!v->is_string()) return R"("description" must be a string)";
                rule.description = v->as_string().c_str();
            }
            return {};
        }

        std::optional<response<string_body> > refuseOversizedRule(const request<string_body> &req, const AlertRule &rule) {
            const auto cap = maxSeriesPerRule();
            if (const auto matched = matchingSeries(rule); cap > 0 && matched > cap) {
                return EmoServer::ErrorResponse(req, status::bad_request,
                                                "the rule matches " + std::to_string(matched) + " series of '" + rule.name + "', more than " +
                                                        std::to_string(cap) + " (euclid.modules.emo.alert.max-series-per-rule); narrow it with \"labels\"");
            }
            return std::nullopt;
        }

    }// namespace

    static response<string_body> handleCreateAlertRule(const request<string_body> &req) {

        auto parsed = readAlertRequest(req);
        if (parsed.failure) return *parsed.failure;

        AlertRule rule;
        rule.ruleId = stringField(parsed.body, "ruleId");
        if (auto error = applyRuleFields(parsed.body, rule); !error.empty()) {
            return EmoServer::ErrorResponse(req, status::bad_request, error);
        }
        if (auto error = ValidateAlertRule(rule); !error.empty()) {
            return EmoServer::ErrorResponse(req, status::bad_request, error);
        }
        rule.accountId = parsed.accountId;
        rule.nameSpace = parsed.nameSpace;

        auto &repository = *Database::RepositoryFactory::instance().emoRepository();
        if (repository.getAlertRule(rule.accountId, rule.ruleId)) {
            return EmoServer::ErrorResponse(req, status::conflict, "An alert rule '" + rule.ruleId + "' already exists");
        }
        if (auto refused = refuseOversizedRule(req, rule)) return *refused;

        rule.created = rule.modified = std::chrono::system_clock::now();
        if (!repository.upsertAlertRule(rule)) {
            return EmoServer::ErrorResponse(req, status::internal_server_error, "The alert rule could not be stored");
        }
        return EmoServer::JsonResponse(req, status::ok, boost::json::serialize(ruleJson(rule)));
    }

    static response<string_body> handleUpdateAlertRule(const request<string_body> &req) {

        auto parsed = readAlertRequest(req);
        if (parsed.failure) return *parsed.failure;

        const auto ruleId = stringField(parsed.body, "ruleId");
        if (ruleId.empty()) return EmoServer::ErrorResponse(req, status::bad_request, R"("ruleId" is required)");

        std::lock_guard lock(alertMutex());
        auto &repository = *Database::RepositoryFactory::instance().emoRepository();
        const auto existing = repository.getAlertRule(parsed.accountId, ruleId);
        if (!existing) return EmoServer::ErrorResponse(req, status::not_found, "No alert rule '" + ruleId + "'");

        auto rule = *existing;
        if (auto error = applyRuleFields(parsed.body, rule); !error.empty()) {
            return EmoServer::ErrorResponse(req, status::bad_request, error);
        }
        if (auto error = ValidateAlertRule(rule); !error.empty()) {
            return EmoServer::ErrorResponse(req, status::bad_request, error);
        }

        const bool otherSeries = rule.name != existing->name || rule.labels != existing->labels;
        if (otherSeries) {
            if (auto refused = refuseOversizedRule(req, rule)) return *refused;
        }

        rule.modified = std::chrono::system_clock::now();
        if (!repository.upsertAlertRule(rule)) {
            return EmoServer::ErrorResponse(req, status::internal_server_error, "The alert rule could not be stored");
        }

        // A rule now judging other series, or none, ends what it said about the old ones. A changed
        // threshold does not: the next period judges the same series against the new lines, which
        // is the change somebody making it wants to see.
        if (existing->enabled && !rule.enabled) retireAlertStates(*existing, "rule-disabled");
        else if (otherSeries) retireAlertStates(*existing, "rule-changed");

        return EmoServer::JsonResponse(req, status::ok, boost::json::serialize(ruleJson(rule)));
    }

    static response<string_body> handleDeleteAlertRule(const request<string_body> &req) {

        auto parsed = readAlertRequest(req);
        if (parsed.failure) return *parsed.failure;

        const auto ruleId = stringField(parsed.body, "ruleId");
        if (ruleId.empty()) return EmoServer::ErrorResponse(req, status::bad_request, R"("ruleId" is required)");

        std::lock_guard lock(alertMutex());
        auto &repository = *Database::RepositoryFactory::instance().emoRepository();
        const auto existing = repository.getAlertRule(parsed.accountId, ruleId);
        if (!existing) return EmoServer::ErrorResponse(req, status::not_found, "No alert rule '" + ruleId + "'");

        retireAlertStates(*existing, "rule-deleted");
        repository.deleteAlertRule(parsed.accountId, ruleId);
        return EmoServer::JsonResponse(req, status::ok);
    }

    static response<string_body> handleListAlertRules(const request<string_body> &req) {

        auto parsed = readAlertRequest(req);
        if (parsed.failure) return *parsed.failure;

        const auto ruleId = stringField(parsed.body, "ruleId");
        boost::json::array items;
        for (const auto &rule: Database::RepositoryFactory::instance().emoRepository()->listAlertRules(parsed.accountId)) {
            if (!ruleId.empty() && rule.ruleId != ruleId) continue;
            items.push_back(ruleJson(rule));
        }
        return EmoServer::JsonResponse(req, status::ok, boost::json::serialize(boost::json::object{{"items", items}}));
    }

    // Where every matched series stands now. What a wall colours a panel's border from, and what
    // its banner lists - so each row carries the rule's metric, lines and description beside the
    // state, rather than a ruleId to be looked up.
    static response<string_body> handleListAlerts(const request<string_body> &req) {

        auto parsed = readAlertRequest(req);
        if (parsed.failure) return *parsed.failure;

        const auto ruleId = stringField(parsed.body, "ruleId");
        const auto wanted = stringField(parsed.body, "state");
        bool activeOnly = false;
        if (const auto *v = parsed.body.if_contains("active"); v && v->is_bool()) activeOnly = v->as_bool();

        const auto repository = Database::RepositoryFactory::instance().emoRepository();
        std::map<std::string, AlertRule> rules;
        for (auto &rule: repository->listAlertRules(parsed.accountId)) rules.emplace(rule.ruleId, std::move(rule));

        boost::json::array items;
        for (const auto &state: repository->listAlertStates(parsed.accountId, ruleId)) {
            if (activeOnly && state.level == AlertLevel::OK) continue;
            if (!wanted.empty() && AlertLevelToString(state.level) != wanted) continue;
            const auto rule = rules.find(state.ruleId);
            if (rule == rules.end()) continue;

            items.push_back(boost::json::object{
                    {"ruleId", state.ruleId},
                    {"name", rule->second.name},
                    {"labels", labelsJson(state.labels)},
                    {"state", AlertLevelToString(state.level)},
                    {"severity", AlertSeverity(state.level)},
                    {"since", isoOrEmpty(state.since)},
                    {"value", state.lastValue},
                    {"lastSeen", isoOrEmpty(state.lastSeen)},
                    {"statistic", AlertStatisticToString(rule->second.statistic)},
                    {"comparison", AlertComparisonToString(rule->second.comparison)},
                    {"warning", optionalNumber(rule->second.warning)},
                    {"critical", optionalNumber(rule->second.critical)},
                    {"description", rule->second.description},
                    {"namespace", rule->second.nameSpace},
            });
        }
        return EmoServer::JsonResponse(req, status::ok, boost::json::serialize(boost::json::object{{"items", items}}));
    }

    static response<string_body> handleListAlertHistory(const request<string_body> &req) {

        auto parsed = readAlertRequest(req);
        if (parsed.failure) return *parsed.failure;

        Database::AlertHistoryQuery query;
        query.accountId = parsed.accountId;
        query.ruleId = stringField(parsed.body, "ruleId");
        query.limit = 100;
        if (const auto *v = parsed.body.if_contains("limit"); v && v->is_number()) query.limit = static_cast<long>(v->to_number<double>());
        if (const auto from = stringField(parsed.body, "from"); !from.empty()) query.from = Core::DateTimeUtils::FromISO8601(from);
        if (const auto to = stringField(parsed.body, "to"); !to.empty()) query.to = Core::DateTimeUtils::FromISO8601(to);

        boost::json::array items;
        for (const auto &transition: Database::RepositoryFactory::instance().emoRepository()->listAlertHistory(query)) {
            items.push_back(transitionJson(transition));
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
        if (action == "average") return handleAverage(req);
        if (action == "push-metrics") return handlePushMetrics(req);
        if (action == "save-dashboard") return handleSaveDashboard(req);
        if (action == "get-dashboard") return handleGetDashboard(req);
        if (action == "list-dashboards") return handleListDashboards(req);
        if (action == "delete-dashboard") return handleDeleteDashboard(req);
        if (action == "create-alert-rule") return handleCreateAlertRule(req);
        if (action == "update-alert-rule") return handleUpdateAlertRule(req);
        if (action == "delete-alert-rule") return handleDeleteAlertRule(req);
        if (action == "list-alert-rules") return handleListAlertRules(req);
        if (action == "list-alerts") return handleListAlerts(req);
        if (action == "list-alert-history") return handleListAlertHistory(req);

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

        // As often as rows are written: a series is NO_DATA after so many missing periods, and
        // checking less often than one arrives would only make that late.
        _alertSweepTaskId = scheduler.SchedulePeriodic("monitoring-alert-sweep", [] { sweepAlerts(); },
                                                       std::chrono::duration_cast<std::chrono::milliseconds>(averagePeriod()));

        // RAW is rolled into HOUR and HOUR into DAY - never RAW straight into DAY, so the daily
        // pass reads 24 rows per series rather than 288.
        const auto hourRollupPeriod = std::chrono::seconds(Core::Configuration::instance().getOr<long>("euclid.modules.emo.rollup-hour-period", std::chrono::seconds(kHourRollupPeriod).count()));
        _hourRollupTaskId = scheduler.SchedulePeriodic("monitoring-rollup-hour", [] { rollup(Resolution::RAW, Resolution::HOUR); },
                                                       std::chrono::duration_cast<std::chrono::milliseconds>(hourRollupPeriod));

        const auto dayRollupPeriod = std::chrono::seconds(Core::Configuration::instance().getOr<long>("euclid.modules.emo.rollup-day-period", std::chrono::seconds(kDayRollupPeriod).count()));
        _dayRollupTaskId = scheduler.SchedulePeriodic("monitoring-rollup-day", [] { rollup(Resolution::HOUR, Resolution::DAY); },
                                                      std::chrono::duration_cast<std::chrono::milliseconds>(dayRollupPeriod));

        // The queue, bucket and topic recounts used to be scheduled here. They are each module's
        // own now - EqsServer, EsmServer and EnsServer - because they correct counters those
        // modules maintain, and running them from the monitoring module meant the correction
        // stopped whenever monitoring did. A queue reporting messages it no longer held, for as
        // long as EMO stayed down, is what that cost. The intervals are still read from the keys
        // they were configured under here, so nothing an operator set has to move.

        // What each pool is doing, sampled from the module records the manager keeps - so a graph
        // can answer "when did parser-dev ramp, and what was it reacting to" rather than only
        // `emm list-modules` answering "what is it doing now".
        const auto moduleInstancePeriod = std::chrono::seconds(Core::Configuration::instance().getOr<long>("euclid.modules.emo.module-instance-period", kModuleInstancePeriod.count()));
        _moduleInstancesTaskId = scheduler.SchedulePeriodic("monitoring-module-instances",
                                                            [] { collectModuleInstances(); },
                                                            std::chrono::duration_cast<std::chrono::milliseconds>(moduleInstancePeriod));

        // Nothing platform-specific about this one, unlike the three below: it asks the repository
        // for sizes rather than reading them off the machine. It used to sit inside the Linux guard
        // with them, which meant a Windows installation recorded nothing about its own database.
        const auto databaseSizePeriod = std::chrono::seconds(Core::Configuration::instance().getOr<long>("euclid.modules.emo.database-size-period", kDatabaseSizePeriod.count()));
        _databaseSizeTaskId = scheduler.SchedulePeriodic("monitoring-database-size", [] { collectDatabaseSize(); },
                                                         std::chrono::duration_cast<std::chrono::milliseconds>(databaseSizePeriod));

        // The volume's free space is one call and moves with every upload, so as often as the
        // database size; what each module's directory holds takes a walk, so far less often.
        // A period of 0 turns the walk off.
        _diskSpaceTaskId = scheduler.SchedulePeriodic("monitoring-disk-space", [] { collectDiskSpace(); },
                                                      std::chrono::duration_cast<std::chrono::milliseconds>(databaseSizePeriod));
        //
        // Asked often and done rarely: the task only asks whether a walk is due, and the periodic
        // claim - see collectDiskUsage() - is what holds walks to the period. Scheduled at the period
        // itself, as it first was, the first walk came two hours after a start: the first tick waits
        // a whole period, and the first claim of a job only records it and is never granted. Now a
        // start is measured within minutes, a restart does not push the next walk back by an hour,
        // and a walk an instance misses is picked up by whichever asks next.
        if (const auto diskUsagePeriod = std::chrono::seconds(Core::Configuration::instance().getOr<long>("euclid.modules.emo.disk-usage-period", kDiskUsagePeriod.count()));
            diskUsagePeriod.count() > 0) {
            const auto tick = std::min(diskUsagePeriod, kDiskUsageTick);
            _diskUsageTaskId = scheduler.SchedulePeriodic("monitoring-disk-usage", [] { collectDiskUsage(); },
                                                          std::chrono::duration_cast<std::chrono::milliseconds>(tick),
                                                          std::chrono::duration_cast<std::chrono::milliseconds>(kDiskUsageFirstTick));
        }

#if defined(__linux__) || defined(_WIN32)
        // What the host itself is doing. Read from /proc on Linux and from the Windows performance
        // counters on Windows - see Core::SystemUtils, which is where the difference lives. Still
        // guarded rather than scheduled unconditionally: on a platform with no reading (macOS) each
        // of these would log a warning once a minute for the life of the process.
        const auto cpuUsagePeriod = std::chrono::seconds(Core::Configuration::instance().getOr<long>("euclid.modules.emo.cpu-usage-period", kCpuUsagePeriod.count()));
        _cpuUsageTaskId = scheduler.SchedulePeriodic("monitoring-cpu-usage", [] { collectCpuUsage(); },
                                                     std::chrono::duration_cast<std::chrono::milliseconds>(cpuUsagePeriod));

        // Same period as the CPU figure, and the same reason: they are read together on a dashboard
        // and drifting apart in age would make them hard to compare.
        _memoryUsageTaskId = scheduler.SchedulePeriodic("monitoring-memory-usage", [] { collectMemoryUsage(); },
                                                        std::chrono::duration_cast<std::chrono::milliseconds>(cpuUsagePeriod));

        // Same period as the CPU figure: how much is queued and how much is being served are the
        // same question from two sides, and are read together.
        _systemLoadTaskId = scheduler.SchedulePeriodic("monitoring-system-load", [] { collectSystemLoad(); },
                                                       std::chrono::duration_cast<std::chrono::milliseconds>(cpuUsagePeriod));
#endif
    }

    EmoServer::~EmoServer() {
        // First: a walk in progress checks this between entries and gives up, rather than holding
        // the scheduler's shutdown - which waits for running tasks - for the minutes it takes.
        s_stopping.store(true);
        auto &scheduler = Core::Scheduler::instance();
        scheduler.Cancel(_diskUsageTaskId);
        scheduler.Cancel(_diskSpaceTaskId);
        scheduler.Cancel(_flushTaskId);
        scheduler.Cancel(_pruneTaskId);
        scheduler.Cancel(_alertSweepTaskId);
        scheduler.Cancel(_hourRollupTaskId);
        scheduler.Cancel(_dayRollupTaskId);
        scheduler.Cancel(_moduleInstancesTaskId);
        scheduler.Cancel(_databaseSizeTaskId);
        scheduler.Cancel(_cpuUsageTaskId);
        scheduler.Cancel(_systemLoadTaskId);
        scheduler.Cancel(_memoryUsageTaskId);
    }

    void EmoServer::flush() {

        std::map<SeriesKey, Accumulator> snapshot;
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

        std::vector<Database::Entity::Monitoring::MonitoringData> written;
        written.reserve(snapshot.size());

        for (const auto &[key, acc]: snapshot) {
            if (acc.samples <= 0) continue;

            Database::Entity::Monitoring::MonitoringData row;
            row.name = key.name;
            row.labels = key.labels;
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

            log_debug << "Monitoring flushed, name: " << row.name << ", labels: " << row.labelKey() << ", value: " << row.value;
            written.push_back(std::move(row));
        }

        // After the rows are on disk, so a state never runs ahead of the data a reader can see it
        // was decided from.
        evaluateAlerts(written);
    }

    void EmoServer::evaluateAlerts(const std::vector<Database::Entity::Monitoring::MonitoringData> &rows) {

        if (rows.empty()) return;

        std::lock_guard lock(alertMutex());
        try {
            const auto repository = Database::RepositoryFactory::instance().emoRepository();

            std::vector<AlertRule> rules;
            for (auto &rule: repository->listAlertRules("")) {
                if (rule.enabled && rule.resolution == Resolution::RAW) rules.push_back(std::move(rule));
            }
            if (rules.empty()) return;

            // Every state at once rather than one read per series: a few hundred rows, against a
            // round trip per matched series every period.
            std::map<std::tuple<std::string, std::string, std::string>, AlertState> states;
            std::map<std::pair<std::string, std::string>, long> seriesPerRule;
            for (auto &state: repository->listAlertStates("", "")) {
                ++seriesPerRule[{state.accountId, state.ruleId}];
                states.emplace(std::make_tuple(state.accountId, state.ruleId, state.labelKey), std::move(state));
            }

            const auto cap = maxSeriesPerRule();
            const auto now = std::chrono::system_clock::now();
            for (const auto &rule: rules) {
                bool capped = false;
                for (const auto &row: rows) {
                    if (row.name != rule.name || !rule.matches(row.labels)) continue;

                    const auto key = std::make_tuple(rule.accountId, rule.ruleId, row.labelKey());
                    auto found = states.find(key);
                    if (found == states.end()) {
                        // Refused at create when it already matched too many; a series that appears
                        // later is simply not taken on, and said once a period.
                        auto &count = seriesPerRule[{rule.accountId, rule.ruleId}];
                        if (cap > 0 && count >= cap) {
                            capped = true;
                            continue;
                        }
                        ++count;
                        AlertState fresh;
                        fresh.accountId = rule.accountId;
                        fresh.ruleId = rule.ruleId;
                        fresh.labelKey = row.labelKey();
                        fresh.labels = row.labels;
                        fresh.since = row.timestamp;
                        found = states.emplace(key, std::move(fresh)).first;
                    }

                    auto &state = found->second;
                    const auto change = EvaluateAlert(rule, state, AlertFigure(rule, row), row.timestamp);
                    if (!repository->upsertAlertState(state)) continue;
                    if (change) announceAlert(rule, state, *change, now);
                }
                if (capped) {
                    log_warning << "Alert rule matches more than " << cap << " series, the rest are not judged, ruleId: " << rule.ruleId
                                << ", metric: " << rule.name << " (euclid.modules.emo.alert.max-series-per-rule)";
                }
            }

        } catch (const std::exception &e) {
            // Runs on the scheduler's thread, with every other periodic task in this process.
            log_error << "Alert evaluation failed, error: " << e.what();
        }
    }

    void EmoServer::sweepAlerts() {

        std::lock_guard lock(alertMutex());
        try {
            const auto repository = Database::RepositoryFactory::instance().emoRepository();

            std::map<std::pair<std::string, std::string>, AlertRule> rules;
            for (auto &rule: repository->listAlertRules("")) {
                if (rule.enabled && rule.noDataAfterPeriods > 0) rules.emplace(std::make_pair(rule.accountId, rule.ruleId), std::move(rule));
            }
            if (rules.empty()) return;

            const auto now = std::chrono::system_clock::now();
            const auto period = averagePeriod();
            for (auto &state: repository->listAlertStates("", "")) {
                const auto rule = rules.find({state.accountId, state.ruleId});
                if (rule == rules.end()) continue;
                const auto change = MarkAlertNoData(rule->second, state, now, period);
                if (!change || !repository->upsertAlertState(state)) continue;
                announceAlert(rule->second, state, *change, now);
            }

        } catch (const std::exception &e) {
            log_error << "Alert sweep failed, error: " << e.what();
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
            log_warning << "Monitoring cpu-usage collection failed, system CPU times not readable";
            return;
        }

        std::lock_guard lock(cpuTimesMutex());
        const auto previous = previousCpuTimes();
        previousCpuTimes() = current;

        // The first call only primes previousCpuTimes() - a percentage needs a delta between tworeadings.
        if (!previous.has_value()) return;

        // The arithmetic is Core::SystemUtils::CpuUsagePercent's, because a worker reporting its
        // own host's figure with its heartbeat does the same sum - and two copies of it is how the
        // two hosts would come to disagree on a dashboard that shows them together.
        const auto usage = Core::SystemUtils::CpuUsagePercent(*previous, *current);
        if (!usage.has_value()) return;

        // Recorded as a sample rather than written straight out, so CPU usage is bucket-aligned
        // and carries a sample count and a min/max like every other metric, and so the rollups
        // can aggregate it at all.
        recordSample("system-cpu-usage", "host", Core::SystemUtils::GetHostName(), *usage, MetricType::GAUGE);
    }

    void EmoServer::collectSystemLoad() {

        const auto host = Core::SystemUtils::GetHostName();

#ifdef _WIN32
        // Windows keeps no run-queue averages, so there is no "system-load-average" to record here
        // and a zero in its place would read as an idle machine. What it does keep is the
        // instantaneous queue - see Core::SystemUtils::ReadProcessorQueueLength() - recorded under
        // its own names so that nothing reading "system-load-average" is handed a figure that is
        // averaged over nothing.
        const auto queue = Core::SystemUtils::ReadProcessorQueueLength();
        if (!queue.has_value()) {
            log_warning << R"(Monitoring system-load collection failed, "\System\Processor Queue Length" not readable)";
            return;
        }

        // The averaging EMO already does over each bucket is what makes this readable: one sample a
        // minute of an instantaneous count would be noise, where the mean and max over a five-minute
        // bucket say how much was waiting and how bad it got.
        recordSample("system-processor-queue-length", "host", host, queue->queueLength, MetricType::GAUGE);

        // The comparable figure, for the same reason as system-load-per-core below: a queue of 8 is
        // a third of a 24-processor host and four times a two-processor one. The usual threshold is
        // a sustained 2 per processor, which is what makes this the series worth alerting on.
        if (queue->cpuCount > 0) {
            const auto perCore = queue->queueLength / static_cast<double>(queue->cpuCount);
            recordSample("system-processor-queue-per-core", "host", host, perCore, MetricType::GAUGE);
        }
#else
        const auto load = Core::SystemUtils::ReadLoadAverage();
        if (!load.has_value()) {
            log_warning << "Monitoring system-load collection failed, /proc/loadavg not readable";
            return;
        }

        // Three series rather than one, labelled by the window they average over, so a dashboard
        // can put the 1-minute figure against the 15-minute one - which is the whole point of the
        // kernel reporting all three. Spiking or subsiding is the difference between them.
        recordSample("system-load-average", {{"host", host}, {"interval", "1m"}}, load->oneMinute, MetricType::GAUGE);
        recordSample("system-load-average", {{"host", host}, {"interval", "5m"}}, load->fiveMinutes, MetricType::GAUGE);
        recordSample("system-load-average", {{"host", host}, {"interval", "15m"}}, load->fifteenMinutes, MetricType::GAUGE);

        // The same figure divided by the number of CPUs, because the raw one cannot be compared
        // across hosts or even read without knowing the machine: a load of 8 is a third of a
        // 24-core host and four times a two-core one. One is the saturation point here, whatever
        // the hardware, which is what makes this the series worth alerting on.
        if (load->cpuCount > 0) {
            const auto perCore = load->oneMinute / static_cast<double>(load->cpuCount);
            recordSample("system-load-per-core", "host", host, perCore, MetricType::GAUGE);
        }
#endif
    }

    void EmoServer::collectModuleInstances() {

        for (const auto &module: Database::RepositoryFactory::instance().emmRepository()->findAll()) {

            // The mean-and-total rule lives with the entity so it can be tested - see Entity::SummarisePool().
            const auto load = Database::Entity::SummarisePool(module);

            // Recorded even at zero, and this is the one that has to be: a pool that has scaled to
            // nothing is exactly what somebody reading the graph is looking for, and a series that
            // simply stops saying anything is indistinguishable from a collector that died.
            recordSample("module-instances", "module", module.name, static_cast<double>(load.running), MetricType::GAUGE);

            // Nothing reported, so there is no load to record - as distinct from a load of zero.
            if (load.reporting == 0) continue;

            recordSample("module-utilisation", "module", module.name, load.utilisation, MetricType::GAUGE);
            recordSample("module-backlog", "module", module.name, static_cast<double>(load.backlog), MetricType::GAUGE);
        }
    }

    void EmoServer::collectMemoryUsage() {

        // The machine's memory, labelled by host, beside the machine's CPU. The per-module
        // "euclid-memory-usage-percent" is a different question - what one process holds - and
        // summing or averaging those never answered this one.
        const auto usage = Core::SystemUtils::ReadSystemMemoryUsagePercent();
        if (!usage.has_value()) {
            log_warning << "Monitoring memory-usage collection failed, system memory not readable";
            return;
        }

        recordSample("system-memory-usage-percent", "host", Core::SystemUtils::GetHostName(), *usage, MetricType::GAUGE);
    }

    void EmoServer::collectDatabaseSize() {

        const auto stats = Database::RepositoryFactory::instance().emoRepository()->databaseStats();
        if (!stats.has_value()) {
            // The in-memory backend has no sizes to report. Not a warning: it is a configuration,
            // not a failure, and one that would otherwise log every five minutes forever.
            log_debug << "Monitoring database-size collection skipped, backend reports no sizes";
            return;
        }

        // Labelled by database rather than by host: several euclid hosts can share one database,
        // and the figure belongs to the database in every case.
        const auto name = Core::Configuration::instance().getOr<std::string>("euclid.mongodb.name", "euclid");

        // Both sizes, because they answer different questions and either one alone misleads:
        // data-size is what euclid holds, storage-size what that costs on disk after compression -
        // typically several times smaller - and total-size is the one that fills a volume.
        recordSample("database-data-size", "database", name, static_cast<double>(stats->dataSize), MetricType::GAUGE);
        recordSample("database-storage-size", "database", name, static_cast<double>(stats->storageSize), MetricType::GAUGE);
        recordSample("database-index-size", "database", name, static_cast<double>(stats->indexSize), MetricType::GAUGE);
        recordSample("database-total-size", "database", name, static_cast<double>(stats->totalSize), MetricType::GAUGE);
        recordSample("database-objects", "database", name, static_cast<double>(stats->objects), MetricType::GAUGE);
        recordSample("database-collections", "database", name, static_cast<double>(stats->collections), MetricType::GAUGE);
    }

    void EmoServer::collectDiskUsage() {

        // One walk at a time. A period shorter than a walk would otherwise start a second over the
        // same files while the first is still going, and each would slow the other down.
        if (s_diskWalkRunning.exchange(true)) {
            log_debug << "Monitoring disk-usage skipped, the previous walk is still running";
            return;
        }
        struct Done {
            ~Done() { s_diskWalkRunning.store(false); }
        } done;

        // One EMO instance per period: every instance on the host sees the same directories, and
        // walking them once is already the expensive part. This is also what holds walks to the
        // period - the task asks far more often than that.
        if (const auto period = std::chrono::seconds(Core::Configuration::instance().getOr<long>("euclid.modules.emo.disk-usage-period", kDiskUsagePeriod.count())); !Database::ClaimPeriodicRun("emo-disk-usage", period)) return;

#if defined(__linux__)
        lowerIoPriority();
#endif

        for (const auto &[module, directory]: moduleDataDirectories()) {
            const auto started = std::chrono::steady_clock::now();
            const auto usage = Core::DirUtils::MeasureDiskUsage(directory, &s_stopping);
            if (s_stopping.load()) return;
            const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

            // A walk that could not finish has counted part of the directory, and a part would read
            // as the directory shrinking. Said, and not recorded.
            if (!usage.complete) {
                log_warning << "Monitoring disk-usage could not walk all of " << directory.string() << ", module: " << module << ", unreadable entries: " << usage.unreadable;
                continue;
            }
            recordSample("module-disk-usage", "module", module, static_cast<double>(usage.bytes), MetricType::GAUGE);
            recordSample("module-disk-files", "module", module, static_cast<double>(usage.files), MetricType::GAUGE);
            recordSample("module-disk-scan-seconds", "module", module, seconds, MetricType::GAUGE);
            log_info << "Monitoring disk-usage, module: " << module << ", bytes: " << usage.bytes << ", files: " << usage.files
                     << ", seconds: " << static_cast<long>(seconds);
        }
    }

    void EmoServer::collectDiskSpace() {

        const auto root = Core::Configuration::instance().getOr<std::string>("euclid.data-dir", "/usr/local/euclid/data");
        std::error_code ec;
        const auto space = std::filesystem::space(root, ec);
        if (ec) {
            log_warning << "Monitoring disk-space could not read the volume of " << root << ", error: " << ec.message();
            return;
        }
        // Free and available differ by what the filesystem keeps back for root: available is what
        // euclid can actually still write, and the one to alert on.
        recordSample("disk-capacity", "path", root, static_cast<double>(space.capacity), MetricType::GAUGE);
        recordSample("disk-free", "path", root, static_cast<double>(space.free), MetricType::GAUGE);
        recordSample("disk-available", "path", root, static_cast<double>(space.available), MetricType::GAUGE);
    }

    response<string_body> EmoServer::DispatchAction(const request<string_body> &req) {
        return dispatch(req);
    }

}// namespace Euclid::Monitoring