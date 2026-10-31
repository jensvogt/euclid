// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 8/18/26.
//

// C++ includes
#include <algorithm>
#include <map>
#include <mutex>
#include <ranges>
#include <set>

// Euclid includes
#include <euclid/database/repository/emo/MongoEmoRepository.h>

namespace Euclid::Database {

    using Entity::Monitoring::MetricType;
    using Entity::Monitoring::MetricTypeToString;
    using Entity::Monitoring::Resolution;
    using Entity::Monitoring::ResolutionToString;

    // The rollup pipeline is long enough that fully qualified builder calls stop being readable.
    namespace {

        using bsoncxx::builder::basic::kvp;
        using bsoncxx::builder::basic::make_array;
        using bsoncxx::builder::basic::make_document;

        bsoncxx::types::b_date toDate(const std::chrono::system_clock::time_point &timestamp) {
            return bsoncxx::types::b_date{std::chrono::duration_cast<std::chrono::milliseconds>(timestamp.time_since_epoch())};
        }

        // $dateTrunc unit for a target resolution. RAW is never a rollup target - its buckets come
        // straight from the flush task - so it has no unit here.
        std::string dateTruncUnit(const Resolution &resolution) {
            return resolution == Resolution::DAY ? "day" : "hour";
        }

        // A literal for a pattern: a label value is somebody's string - a queue name, a URI, a
        // JVM memory pool called "G1 Eden Space" - and every one of these characters means
        // something else to a regular expression.
        std::string quoteRegex(const std::string &value) {
            static constexpr std::string_view special = R"(\^$.|?*+()[]{})";
            std::string quoted;
            quoted.reserve(value.size() * 2);
            for (const char c: value) {
                if (special.find(c) != std::string_view::npos) quoted += '\\';
                quoted += c;
            }
            return quoted;
        }

        // Equality filters plus the time range shared by list() and average().
        bsoncxx::builder::basic::document queryFilter(const MonitoringQuery &query) {

            bsoncxx::builder::basic::document filter{};
            filter.append(kvp("resolution", ResolutionToString(query.resolution)));
            if (!query.name.empty()) filter.append(kvp("name", query.name));

            // Every named dimension has to match, and a row may carry more than was asked for: a
            // caller narrowing to {"area": "heap"} wants the heap series whatever else labels it.
            for (const auto &[name, value]: query.labels) {
                filter.append(kvp("labels." + name, value));
            }
            if (!query.labelName.empty() && !query.labelValue.empty()) {
                filter.append(kvp("labels." + query.labelName, query.labelValue));
            } else if (!query.labelName.empty()) {
                filter.append(kvp("labels." + query.labelName, make_document(kvp("$exists", true))));
            } else if (!query.labelValue.empty()) {
                // A value with no dimension named: "whichever label carries this". Kept because it
                // was always possible, and answered against labelKey rather than by unpacking the
                // map, because "k1=v1;k2=v2" is a string both backends can match a pattern against
                // - $expr and $objectToArray would work on MongoDB and throw on the in-memory one.
                // Anchored on both sides so a value does not match a longer one it is a prefix of.
                filter.append(kvp("labelKey", make_document(kvp("$regex", "(^|;)[^;=]*=" + quoteRegex(query.labelValue) + "($|;)"))));
            }

            if (constexpr auto epoch = std::chrono::system_clock::time_point{}; query.from != epoch || query.to != epoch) {
                bsoncxx::builder::basic::document range{};
                if (query.from != epoch) range.append(kvp("$gte", toDate(query.from)));
                if (query.to != epoch) range.append(kvp("$lt", toDate(query.to)));
                filter.append(kvp("timestamp", range.extract()));
            }
            return filter;
        }

