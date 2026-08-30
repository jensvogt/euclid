// Boost includes
#include <boost/asio/io_context.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/json.hpp>

// Euclid includes
#include <euclid/core/EventPusher.h>
#include <euclid/core/LogStream.h>

namespace Euclid::Core {

    namespace beast = boost::beast;
    namespace http = beast::http;
    namespace asio = boost::asio;

    namespace {
        std::string &gatewaySocketPath() {
            static std::string path;
            return path;
        }
    }// namespace

    void EventPusher::SetGatewaySocketPath(std::string path) {
        gatewaySocketPath() = std::move(path);
    }

    void EventPusher::Push(const std::string &topic, const std::string &accountId, const std::string &region, const boost::json::object &body) {

        const auto &socketPath = gatewaySocketPath();
        if (socketPath.empty()) return;

        // Fresh one-shot connection per push, same shape as
        // Core::Monitoring::MetricsPusher::pushMetrics() - deliberately not shared/pooled, so a
        // gateway restart never leaves this holding a stale connection.
        try {
            namespace local = asio::local;
            asio::io_context ioc;
            local::stream_protocol::socket sock(ioc);
            sock.connect(local::stream_protocol::endpoint(socketPath));

            const auto payload = boost::json::serialize(boost::json::object{
                    {"topic", topic},
                    {"accountId", accountId},
                    {"region", region},
                    {"body", body},
            });

            http::request<http::string_body> req{http::verb::post, "/", 11};
            req.set("x-euclid-action", "push-event");
            req.body() = payload;
            req.prepare_payload();
            write(sock, req);

            beast::flat_buffer buf;
            http::response<http::string_body> res;
            read(sock, buf, res);

        } catch (const std::exception &ex) {
            log_warning << "EventPusher: failed to push event '" << topic << "' to " << socketPath << ", error: " << ex.what();
        }
    }

}// namespace Euclid::Core
