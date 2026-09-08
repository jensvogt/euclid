//
// Created by vogje01 on 8/18/26.
//

// C++ includes
#include <mutex>

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

        // Equality filters plus the time range shared by list() and average().
        bsoncxx::builder::basic::document queryFilter(const MonitoringQuery &query) {

            bsoncxx::builder::basic::document filter{};
            filter.append(kvp("resolution", ResolutionToString(query.resolution)));
            if (!query.name.empty()) filter.append(kvp("name", query.name));
            if (!query.labelName.empty()) filter.append(kvp("labelName", query.labelName));
            if (!query.labelValue.empty()) filter.append(kvp("labelValue", query.labelValue));

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
        ensureIndexes();
    }

    void MongoEmoRepository::ensureIndexes() {

        try {
            auto collection = Database::instance().collection(COLLECTION);

            // Identity of a data point: one series, one resolution, one bucket. Unique because
            // both writers upsert on exactly these fields, and because rollup()'s $merge requires
            // a unique index on its "on" fields. Also covers list()'s equality filters plus its
            // timestamp sort, which MongoDB can walk backwards for the descending order.
            mongocxx::options::index bucketOptions;
            bucketOptions.unique(true);
            collection.create_index(make_document(kvp("name", 1), kvp("labelName", 1), kvp("labelValue", 1),
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

        } catch (const std::exception &e) {
            log_error << "Ensure monitoring indexes failed, error: " << e.what();
        }
    }

    void MongoEmoRepository::upsert(const Entity::Monitoring::MonitoringData &data) {

        try {
            auto collection = Database::instance().collection(COLLECTION);

            const auto filter = make_document(kvp("name", data.name), kvp("labelName", data.labelName), kvp("labelValue", data.labelValue),
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
                                kvp("labelName", "$labelName"),
                                kvp("labelValue", "$labelValue"),
                                kvp("type", "$type"),
                                kvp("bucket", make_document(kvp("$dateTrunc", make_document(kvp("date", "$timestamp"), kvp("unit", dateTruncUnit(to)))))))),
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
                    kvp("labelName", "$_id.labelName"),
                    kvp("labelValue", "$_id.labelValue"),
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
                    kvp("on", make_array("name", "labelName", "labelValue", "resolution", "timestamp")),
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

}// namespace Euclid::Database