        // Monitoring is the one part of euclid that is not offered on an in-memory installation,
        // and it is worth being plain about why. Both derived figures here are aggregation
        // pipelines - a sample-weighted mean, and a rollup that groups by "$dateTrunc" and "$merge"s
        // the coarser tier back into the collection it read. Reimplementing those in C++ would put
        // a second implementation of exactly the arithmetic most likely to disagree with the first
        // behind the same interface, and the disagreement would show up as metrics that are subtly
        // wrong rather than absent - which is the worse failure for a monitoring module.
        //
        // So the samples are still collected, still written and still listed; only the tiers
        // computed from them are not. Said once per process rather than per firing, because the
        // rollup runs on a timer and would otherwise report the same thing every minute forever.
        bool aggregationAvailable(const Collection &collection, const char *what) {

            if (collection.supports_aggregation()) return true;

            static std::once_flag once;
            std::call_once(once, [] {
                log_warning << "Monitoring aggregation needs MongoDB, no averages or rollups on the in-memory backend";
            });
            log_debug << "Monitoring " << what << " skipped, the in-memory backend has no aggregation";
            return false;
        }

    }// namespace

    MongoEmoRepository::MongoEmoRepository() {
        // Deferred rather than done here: on the EMD backend a repository can be constructed before
        // the module that holds the store is listening, and a constructor gets no second chance.
        Database::instance().onReachable(&ensureIndexes);
    }

    void MongoEmoRepository::ensureIndexes() {

        try {
            auto collection = Database::instance().collection(COLLECTION);

            // The key this replaced, from before a series was identified by a label map. Dropped
            // rather than left alone: it is unique over labelName/labelValue, fields a row written
            // now does not carry at all, and a missing field indexes as null - so the second row
            // of any bucket would collide with the first and be refused.
            collection.drop_index("name_1_labelName_1_labelValue_1_resolution_1_timestamp_1");

            // Identity of a data point: one series, one resolution, one bucket. Unique because
            // both writers upsert on exactly these fields, and because rollup()'s $merge requires
            // a unique index on its "on" fields. Also covers list()'s equality filters plus its
            // timestamp sort, which MongoDB can walk backwards for the descending order.
            mongocxx::options::index bucketOptions;
            bucketOptions.unique(true);
            collection.create_index(make_document(kvp("name", 1), kvp("labelKey", 1),
                                                  kvp("resolution", 1), kvp("timestamp", 1)),
                                    bucketOptions);

            // Serves a whole-tier scan over a time range - the shape a graph asks for, and the
            // shape rollup() reads - which the index above cannot, since name leads it.
            collection.create_index(make_document(kvp("resolution", 1), kvp("timestamp", -1)));

            // Retention. expireAfterSeconds 0 means "expire at the value of the field", so each
            // row expires on its own expiresAt and every tier can have its own retention while
            // living in one collection. MongoDB's TTL monitor does the deleting, roughly once a
            // minute; deleteExpired() stays as a portable fallback for backends without TTL.
            mongocxx::options::index ttlOptions;
            ttlOptions.expire_after(std::chrono::seconds(0));
            collection.create_index(make_document(kvp("expiresAt", 1)), ttlOptions);

            // Identity of a saved dashboard, and the filter every one of its reads uses. Unique
            // over the pair rather than over the name, so that two users may each keep an
            // "Overview" while neither can end up with two of their own. Leading with userId also
            // serves listDashboards(), which asks for one user and nothing else.
            auto dashboards = Database::instance().collection(DASHBOARD_COLLECTION);
            mongocxx::options::index dashboardOptions;
            dashboardOptions.unique(true);
            dashboards.create_index(make_document(kvp("userId", 1), kvp("name", 1)), dashboardOptions);

            // A rule is the account's ruleId; unique so a second create cannot make two of one.
            // {accountId, name} serves a lookup by metric.
            mongocxx::options::index uniqueOptions;
            uniqueOptions.unique(true);
            auto rules = Database::instance().collection(ALERT_RULE_COLLECTION);
            rules.create_index(make_document(kvp("accountId", 1), kvp("ruleId", 1)), uniqueOptions);
            rules.create_index(make_document(kvp("accountId", 1), kvp("name", 1)));

            // One state per rule and series, which is what the evaluator upserts on.
            auto states = Database::instance().collection(ALERT_STATE_COLLECTION);
            states.create_index(make_document(kvp("accountId", 1), kvp("ruleId", 1), kvp("labelKey", 1)), uniqueOptions);

            // The history is read newest first, for one rule or for all of an account's, and
            // expires row by row like the data points.
            auto history = Database::instance().collection(ALERT_HISTORY_COLLECTION);
            history.create_index(make_document(kvp("accountId", 1), kvp("ruleId", 1), kvp("timestamp", -1)));
            history.create_index(make_document(kvp("accountId", 1), kvp("timestamp", -1)));
            history.create_index(make_document(kvp("expiresAt", 1)), ttlOptions);

        } catch (const std::exception &e) {
            log_error << "Ensure monitoring indexes failed, error: " << e.what();
        }
    }

