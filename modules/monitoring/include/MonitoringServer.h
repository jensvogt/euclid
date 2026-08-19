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
#include <euclid/database/entity/monitoring/MonitoringData.h>

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
    class MonitoringServer final : public Core::HttpActionServer {
    public:
        /**
         * @brief Constructs the server.
         *
         * Also starts the background flush/prune tasks.
         *
         * @param socketPath Unix domain socket path to listen on.
         * @param threads    Number of io_context worker threads.
         */
        explicit MonitoringServer(std::string socketPath, int threads = 2);

        /**
         * @brief Cancels the background tasks.
         */
        ~MonitoringServer() override;

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
         * @brief Deletes monitoring data older than euclid.monitoring.retention days.
         */
        static void prune();

        /**
         * @brief Ids of the scheduled flush/prune tasks, used to cancel them on destruction.
         */
        std::string _flushTaskId;
        std::string _pruneTaskId;
    };

}// namespace Euclid::Monitoring
