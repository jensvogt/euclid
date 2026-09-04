//
// Created by vogje01 on 8/18/26.
//

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

    }// namespace

    MongoEmoRepository::MongoEmoRepository() {
        ensureIndexes();
    }

    void MongoEmoRepository::ensureIndexes() {

        try {
            const auto entry = Database::instance().client();
            auto collection = (*entry)[Database::instance().databaseName()][COLLECTION];

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
            const auto entry = Database::instance().client();
            auto collection = (*entry)[Database::instance().databaseName()][COLLECTION];

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

            const auto entry = Database::instance().client();
            auto collection = (*entry)[Database::instance().databaseName()][COLLECTION];

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

            const auto entry = Database::instance().client();
            auto collection = (*entry)[Database::instance().databaseName()][COLLECTION];

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
            const auto entry = Database::instance().client();
            auto collection = (*entry)[Database::instance().databaseName()][COLLECTION];

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
            const auto entry = Database::instance().client();
            auto collection = (*entry)[Database::instance().databaseName()][COLLECTION];

            const auto result = collection.delete_many(make_document(kvp("expiresAt", make_document(kvp("$lt", toDate(now))))).view());
            const auto deleted = result ? static_cast<long>(result->deleted_count()) : 0;

            log_debug << "Pruned monitoring data, count: " << deleted;
            return deleted;

        } catch (const std::exception &e) {
            log_error << "Prune monitoring data failed, error: " << e.what();
            return 0;
        }
    }

}// namespace Euclid::Database