    void MongoEmoRepository::upsert(const Entity::Monitoring::MonitoringData &data) {

        try {
            auto collection = Database::instance().collection(COLLECTION);

            const auto filter = make_document(kvp("name", data.name), kvp("labelKey", data.labelKey()),
                                              kvp("resolution", ResolutionToString(data.resolution)), kvp("timestamp", toDate(data.timestamp)));

            mongocxx::options::replace options;
            options.upsert(true);
            collection.replace_one(filter.view(), data.toDocument().view(), options);

        } catch (const std::exception &e) {
            log_error << "Upsert monitoring data failed, error: " << e.what();
        }
    }

    std::vector<Entity::Monitoring::MonitoringData> MongoEmoRepository::list(const MonitoringQuery &query) const {

        try {
            mongocxx::options::find opts;
            opts.sort(make_document(kvp("timestamp", -1)));
            if (query.limit > 0) opts.limit(query.limit);

            auto collection = Database::instance().collection(COLLECTION);

            std::vector<Entity::Monitoring::MonitoringData> result;
            for (auto cursor = collection.find(queryFilter(query).extract(), opts); const auto &doc: cursor) {
                result.push_back(Entity::Monitoring::MonitoringData::fromDocument(doc));
            }
            return result;

        } catch (const std::exception &e) {
            log_error << "List monitoring data failed, error: " << e.what();
            return {};
        }
    }

    double MongoEmoRepository::average(const MonitoringQuery &query) const {

        try {
            mongocxx::pipeline pipeline{};
            pipeline.match(queryFilter(query).extract());

            // Sample-weighted, for the same reason a rollup weights: buckets do not all carry the
            // same number of samples, so a plain mean of bucket means over-weights quiet buckets.
            pipeline.group(make_document(
                    kvp("_id", bsoncxx::types::b_null{}),
                    kvp("weighted", make_document(kvp("$sum", make_document(kvp("$multiply", make_array("$value", "$samples")))))),
                    kvp("samples", make_document(kvp("$sum", "$samples")))));

            auto collection = Database::instance().collection(COLLECTION);
            if (!aggregationAvailable(collection, "average")) return {};

            double result{};
            for (auto cursor = collection.aggregate(pipeline); const auto &doc: cursor) {
                if (const auto samples = doc["samples"].get_int64().value; samples > 0) {
                    result = doc["weighted"].get_double().value / static_cast<double>(samples);
                }
            }
            return result;

        } catch (const std::exception &e) {
            log_error << "Average monitoring data failed, error: " << e.what();
            return {};
        }
    }

