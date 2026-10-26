// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// C++ includes
#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#if defined(__linux__)
#include <cerrno>
#include <sys/syscall.h>
#include <unistd.h>
#endif

// Boost includes
#include <boost/beast/core.hpp>

// Euclid includes
#include <EmoServer.h>
#include <euclid/core/Configuration.h>
#include <euclid/core/DateTimeUtils.h>
#include <euclid/core/DirUtils.h>
#include <euclid/database/PeriodicClaim.h>
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