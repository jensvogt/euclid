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

    std::optional<TransferContext> TransferContext::Load(const std::string &serverId) {

        const auto server = Database::RepositoryFactory::instance().etsRepository()->findServerByServerId(serverId);
        if (!server.has_value()) {
            log_error << "Transfer server definition not found, serverId: " << serverId;
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

    ModuleResponse CallModuleAt(const std::string &socketPath, const std::string &action, const std::string &token,
                                const std::vector<std::pair<std::string, std::string> > &headers, const std::string &body) {

        try {
            boost::asio::io_context ioc;
            local::stream_protocol::socket sock(ioc);
            sock.connect(local::stream_protocol::endpoint(socketPath));

            http::request<http::string_body> req{http::verb::post, "/", 11};
            req.set("x-euclid-action", action);
            if (!token.empty()) req.set(http::field::authorization, "Bearer " + token);
            for (const auto &[name, value]: headers) req.set(name, value);
            req.body() = body;
            req.prepare_payload();

            write(sock, req);

            // Beast caps a response body at 1MB unless told otherwise, which an object download
            // passes as soon as the file is bigger than a text file - the read then fails and the
            // call looks like an unreachable module. The peer is a local module answering over a
            // Unix socket with a size it has already bounded itself, so there is nothing left for
            // a limit here to protect against.
            beast::flat_buffer buffer;
            http::response_parser<http::string_body> parser;
            parser.body_limit(boost::none);
            read(sock, buffer, parser);

            return {.status = static_cast<int>(parser.get().result_int()), .body = std::move(parser.get().body())};

        } catch (const std::exception &e) {
            log_warning << "Call to action '" << action << "' at " << socketPath << " failed, error: " << e.what();
            return {};
        }
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
            if (auto response = CallModuleAt(socketPath, action, token, headers, body); response.status != 0) {
                return response;
            }
        }

        return {};
    }

}// namespace Euclid::Transfer