    long MongoEmoRepository::rollup(const Resolution from, const Resolution to,
                                    const std::chrono::system_clock::time_point windowStart, const std::chrono::system_clock::time_point windowEnd,
                                    const std::chrono::seconds retention) {

        try {
            auto collection = Database::instance().collection(COLLECTION);
            if (!aggregationAvailable(collection, "rollup")) return 0;

            mongocxx::pipeline pipeline{};

            // Stage 1: $match - only the source tier, only the requested window. Note this reads
            // the same collection $merge writes to, which is safe precisely because the written
            // rows carry the target resolution and so can never match this filter.
            pipeline.match(make_document(
                    kvp("resolution", ResolutionToString(from)),
                    kvp("timestamp", make_document(kvp("$gte", toDate(windowStart)), kvp("$lt", toDate(windowEnd))))));

            // Stage 2: $group - one group per series per target bucket. type is part of the key
            // only so the next stage can branch on it; every row of a series carries the same one.
            pipeline.group(make_document(
                    kvp("_id", make_document(
                                kvp("name", "$name"),
                                // Grouped on the canonical key rather than the map: a $group _id is
                                // compared as a BSON value, so two identical maps stored in a
                                // different field order would roll up as two series.
                                kvp("labelKey", "$labelKey"),
                                kvp("type", "$type"),
                                kvp("bucket", make_document(kvp("$dateTrunc", make_document(kvp("date", "$timestamp"), kvp("unit", dateTruncUnit(to)))))))),
                    // The map itself is carried rather than grouped on: every row of a series has
                    // the same one, so the first is the series'.
                    kvp("labels", make_document(kvp("$first", "$labels"))),
                    kvp("total", make_document(kvp("$sum", "$value"))),
                    kvp("weighted", make_document(kvp("$sum", make_document(kvp("$multiply", make_array("$value", "$samples")))))),
                    kvp("samples", make_document(kvp("$sum", "$samples"))),
                    kvp("minValue", make_document(kvp("$min", "$minValue"))),
                    kvp("maxValue", make_document(kvp("$max", "$maxValue")))));

            // Stage 3: $project - reshape into a MonitoringData row. A RATE is the total of its
            // source buckets, a GAUGE their sample-weighted mean; the $gt guard keeps a source row
            // written with no samples from turning the whole bucket into a division by zero.
            pipeline.project(make_document(
                    kvp("_id", 0),
                    kvp("name", "$_id.name"),
                    kvp("labelKey", "$_id.labelKey"),
                    kvp("labels", "$labels"),
                    kvp("type", "$_id.type"),
                    kvp("resolution", ResolutionToString(to)),
                    kvp("timestamp", "$_id.bucket"),
                    kvp("value", make_document(kvp("$cond", make_array(
                                                           make_document(kvp("$eq", make_array("$_id.type", MetricTypeToString(MetricType::RATE)))),
                                                           "$total",
                                                           make_document(kvp("$cond", make_array(
                                                                                     make_document(kvp("$gt", make_array("$samples", 0))),
                                                                                     make_document(kvp("$divide", make_array("$weighted", "$samples"))),
                                                                                     0.0))))))),
                    kvp("samples", "$samples"),
                    kvp("minValue", "$minValue"),
                    kvp("maxValue", "$maxValue"),
                    kvp("expiresAt", make_document(kvp("$add", make_array("$_id.bucket", static_cast<std::int64_t>(std::chrono::milliseconds(retention).count())))))));

            // Stage 4: $merge - write the buckets back into the same collection, replacing rather
            // than appending. This is what makes the rollup idempotent: re-running an overlapping
            // window, whether after a crash or simply to refresh the still-incomplete current
            // bucket, converges on the same rows instead of duplicating them.
            pipeline.merge(make_document(
                    kvp("into", COLLECTION),
                    kvp("on", make_array("name", "labelKey", "resolution", "timestamp")),
                    kvp("whenMatched", "replace"),
                    kvp("whenNotMatched", "insert")));

            // $merge is a terminal stage and yields no documents, so the cursor is drained purely
            // to run the pipeline; the count comes from a separate indexed count afterwards.
            for (auto cursor = collection.aggregate(pipeline);  [[maybe_unused]] const auto &doc: cursor) {}

            const auto written = collection.count_documents(make_document(
                    kvp("resolution", ResolutionToString(to)),
                    kvp("timestamp", make_document(kvp("$gte", toDate(Entity::Monitoring::AlignDown(windowStart, ResolutionBucket(to)))),
                                                   kvp("$lt", toDate(windowEnd))))));

            log_debug << "Monitoring rolled up, from: " << ResolutionToString(from) << ", to: " << ResolutionToString(to) << ", buckets: " << written;
            return static_cast<long>(written);

        } catch (const std::exception &e) {
            log_error << "Rollup monitoring data failed, error: " << e.what();
            return 0;
        }
    }

