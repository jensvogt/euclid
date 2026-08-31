#pragma once

// C++ includes
#include <string>

// Boost includes
#include <boost/beast/http.hpp>
#include <boost/json.hpp>

// Euclid includes
#include <euclid/core/HttpActionServer.h>
#include <euclid/core/LogStream.h>
#include <euclid/database/RepositoryFactory.h>

namespace Euclid::ETS {

    using namespace boost::beast::http;

    /**
     * @brief Transfer server module - the control plane for FTP and SFTP endpoints.
     *
     * @par
     * ETS itself never speaks FTP or SFTP. It owns the definitions of transfer servers - which
     * protocol, which port, which EAM users and groups may log in, which ESM bucket the files
     * really live in - and start/stop are nothing more than writing a desired state onto one of
     * those definitions. euclid-mgr's reconciler is what turns that into a running
     * euclid-ftp/euclid-sftp process, and the process reads its own definition back from here.
     *
     * @par
     * Splitting it this way means a transfer server survives everything independently: ETS can
     * be down while servers keep running, the manager can restart and bring every RUNNING server
     * back without asking anyone, and a definition can be edited while its server is up (the
     * change takes effect when the reconciler next restarts it).
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class EtsServer final : public Core::HttpActionServer {

    public:

        /**
         * @brief Constructs the server.
         *
         * @param socketPath Unix domain socket path to listen on.
         * @param threads    number of io_context worker threads.
         */
        explicit EtsServer(std::string socketPath, int threads = 2);

    protected:

        /**
         * @brief Dispatch the request.
         *
         * @param req HTTP request
         * @return HTTP response
         */
        [[nodiscard]]
        response<string_body> Dispatch(const request<string_body> &req) override;
    };

}// namespace Euclid::ETS
