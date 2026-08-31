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

    ModuleResponse CallModule(const std::string &moduleName, const std::string &action, const std::string &token,
                              const std::vector<std::pair<std::string, std::string> > &headers, const std::string &body) {

        // Every currently running instance of the target module, newest state as published by
        // euclid-mgr. More than one is normal for an autoscaled module; any of them can serve
        // the request, so the first reachable one wins.
        std::vector<std::string> sockets;
        for (const auto &module: Database::RepositoryFactory::instance().emmRepository()->findAll()) {
            if (module.name != moduleName) continue;
            for (const auto &instance: module.instances) {
                if (instance.state == Database::Entity::ModuleState::RUNNING) sockets.push_back(instance.socketPath);
            }
        }

        if (sockets.empty()) {
            log_warning << "No running instance of module '" << moduleName << "' to call action '" << action << "'";
            return {};
        }

        for (const auto &socketPath: sockets) {
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

                beast::flat_buffer buffer;
                http::response<http::string_body> res;
                read(sock, buffer, res);

                return {.status = static_cast<int>(res.result_int()), .body = res.body()};

            } catch (const std::exception &e) {
                log_warning << "Call to module '" << moduleName << "' at " << socketPath << " failed, error: " << e.what();
            }
        }

        return {};
    }

}// namespace Euclid::Transfer