    long MongoEmoRepository::deleteExpired(const std::chrono::system_clock::time_point now) {

        try {
            auto collection = Database::instance().collection(COLLECTION);

            const auto result = collection.delete_many(make_document(kvp("expiresAt", make_document(kvp("$lt", toDate(now))))).view());
            const auto deleted = result ? static_cast<long>(result->deleted_count()) : 0;

            log_debug << "Pruned monitoring data, count: " << deleted;
            return deleted;

        } catch (const std::exception &e) {
            log_error << "Prune monitoring data failed, error: " << e.what();
            return 0;
        }
    }


    std::optional<IEmoRepository::DatabaseStats> MongoEmoRepository::databaseStats() const {

        try {
            // The in-memory store keeps no size counters - it has no files, no indexes and no
            // storage engine to ask - so what can be answered is answered and the rest is left at
            // zero, rather than the whole metric disappearing on that backend.
            if (Database::instance().inMemory()) {
                const auto store = Database::instance().store();
                DatabaseStats stats;
                for (const auto &collection: store->Collections()) {
                    stats.collections++;
                    stats.objects += store->Size(collection);
                }
                return stats;
            }

            const auto entry = Database::instance().client();
            auto database = (*entry)[Database::instance().databaseName()];

            // "scale" 1 so the sizes come back in bytes rather than the server's default, which a
            // reader would otherwise have to know about to interpret the numbers at all.
            const auto stats = database.run_command(make_document(kvp("dbStats", 1), kvp("scale", 1)));
            const auto view = stats.view();

            // dbStats returns whichever numeric type each figure happens to fit: the counts come
            // back as int64 and the sizes as double on a database of any size, but both are int32
            // on an empty one. Asking for the wrong one throws, so none is assumed.
            auto asLong = [&view](const char *field) -> long {
                const auto element = view[field];
                if (!element) return 0;
                switch (element.type()) {
                    case bsoncxx::type::k_int64:
                        return static_cast<long>(element.get_int64().value);
                    case bsoncxx::type::k_int32:
                        return element.get_int32().value;
                    case bsoncxx::type::k_double:
                        return static_cast<long>(element.get_double().value);
                    default:
                        return 0;
                }
            };

            return DatabaseStats{
                    .collections = asLong("collections"),
                    .objects = asLong("objects"),
                    .dataSize = asLong("dataSize"),
                    .storageSize = asLong("storageSize"),
                    .indexSize = asLong("indexSize"),
                    .totalSize = asLong("totalSize"),
            };

        } catch (const std::exception &e) {
            log_error << "Database stats failed, error: " << e.what();
        }
        return std::nullopt;
    }

    // ── Saved dashboards ──────────────────────────────────────────────────────
    //
    // Every one of these filters on userId as well as name, including the ones that already have a
    // name to go on. Not belt and braces: a name is not unique across the collection - two users
    // may each keep an "Overview" - so a get or a delete that trusted the name alone would hand
    // over, or destroy, whichever of them the index happened to reach first.

    void MongoEmoRepository::upsertDashboard(const Entity::Monitoring::Dashboard &dashboard) {

        try {
            auto collection = Database::instance().collection(DASHBOARD_COLLECTION);

            const auto filter = make_document(kvp("userId", dashboard.userId), kvp("name", dashboard.name));

            mongocxx::options::replace options;
            options.upsert(true);
            collection.replace_one(filter.view(), dashboard.toDocument().view(), options);

        } catch (const std::exception &e) {
            log_error << "Upsert dashboard failed, error: " << e.what();
        }
    }

    std::optional<Entity::Monitoring::Dashboard> MongoEmoRepository::getDashboard(const std::string &userId, const std::string &name) const {

        try {
            auto collection = Database::instance().collection(DASHBOARD_COLLECTION);

            if (const auto doc = collection.find_one(make_document(kvp("userId", userId), kvp("name", name)).view())) {
                return Entity::Monitoring::Dashboard::fromDocument(doc->view());
            }
            // Not an error: "this user has no dashboard by that name" is an ordinary answer, and
            // the caller is what turns it into a 404.
            return std::nullopt;

        } catch (const std::exception &e) {
            log_error << "Get dashboard failed, error: " << e.what();
            return std::nullopt;
        }
    }

