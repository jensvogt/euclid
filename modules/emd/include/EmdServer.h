//
// Created by vogje01 on 9/8/26.
//

#pragma once

// Boost includes
#include <boost/beast/http.hpp>

// Euclid includes
#include <euclid/core/HttpActionServer.h>
#include <euclid/core/LogStream.h>
#include <euclid/database/emd/DocumentStore.h>

namespace Euclid::EMD {

    using namespace boost::beast::http;

    /**
     * @brief The memory database: one document store, served to every module over a socket.
     *
     * @par
     * euclid's other backend is MongoDB, and everything above the repositories is written for it.
     * The in-process memory store keeps those semantics but lives inside whichever process created
     * it, so a login issued by EAM is invisible to the module the caller goes on to talk to - which
     * makes it useless for anything but a single-module test. This module holds one store and hands
     * it to all of them, which is the difference between a backend that can run an installation and
     * one that cannot.
     *
     * @par
     * It is off by default and has no CLI and no place in the RUI, because it is not a service
     * anybody uses - it is where the others keep their data when there is no database to keep it
     * in. What it is for is tests, demonstrations, and a container that has to come up with nothing
     * beside it.
     *
     * @par
     * There is no authentication. The socket is the boundary, exactly as it is for a database
     * listening on a private socket: whoever can open it can read and write everything, so it
     * belongs in a directory only euclid can enter. Putting euclid's own authentication in front of
     * it would be circular anyway - the users to check against are in the store.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class EmdServer final : public Core::HttpActionServer {

    public:

        /**
         * @brief Constructs the server.
         *
         * @param socketPath Unix domain socket path to listen on.
         * @param threads    Number of io_context worker threads.
         */
        explicit EmdServer(std::string socketPath, int threads = 4);

    protected:

        /**
         * @brief Dispatch one store operation.
         *
         * @param req HTTP request whose body is a BSON document of arguments
         * @return HTTP response whose body is a BSON document of results
         */
        [[nodiscard]]
        response<string_body> Dispatch(const request<string_body> &req) override;

    private:

        /**
         * @brief The store itself. One per process, and this is the process.
         */
        Database::Emd::DocumentStore _store;
    };

}// namespace Euclid::EMD
