#pragma once

// C++ includes
#include <string>

// Boost includes
#include <boost/json.hpp>
#include <boost/beast/http.hpp>

// Euclid includes
#include <euclid/core/HttpActionServer.h>
#include <euclid/core/LogStream.h>
#include <euclid/core/Scheduler.h>
#include <euclid/database/RepositoryFactory.h>
#include <euclid/database/entity/emo/MonitoringData.h>

namespace Euclid::Monitoring {

    using namespace boost::beast::http;

    /**
     * @brief Monitoring service server listening on a Unix domain socket.
     *
     * Accepts "push-metrics" pushes from the sqs/access modules (see
     * Core::Monitoring::MetricsPusher, which drives the other end of this from each of those
     * processes), aggregates the name/labelName/labelValue samples over a configured period, and
     * persists the result.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class EmoServer final : public Core::HttpActionServer {
    public:

        /**
         * @brief Constructs the server.
         *
         * Also starts the background flush/prune tasks.
         *
         * @param socketPath Unix domain socket path to listen on.
         * @param threads    Number of io_context worker threads.
         */
        explicit EmoServer(std::string socketPath, int threads = 2);

        /**
         * @brief Cancels the background tasks.
         */
        ~EmoServer() override;

    protected:

        /**
         * @brief Dispatch the request.
         *
         * @param req HTTP request
         * @return HTTP response
         */
        [[nodiscard]]
        response<string_body> Dispatch(const request<string_body> &req) override;

    private:

        /**
         * @brief Averages the current period's accumulated samples (see the file-local
         * accumulator state in MonitoringServer.cpp, fed by the "push-metrics" action handler)
         * and persists one MonitoringData row per name/labelName/labelValue, then resets for the
         * next period.
         */
        static void flush();

        /**
         * @brief Aggregates one resolution tier into the next coarser one, covering the target
         * bucket currently in progress and the one before it.
         *
         * @par
         * Scheduled twice, RAW into HOUR and HOUR into DAY, so each pass only ever reads the tier
         * directly above its target. This is what keeps a year of history affordable: the tiers
         * are retained for 7 days, 90 days and 5 years respectively, so a series costs a couple of
         * thousand rows instead of the half million an undownsampled five-minute history would.
         *
         * @param from tier to read.
         * @param to tier to write.
         */
        static void rollup(Database::Entity::Monitoring::Resolution from, Database::Entity::Monitoring::Resolution to);

        /**
         * @brief Deletes monitoring data whose per-tier retention has elapsed.
         */
        static void prune();

        /**
         * @brief Reads the Linux system's overall CPU usage (from /proc/stat) since the previous
         * call and persists it as a "system-cpu-usage" MonitoringData row. The first call after
         * process start only primes the previous-reading state, since a delta needs two samples.
         */
        static void collectCpuUsage();

        /**
         * @brief Samples the machine's memory usage, labelled by host.
         *
         * @par
         * The host's, not a process's: "euclid-memory-usage-percent" is recorded per module from
         * /proc/self/status and answers what one process holds, which is a different question and
         * cannot be added up into this one.
         */
        static void collectMemoryUsage();

        /**
         * @brief Records the database's own size, object count and collection count.
         *
         * @par
         * Asked of the repository rather than measured here, so the in-memory backend can answer
         * "there is nothing to measure" instead of this module having to know which backend it is
         * running on. Recorded as gauges beside the host's CPU and memory: it is the same kind of
         * fact - what this installation is currently consuming - and the same thing an operator
         * watches to see a disk filling before it does.
         */
        static void collectDatabaseSize();

        /**
         * @brief Ids of the scheduled flush/rollup/prune/cpu-usage tasks, used to cancel them on destruction.
         */
        std::string _flushTaskId;
        std::string _pruneTaskId;
        std::string _hourRollupTaskId;
        std::string _dayRollupTaskId;
        std::string _databaseSizeTaskId;

        std::string _cpuUsageTaskId;
        std::string _memoryUsageTaskId;
        std::string _queueCountsTaskId;

        std::string _bucketCountsTaskId;

        std::string _topicCountsTaskId;
    };

}// namespace Euclid::Monitoring