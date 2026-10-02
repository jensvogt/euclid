// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <TransferContext.h>

// C++ includes
#include <vector>

// Boost includes
#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

// Euclid includes
#include <euclid/core/LogStream.h>
#include <euclid/database/RepositoryFactory.h>

namespace Euclid::Transfer {

    namespace beast = boost::beast;
    namespace http = beast::http;
    namespace local = boost::asio::local;

    std::optional<TransferContext> TransferContext::Load(const std::string &runtimeName) {

        // By the name the manager started this process under - what --transfer-server carries -
        // rather than by the serverId it is defined as: a serverId is unique only within an
        // account and a namespace, and a process on a host has neither to look itself up with.
        const auto server = Database::RepositoryFactory::instance().etsRepository()->findServerByRuntimeName(runtimeName);
        if (!server.has_value()) {
            log_error << "Transfer server definition not found, runtimeName: " << runtimeName;
            return std::nullopt;
        }
        return TransferContext(*server);
    }

    std::vector<std::string> ModuleSockets(const std::string &moduleName) {

        std::vector<std::string> sockets;
        for (const auto &module: Database::RepositoryFactory::instance().emmRepository()->findAll()) {
            if (module.name != moduleName) continue;
            for (const auto &instance: module.instances) {
                if (instance.state == Database::Entity::ModuleState::RUNNING) sockets.push_back(instance.socketPath);
            }
        }
        return sockets;
    }

    // The transport itself is Core::ModuleClient::CallAt(). It lived here while the transfer
    // servers were the only thing that called a module directly; the manager now does it too, to
    // fetch an application's artifact through ESM rather than off ESM's disk (worker-nodes.md
    // §3.1), and core is the lowest layer the two share.
    //
    // Both halves of the permission go on the wire there, and the target half was missing until
    // 2026-09-26. Core::HttpActionServer's gate builds what it requires as "<target>:<action>", so
    // without it every call a transfer server made asked for ":list-objects" or ":put-object" -
    // not permissions the vocabulary has, so refused before a single grant was read. The symptom
    // was an FTP or SFTP session that logged in and then got 403 on everything, with no grant able
    // to fix it: the reason names a permission that cannot be held. Requests through the gateway
    // were unaffected, because ProxyServer sets the header itself, which is what made this look
    // like a grant problem.
    ModuleResponse CallModuleAt(const std::string &socketPath, const std::string &moduleName, const std::string &action,
                                const std::string &token, const std::vector<std::pair<std::string, std::string> > &headers,
                                const std::string &body) {
        return Core::ModuleClient::CallAt(socketPath, moduleName, action, token, headers, body);
    }

    ModuleResponse CallModuleSticky(std::string &socketPath, const std::string &moduleName, const std::string &action,
                                    const std::string &token,
                                    const std::vector<std::pair<std::string, std::string> > &headers,
                                    const std::string &body) {

        const auto previous = socketPath;

        // The rule itself is Detail::StickyCall(), which is where it can be tested. This supplies
        // the two things it needs and that a test should not: a socket to call and a database to
        // resolve against.
        auto response = Detail::StickyCall(
                socketPath,
                [&](const std::string &candidate) { return CallModuleAt(candidate, moduleName, action, token, headers, body); },
                [&] { return ModuleSockets(moduleName); });

        if (socketPath != previous && response.status != 0) {
            log_info << "Instance of '" << moduleName << "' at " << previous
                     << " stopped answering, carried on at " << socketPath;
        }
        return response;
    }

    ModuleResponse CallModule(const std::string &moduleName, const std::string &action, const std::string &token,
                              const std::vector<std::pair<std::string, std::string> > &headers, const std::string &body) {

        // Every currently running instance of the target module, newest state as published by
        // euclid-mgr. More than one is normal for an autoscaled module; any of them can serve
        // the request, so the first reachable one wins.
        const auto sockets = ModuleSockets(moduleName);
        if (sockets.empty()) {
            log_warning << "No running instance of module '" << moduleName << "' to call action '" << action << "'";
            return {};
        }

        for (const auto &socketPath: sockets) {
            if (auto response = CallModuleAt(socketPath, moduleName, action, token, headers, body); response.status != 0) {
                return response;
            }
        }

        return {};
    }

}// namespace Euclid::Transfer