    std::vector<Entity::Monitoring::Dashboard> MongoEmoRepository::listDashboards(const std::string &userId) const {

        try {
            mongocxx::options::find opts;
            opts.sort(make_document(kvp("modified", -1)));

            auto collection = Database::instance().collection(DASHBOARD_COLLECTION);

            std::vector<Entity::Monitoring::Dashboard> result;
            for (auto cursor = collection.find(make_document(kvp("userId", userId)).view(), opts); const auto &doc: cursor) {
                result.push_back(Entity::Monitoring::Dashboard::fromDocument(doc));
            }
            return result;

        } catch (const std::exception &e) {
            log_error << "List dashboards failed, error: " << e.what();
            return {};
        }
    }

    long MongoEmoRepository::deleteDashboard(const std::string &userId, const std::string &name) {

        try {
            auto collection = Database::instance().collection(DASHBOARD_COLLECTION);

            const auto result = collection.delete_one(make_document(kvp("userId", userId), kvp("name", name)).view());
            return result ? static_cast<long>(result->deleted_count()) : 0;

        } catch (const std::exception &e) {
            log_error << "Delete dashboard failed, error: " << e.what();
            return 0;
        }
    }

    std::vector<MetricSummary> MongoEmoRepository::listMetrics(const std::chrono::system_clock::time_point since) const {

        try {
            auto collection = Database::instance().collection(COLLECTION);
            const auto filter = make_document(kvp("resolution", ResolutionToString(Resolution::RAW)),
                                              kvp("timestamp", make_document(kvp("$gte", toDate(since)))));

            // One entry per series, folded into one per name below. Grouped by series rather than
            // by name so the label names can be read off a series' labels here instead of being
            // unpacked from a map in the pipeline, which the in-memory store could not run anyway.
            struct Series {
                std::string name;
                std::map<std::string, std::string> labels;
                MetricType type{MetricType::GAUGE};
                std::chrono::system_clock::time_point lastSeen{};
            };
            std::vector<Series> series;

            if (collection.supports_aggregation()) {
                mongocxx::pipeline pipeline{};
                pipeline.match(filter.view());
                pipeline.group(make_document(
                        kvp("_id", make_document(kvp("name", "$name"), kvp("labelKey", "$labelKey"))),
                        kvp("labels", make_document(kvp("$first", "$labels"))),
                        kvp("type", make_document(kvp("$last", "$type"))),
                        kvp("lastSeen", make_document(kvp("$max", "$timestamp")))));
                for (auto cursor = collection.aggregate(pipeline); const auto &doc: cursor) {
                    Series one;
                    one.name = std::string(doc["_id"]["name"].get_string().value);
                    if (const auto labels = doc["labels"]; labels && labels.type() == bsoncxx::type::k_document) {
                        for (const auto &label: labels.get_document().value) {
                            if (label.type() == bsoncxx::type::k_string) one.labels[std::string(label.key())] = std::string(label.get_string().value);
                        }
                    }
                    if (const auto type = doc["type"]; type && type.type() == bsoncxx::type::k_string) {
                        one.type = Entity::Monitoring::MetricTypeFromString(std::string(type.get_string().value));
                    }
                    if (const auto last = doc["lastSeen"]; last && last.type() == bsoncxx::type::k_date) {
                        one.lastSeen = std::chrono::system_clock::time_point{last.get_date().value};
                    }
                    series.push_back(std::move(one));
                }
            } else {
                // The in-memory store: the same answer from the rows themselves.
                std::map<std::pair<std::string, std::string>, Series> seen;
                for (auto cursor = collection.find(filter.view()); const auto &doc: cursor) {
                    const auto row = Entity::Monitoring::MonitoringData::fromDocument(doc);
                    auto &one = seen[{row.name, row.labelKey()}];
                    one.name = row.name;
                    one.labels = row.labels;
                    one.type = row.type;
                    one.lastSeen = std::max(one.lastSeen, row.timestamp);
                }
                for (auto &one: seen | std::views::values) series.push_back(std::move(one));
            }

            std::map<std::string, MetricSummary> byName;
            std::map<std::string, std::set<std::string> > labelNames;
            for (const auto &one: series) {
                auto &summary = byName[one.name];
                summary.name = one.name;
                summary.type = one.type;
                ++summary.series;
                summary.lastSeen = std::max(summary.lastSeen, one.lastSeen);
                for (const auto &label: one.labels | std::views::keys) labelNames[one.name].insert(label);
            }

            std::vector<MetricSummary> result;
            result.reserve(byName.size());
            for (auto &[name, summary]: byName) {
                summary.labelNames.assign(labelNames[name].begin(), labelNames[name].end());
                result.push_back(std::move(summary));
            }
            return result;

        } catch (const std::exception &e) {
            log_error << "List metrics failed, error: " << e.what();
            return {};
        }
    }

