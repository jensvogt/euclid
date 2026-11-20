// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <memory>
#include <string>

// Boost includes
#include <boost/beast/http.hpp>

// Euclid includes
#include <Catalog.h>
#include <DnsListener.h>
#include <euclid/core/HttpActionServer.h>

namespace Euclid::EDS {

    using namespace boost::beast::http;

    /**
     * @brief EDS - the domain name service.
     *
     * @par
     * Two servers in one process. The Unix socket every module has, through which zones and
     * records are managed with the usual credentials and grants; and a DNS server on a UDP and a
     * TCP port, which answers anybody who asks - DNS has no credentials - from the zones stored
     * here and the names euclid gives what it runs, and forwards everything else upstream for
     * clients on the networks allowed to recurse.
     *
     * @par
     * The DNS side never reads the database on a query: see Catalog. A record written through the
     * API is answered within a moment on this instance, and within euclid.modules.eds.refresh-seconds
     * on any other.
     *
     * @author jensvogt47\@gmail.com
     */
    class EdsServer final : public Core::HttpActionServer {

    public:
        /**
         * @brief Constructs the server, loads the zones and starts the DNS listener.
         *
         * @par
         * A listener that cannot bind its port is logged and the module runs on without it, as
         * EAG does: the zones can still be managed, and a process the manager kept restarting
         * would say nothing more about why.
         *
         * @param socketPath Unix domain socket path to listen on.
         * @param threads    Number of io_context worker threads.
         */
        explicit EdsServer(std::string socketPath, int threads = 4);

        ~EdsServer() override;

        EdsServer(const EdsServer &) = delete;
        EdsServer &operator=(const EdsServer &) = delete;

    protected:
        [[nodiscard]]
        response<string_body> DispatchAction(const request<string_body> &req) override;

    private:
        std::unique_ptr<Catalog> _catalog;
        std::unique_ptr<DnsListener> _listener;
    };

}// namespace Euclid::EDS
