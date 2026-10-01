// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Boost includes
#include <boost/beast/http.hpp>

// Euclid includes
#include <euclid/core/ModuleClient.h>
#include <euclid/database/entity/ets/TransferServer.h>

namespace Euclid::Transfer {

    /**
     * @brief Everything a spawned euclid-ftp/euclid-sftp process needs to know about itself.
     *
     * @par
     * A transfer server process is started by euclid-mgr with nothing but a --transfer-server
     * ID; it reads its own definition back out of the ETS repository from there. That
     * indirection is what lets the ETS module change a server's bucket, ports or permitted
     * users without the manager having to pass any of it on the command line.
     *
     * @author jensvogt47\@gmail.com
     */
    class TransferContext {

    public:

        /**
         * @brief Loads the definition of the transfer server this process was started as.
         *
         * @param runtimeName the name the manager started it under, as passed to
         * --transfer-server. Not the serverId the server is defined as: that is unique only within
         * an account and a namespace, and this process has neither - see Entity::ETS::RuntimeName().
         * @return the context, or std::nullopt if no such server is defined.
         */
        [[nodiscard]]
        static std::optional<TransferContext> Load(const std::string &runtimeName);

        /**
         * @brief The definition this process is running.
         */
        [[nodiscard]]
        const Database::Entity::ETS::TransferServer &Server() const { return _server; }

    private:

        explicit TransferContext(Database::Entity::ETS::TransferServer server) : _server(std::move(server)) {}

        Database::Entity::ETS::TransferServer _server;
    };

    /**
     * @brief Result of one call to a module over its Unix domain socket.
     *
     * @par
     * Core::ModuleClient::ModuleResponse under its original name. The type and the transport moved
     * to core when the manager came to need them too - it fetches an application's artifact through
     * ESM rather than off ESM's disk, see docs/worker-nodes.md §3.1 - and core is the lowest layer
     * all three callers share. The alias keeps every `Transfer::ModuleResponse` in the FTP and SFTP
     * servers meaning what it always did.
     */
    using ModuleResponse = Core::ModuleClient::ModuleResponse;

    /**
     * @brief Socket paths of every currently running instance of a module.
     *
     * @par
     * Resolved through the module repository, the same way Core::Monitoring::MetricsPusher finds
     * the monitoring module: euclid-mgr publishes each live instance's socket path there, so
     * addressing a module means asking which of its processes are currently up rather than
     * assuming the single base socket from the config.
     *
     * @param moduleName module to look up, e.g. "esm".
     * @return the sockets, in no particular order; empty if no instance is running.
     */
    [[nodiscard]]
    std::vector<std::string> ModuleSockets(const std::string &moduleName);

    /**
     * @brief Calls one action on one specific module instance.
     *
     * @par
     * Prefer CallModule() for a single call, and CallModuleSticky() for a sequence that wants to
     * keep to one instance without depending on it surviving. This one is the building block both
     * are made of.
     *
     * @param socketPath instance socket, as returned by ModuleSockets().
     * @param moduleName the module being called, for the x-euclid-target header. The authorization
     * gate builds the permission it requires out of that header and x-euclid-action, so a request
     * without it asks to be allowed ":list-objects", which is not a permission any role can hold -
     * and is refused with a 403 no grant can fix.
     * @param action value for the x-euclid-action header.
     * @param token bearer token to authenticate as.
     * @param headers additional request headers, e.g. x-euclid-bucket-ern.
     * @param body request body, raw.
     * @return the module's response, or a zero status if the instance could not be reached.
     */
    [[nodiscard]]
    ModuleResponse CallModuleAt(const std::string &socketPath, const std::string &moduleName, const std::string &action,
                                const std::string &token, const std::vector<std::pair<std::string, std::string> > &headers,
                                const std::string &body);

