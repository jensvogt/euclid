// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// Boost includes
#include <boost/json.hpp>
#include <boost/beast/http.hpp>

// Euclid includes
#include <euclid/core/HttpActionServer.h>
#include <euclid/core/LogStream.h>
#include <euclid/core/monitoring/MonitoringTimer.h>
#include <euclid/database/RepositoryFactory.h>

namespace Euclid::EMM {

    using namespace boost::beast::http;

    /**
     * @brief EMM (module manager) service server listening on a Unix domain socket.
     *
     * Receives HTTP requests forwarded by the gateway and dispatches them
     * to per-action handler methods. Every action is administrator-only, since it
     * exposes/mutates every module's live process pool and raw database collections.
     *
     * @author jensvogt47\@gmail.com
     */
    class EmmServer final : public Core::HttpActionServer {
    public:

        /**
         * @brief Constructs the server.
         *
         * @param socketPath Unix domain socket path to listen on.
         * @param threads    Number of io_context worker threads.
         */
        explicit EmmServer(std::string socketPath, int threads = 2);

        /**
         * @brief Cancels the nightly backup so the scheduler does not fire into a destroyed server.
         */
        ~EmmServer() override;

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
         * @brief Scheduler id of the nightly backup, empty when it is disabled or unschedulable.
         */
        std::string _backupTaskId;

    };

}// namespace Euclid::EMM
