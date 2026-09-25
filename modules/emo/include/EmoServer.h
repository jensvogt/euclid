// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

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
     * @author jensvogt47\@gmail.com
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
        response<string_body> DispatchAction(const request<string_body> &req) override;

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
         * @brief Reads the host's overall CPU usage since the previous call and persists it as a
         * "system-cpu-usage" MonitoringData row. The first call after process start only primes the
         * previous-reading state, since a delta needs two samples.
         *
         * @par
         * The reading itself is Core::SystemUtils::ReadCpuTimes()' problem - /proc/stat on Linux,
         * GetSystemTimes() on Windows - so the arithmetic here is the same on both.
         */
        static void collectCpuUsage();

        /**
         * @brief Records how much work is waiting for the processors, under the names the platform
         * has figures for.
         *
         * @par Linux
         * The run-queue averages from /proc/loadavg, as "system-load-average" labelled by the window
         * each averages over, plus "system-load-per-core" for the one-minute figure divided by the
         * CPU count. Both, because neither is sufficient alone. The raw averages are what an
         * operator recognises and what the three windows make comparable - a 1-minute figure well
         * above the 15-minute one is a machine that has just got busy. The per-core figure is the
         * one that means the same thing on every host: it saturates at 1 whatever the hardware,
         * where a raw load of 8 is a third of a 24-core machine and four times a two-core one.
         *
         * @par Windows
         * The "\\System\\Processor Queue Length" performance counter, as
         * "system-processor-queue-length" and "system-processor-queue-per-core". Different names on
         * purpose: Windows keeps no 1-, 5- and 15-minute averages, so recording an instantaneous
         * count as "system-load-average" would put a figure averaged over nothing into a series
         * whose whole meaning is the window it averages over. The averaging EMO does per bucket is
         * what makes the instantaneous count readable instead.
         */
        static void collectSystemLoad();

        /**
         * @brief Samples what each module's pool is doing: how many instances it runs, how loaded
         * they say they are, and how much work is waiting.
         *
         * @par
         * Read from the module records the manager keeps, so it covers every module and every
         * application rather than only the ones that push metrics of their own. Recorded per
         * module rather than per instance, deliberately: instances come and go as a pool scales, so
         * a series per instance is a series that ends every time the thing it measures is replaced,
         * and a graph of it is unreadable. Which instance was busy is a question for
         * `emm list-modules`, which answers about now; this answers about the last fortnight.
         *
         * @par Averages and sums
         * "module-utilisation" is the mean across the pool's reporting instances and
         * "module-backlog" is the total across them, because that is what each one means - half a
         * pool at 100% is a pool at 50%, while half a pool holding 500 messages each is 1000
         * messages waiting. Both are computed here and recorded as one sample per module: EMO
         * averages the samples sharing a label, so recording one per instance would silently turn
         * the backlog into a mean.
         *
         * @par
         * An application that reports nothing contributes an instance count and no load, rather
         * than a load of zero - see Entity::ModuleInstance::utilisation.
         */
        static void collectModuleInstances();

        /**
         * @brief Samples the machine's memory usage, labelled by host.
         *
         * @par
         * The host's, not a process's: "euclid-memory-usage-percent" is recorded per module and
         * answers what one process holds, which is a different question and cannot be added up into
         * this one.
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
        std::string _systemLoadTaskId;
        std::string _memoryUsageTaskId;
        std::string _queueCountsTaskId;

        std::string _bucketCountsTaskId;

        std::string _topicCountsTaskId;

        std::string _moduleInstancesTaskId;
    };

}// namespace Euclid::Monitoring