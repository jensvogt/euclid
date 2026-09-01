#pragma once

// C++ includes
#include <string>

// Boost includes
#include <boost/beast/http.hpp>
#include <boost/json.hpp>

// Euclid includes
#include <euclid/core/HttpActionServer.h>
#include <euclid/core/LogStream.h>
#include <euclid/database/EventBus.h>
#include <euclid/database/RepositoryFactory.h>

namespace Euclid::EES {

    using namespace boost::beast::http;

    /**
     * @brief Event service module - the way a client subscribes to what happens inside euclid.
     *
     * @par
     * Everything a module publishes already goes onto the internal event bus, which persists each
     * delivery, hands it to one consumer at a time and keeps it until that consumer says it is
     * done. What was missing was a way to be one of those consumers without being a euclid
     * module, and this is it: subscribe under a name, receive, acknowledge.
     *
     * @par Why not a queue per consumer
     * Because the bus already fans out per subscriber. Two applications watching the same bucket
     * each get their own copy of an event; two instances of one application share a subscriber
     * name, so exactly one of them processes it. Nothing has to be created per bucket, per topic
     * or per client to arrange that.
     *
     * @par Delivery
     * At-least-once, unordered - the same guarantees a queue gives. A received event is invisible
     * to other claimers until its lease expires, comes back if it is never acknowledged, and is
     * deleted when it is. An event nobody claims is expired by the bus after its retention
     * period, so an application that never returns cannot fill the database.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class EesServer final : public Core::HttpActionServer {

    public:

        /**
         * @brief Constructs the server.
         *
         * @param socketPath Unix domain socket path to listen on.
         * @param threads    number of io_context worker threads.
         */
        explicit EesServer(std::string socketPath, int threads = 2);

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

}// namespace Euclid::EES