    // ── Alerting ─────────────────────────────────────────────────────────────

    namespace {
        // {accountId, ruleId}, either left out when empty.
        bsoncxx::document::value ruleFilter(const std::string &accountId, const std::string &ruleId) {
            bsoncxx::builder::basic::document filter{};
            if (!accountId.empty()) filter.append(kvp("accountId", accountId));
            if (!ruleId.empty()) filter.append(kvp("ruleId", ruleId));
            return filter.extract();
        }
    }// namespace

    bool MongoEmoRepository::upsertAlertRule(const Entity::Monitoring::AlertRule &rule) {

        try {
            auto collection = Database::instance().collection(ALERT_RULE_COLLECTION);
            mongocxx::options::replace options;
            options.upsert(true);
            collection.replace_one(make_document(kvp("accountId", rule.accountId), kvp("ruleId", rule.ruleId)).view(), rule.toDocument().view(), options);
            return true;

        } catch (const std::exception &e) {
            log_error << "Upsert alert rule failed, ruleId: " << rule.ruleId << ", error: " << e.what();
            return false;
        }
    }

    std::optional<Entity::Monitoring::AlertRule> MongoEmoRepository::getAlertRule(const std::string &accountId, const std::string &ruleId) const {

        try {
            auto collection = Database::instance().collection(ALERT_RULE_COLLECTION);
            if (const auto doc = collection.find_one(make_document(kvp("accountId", accountId), kvp("ruleId", ruleId)).view())) {
                return Entity::Monitoring::AlertRule::fromDocument(doc->view());
            }
            return std::nullopt;

        } catch (const std::exception &e) {
            log_error << "Get alert rule failed, ruleId: " << ruleId << ", error: " << e.what();
            return std::nullopt;
        }
    }

    std::vector<Entity::Monitoring::AlertRule> MongoEmoRepository::listAlertRules(const std::string &accountId) const {

        try {
            mongocxx::options::find opts;
            opts.sort(make_document(kvp("accountId", 1), kvp("ruleId", 1)));

            auto collection = Database::instance().collection(ALERT_RULE_COLLECTION);
            std::vector<Entity::Monitoring::AlertRule> result;
            for (auto cursor = collection.find(ruleFilter(accountId, ""), opts); const auto &doc: cursor) {
                result.push_back(Entity::Monitoring::AlertRule::fromDocument(doc));
            }
            return result;

        } catch (const std::exception &e) {
            log_error << "List alert rules failed, error: " << e.what();
            return {};
        }
    }

    long MongoEmoRepository::deleteAlertRule(const std::string &accountId, const std::string &ruleId) {

        try {
            auto collection = Database::instance().collection(ALERT_RULE_COLLECTION);
            const auto result = collection.delete_one(make_document(kvp("accountId", accountId), kvp("ruleId", ruleId)).view());
            return result ? static_cast<long>(result->deleted_count()) : 0;

        } catch (const std::exception &e) {
            log_error << "Delete alert rule failed, ruleId: " << ruleId << ", error: " << e.what();
            return 0;
        }
    }

