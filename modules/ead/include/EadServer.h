// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

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

namespace Euclid::EAD {

    using namespace boost::beast::http;

    /**
     * @brief Audit module - what was run, by whom, in which account.
     *
     * @par Why this module does not write the trail
     * Every module records its own commands as it answers them, through
     * Core::HttpActionServer::Dispatch(). EAD only reads. That split is deliberate: an audit whose
     * write path ran through a separate module would be missing entries exactly when that module
     * was down or stopped - which is both the likeliest moment for something worth auditing and
     * the easiest thing for somebody to arrange.
     *
     * @par What it holds
     * Account, namespace, user, module, command and the parameters it carried, with the status it
     * was answered with and the moment it happened. The parameters have their sensitive values
     * replaced before they are stored, on the writing side, so there is no path by which a
     * password reaches this collection - see Database::Entity::EAD::Redact().
     *
     * @par What it does not hold
     * Successful reads, unless `euclid.modules.ead.audit-reads` asks for them. On a working
     * installation they outnumber everything else by orders of magnitude, and a trail that large
     * buries what it was kept for.
     *
     * @par
     * Nor what flows through a resource rather than being one: a message, an object, an item, an
     * event or a part, when the command succeeded. The trail is about the things somebody manages -
     * "admin deleted queue X" - and the traffic through them arrives at a rate that was measured
     * destroying this very collection, 46,000 discarded entries at a time. A failure is recorded
     * whatever it was acting on. See Core::Permissions::IsSecondLevel() for the rule.
     *
     * @author jensvogt47\@gmail.com
     */
    class EadServer final : public Core::HttpActionServer {

      public:

        /**
         * @brief Constructs the server.
         *
         * @param socketPath Unix domain socket path to listen on.
         * @param threads    number of io_context worker threads.
         */
        explicit EadServer(std::string socketPath, int threads = 4);

      protected:

        /**
         * @brief Dispatch the request.
         *
         * @param req HTTP request
         * @return HTTP response
         */
        [[nodiscard]]
        response<string_body> DispatchAction(const request<string_body> &req) override;
    };

}// namespace Euclid::EAD