    /**
     * @brief Calls an action on a module, preferring the instance that answered last.
     *
     * @par What it is for
     * A multipart transfer is a long sequence of calls, and it used to hold one instance's socket
     * for all of them. That made an upload only as durable as the instance it happened to start
     * on: a 12 GB delivery is fifteen hundred parts over about ninety seconds, during which its
     * own request load ramps ESM from one instance to ten, and if the autoscaler then stops that
     * particular one every part already sent is lost - there is no resume, the next attempt starts
     * at part one. Observed doing exactly that: "upload-part ... failed, error: Broken pipe" at
     * part 566 of an 11.5 GiB file.
     *
     * @par Why the parts can move
     * The reason given for pinning - that the staged parts live next to the instance that created
     * them - is not so. They live under `euclid.modules.esm.data-dir`, which is one configured
     * path, and every instance of a module runs on the host that spawned it. So any instance can
     * see the upload directory, accept a part into it, and assemble it afterwards. Writing a part
     * is idempotent too: it is a numbered file, so a part retried against another instance
     * overwrites rather than duplicates.
     *
     * @par Why it is sticky rather than resolved each time
     * Resolving reads every module's record from the database. Doing that per part would be
     * fifteen hundred queries for one upload. So the socket that worked is kept and only looked up
     * again when a call fails at the transport - which is what an instance going away looks like
     * from here.
     *
     * @param socketPath in/out: the instance to try first, updated to whichever answered. Pass an
     * empty string to resolve one.
     * @param moduleName module to call, e.g. "esm".
     * @param action value for the x-euclid-action header.
     * @param token bearer token to authenticate as.
     * @param headers additional request headers.
     * @param body request body, raw.
     * @return the module's response, or a zero status if no instance could be reached.
     */
    namespace Detail {

        /**
         * @brief Builds the request CallModuleAt() puts on a module socket.
         *
         * @par
         * Core::ModuleClient::BuildRequest() under its original name - see the note on
         * ModuleResponse above for why the transport moved. Kept as an alias because what it is
         * for is unchanged: the wire contract can be asserted without a socket, since a module is
         * reached over AF_UNIX and Boost.Asio cannot bind one of those on Windows at all, so a
         * test that needed a listener would not run on the platform this was found on.
         */
        inline auto BuildModuleRequest(const std::string &moduleName, const std::string &action, const std::string &token,
                                       const std::vector<std::pair<std::string, std::string> > &headers, const std::string &body) {
            return Core::ModuleClient::BuildRequest(moduleName, action, token, headers, body);
        }

        /**
         * @brief The instance-selection rule behind CallModuleSticky(), over injected callables.
         *
         * @par
         * Separated so it can be tested: the real thing needs a Unix socket to call and a database
         * to resolve against, and what is worth pinning is none of that - it is that a remembered
         * instance costs no lookup, that a transport failure moves on rather than ending the
         * transfer, that the instance which just failed is not tried twice, and that whichever
         * answered is the one remembered next.
         *
         * @param socketPath in/out: tried first when not empty, set to whichever answered.
         * @param callAt invoked with a socket path; returns a ModuleResponse, status 0 meaning
         * the instance could not be reached.
         * @param resolve invoked only when there is no remembered socket or it failed; returns the
         * candidates to try.
         * @return the response, or a zero status when nothing answered.
         */
        template<typename CallAt, typename Resolve>
        ModuleResponse StickyCall(std::string &socketPath, const CallAt &callAt, const Resolve &resolve) {

            if (!socketPath.empty()) {
                if (auto response = callAt(socketPath); response.status != 0) return response;
            }

            for (const auto &candidate: resolve()) {
                if (candidate == socketPath) continue;
                if (auto response = callAt(candidate); response.status != 0) {
                    socketPath = candidate;
                    return response;
                }
            }
            return {};
        }

    }// namespace Detail

    [[nodiscard]]
    ModuleResponse CallModuleSticky(std::string &socketPath, const std::string &moduleName, const std::string &action,
                                    const std::string &token,
                                    const std::vector<std::pair<std::string, std::string> > &headers,
                                    const std::string &body);

    /**
     * @brief Calls one action on a running instance of another module.
     *
     * @par
     * The target instance is resolved through the module repository, the same way
     * Core::Monitoring::MetricsPusher finds the monitoring module: euclid-mgr publishes each
     * live instance's socket path there, so addressing a module means asking which of its
     * processes are currently up rather than assuming the single base socket from the config.
     *
     * @param moduleName module to call, e.g. "esm".
     * @param action value for the x-euclid-action header.
     * @param token bearer token to authenticate as; a transfer server passes one minted for the
     * logged-in user, so the target module applies that user's own permissions.
     * @param headers additional request headers, e.g. x-euclid-bucket-ern.
     * @param body request body, raw.
     * @return the module's response, or a zero status if no instance could be reached.
     */
    [[nodiscard]]
    ModuleResponse CallModule(const std::string &moduleName, const std::string &action, const std::string &token,
                              const std::vector<std::pair<std::string, std::string> > &headers, const std::string &body);

}// namespace Euclid::Transfer