    std::vector<Entity::Monitoring::AlertState> MongoEmoRepository::listAlertStates(const std::string &accountId, const std::string &ruleId) const {

        try {
            mongocxx::options::find opts;
            opts.sort(make_document(kvp("accountId", 1), kvp("ruleId", 1), kvp("labelKey", 1)));

            auto collection = Database::instance().collection(ALERT_STATE_COLLECTION);
            std::vector<Entity::Monitoring::AlertState> result;
            for (auto cursor = collection.find(ruleFilter(accountId, ruleId), opts); const auto &doc: cursor) {
                result.push_back(Entity::Monitoring::AlertState::fromDocument(doc));
            }
            return result;

        } catch (const std::exception &e) {
            log_error << "List alert states failed, error: " << e.what();
            return {};
        }
    }

    bool MongoEmoRepository::upsertAlertState(const Entity::Monitoring::AlertState &state) {

        try {
            auto collection = Database::instance().collection(ALERT_STATE_COLLECTION);
            mongocxx::options::replace options;
            options.upsert(true);
            collection.replace_one(make_document(kvp("accountId", state.accountId), kvp("ruleId", state.ruleId), kvp("labelKey", state.labelKey)).view(),
                                   state.toDocument().view(), options);
            return true;

        } catch (const std::exception &e) {
            log_error << "Upsert alert state failed, ruleId: " << state.ruleId << ", series: " << state.labelKey << ", error: " << e.what();
            return false;
        }
    }

    long MongoEmoRepository::deleteAlertStates(const std::string &accountId, const std::string &ruleId) {

        try {
            auto collection = Database::instance().collection(ALERT_STATE_COLLECTION);
            const auto result = collection.delete_many(make_document(kvp("accountId", accountId), kvp("ruleId", ruleId)).view());
            return result ? static_cast<long>(result->deleted_count()) : 0;

        } catch (const std::exception &e) {
            log_error << "Delete alert states failed, ruleId: " << ruleId << ", error: " << e.what();
            return 0;
        }
    }

    bool MongoEmoRepository::insertAlertTransition(const Entity::Monitoring::AlertTransition &transition) {

        try {
            Database::instance().collection(ALERT_HISTORY_COLLECTION).insert_one(transition.toDocument().view());
            return true;

        } catch (const std::exception &e) {
            log_error << "Insert alert transition failed, ruleId: " << transition.ruleId << ", error: " << e.what();
            return false;
        }
    }

    std::vector<Entity::Monitoring::AlertTransition> MongoEmoRepository::listAlertHistory(const AlertHistoryQuery &query) const {

        try {
            bsoncxx::builder::basic::document filter{};
            if (!query.accountId.empty()) filter.append(kvp("accountId", query.accountId));
            if (!query.ruleId.empty()) filter.append(kvp("ruleId", query.ruleId));
            if (constexpr auto epoch = std::chrono::system_clock::time_point{}; query.from != epoch || query.to != epoch) {
                bsoncxx::builder::basic::document range{};
                if (query.from != epoch) range.append(kvp("$gte", toDate(query.from)));
                if (query.to != epoch) range.append(kvp("$lt", toDate(query.to)));
                filter.append(kvp("timestamp", range.extract()));
            }

            mongocxx::options::find opts;
            opts.sort(make_document(kvp("timestamp", -1)));
            if (query.limit > 0) opts.limit(query.limit);

            auto collection = Database::instance().collection(ALERT_HISTORY_COLLECTION);
            std::vector<Entity::Monitoring::AlertTransition> result;
            for (auto cursor = collection.find(filter.extract(), opts); const auto &doc: cursor) {
                result.push_back(Entity::Monitoring::AlertTransition::fromDocument(doc));
            }
            return result;

        } catch (const std::exception &e) {
            log_error << "List alert history failed, error: " << e.what();
            return {};
        }
    }

}// namespace Euclid::Database
