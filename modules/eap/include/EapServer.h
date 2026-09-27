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
#include <euclid/core/BuiltinRoles.h>
#include <euclid/database/RepositoryFactory.h>

namespace Euclid::EAP {

    using namespace boost::beast::http;

    /**
     * @brief Application module - the control plane for applications euclid runs.
     *
     * @par
     * EAP never runs an application itself. It owns the definitions - which artifact in which
     * bucket, which runtime starts it, which EAM user it runs as, how far the autoscaler may
     * scale it - and start/stop are nothing more than writing a desired state onto one of those
     * definitions. euclid-mgr's reconciler is what turns that into running processes: it
     * materialises the artifact out of ESM, hands the process its credentials through the
     * environment, and from then on treats it exactly like any other module in the pool.
     *
     * @par
     * The application itself needs no euclid library, no database access and no knowledge of any
     * of this - which is what lets it be written in Java, Python, Node.js, Rust or C++. All it
     * has to do is listen on the socket path it is given in EUCLID_SOCKET.
     *
     * @author jensvogt47\@gmail.com
     */
    class EapServer final : public Core::HttpActionServer {

    public:

        /**
         * @brief Constructs the server.
         *
         * Also creates the bucket applications are deployed from, if it is not there already - see
         * EnsureApplicationBucket().
         *
         * @param socketPath Unix domain socket path to listen on.
         * @param threads    number of io_context worker threads.
         */
        explicit EapServer(std::string socketPath, int threads = 2);

        /**
         * @brief Creates the internal bucket applications are deployed from, unless it exists.
         *
         * @par
         * An artifact lives in an ESM bucket, and every other part of deploying one is arranged by
         * euclid: the principal, the runtime name, the pool. The bucket was the exception - a
         * `create-bucket` an operator had to know to run first, whose only symptom when forgotten is
         * a create-application refused with "Bucket not found". So it is made here instead, named by
         * `euclid.modules.eap.bucket` (default "apps").
         *
         * @par Where
         * In every account and namespace this installation serves: each of `euclid.account-ids`,
         * and within each of those the account root plus every namespace named in
         * `euclid.namespaces` or existing in EAM. Not one bucket but one per namespace, because a
         * bucket name is unique within (account, namespace) and every lookup that resolves a name is
         * scoped to the namespace the request was made in - a bucket at the account root is not
         * visible to a client working in "development" and cannot be deployed from there. An empty
         * bucket where nothing is deployed costs nothing, and is hidden besides.
         *
         * @par
         * A namespace created after this ran gets its bucket on EAP's next start. EAM publishes
         * nothing to subscribe to, and re-scanning on a timer would be a lot of machinery for a
         * bucket somebody can also make by hand.
         *
         * @par
         * Marked internal, so it stays out of list-buckets and the bucket count: its contents are
         * artifacts EAP and the manager deal in, and an operator browsing their own buckets has no
         * reason to act on it. Hidden is all that means - see Entity::ESM::Bucket::internal - so
         * uploading an artifact into it works exactly as the documentation has always said.
         *
         * @par
         * An existing bucket is left untouched, including one an operator made by hand and one that
         * is not marked internal: it holds the installation's artifacts, and rewriting the row would
         * reset the counters and encryption setting it carries.
         *
         * @par
         * Nothing here is fatal. Refusing to start over a bootstrap step would take every
         * application definition with it, so a failure is logged per account and the next start
         * tries again.
         *
         * @param repository storage repository the bucket is read from and written to.
         * @param identities identity repository the account's namespaces are read from.
         */
        static void EnsureApplicationBucket(Database::IEsmRepository &repository, const Database::IEamRepository &identities);

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

}// namespace Euclid::EAP
