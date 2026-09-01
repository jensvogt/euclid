// Boost includes
#include <boost/asio/io_context.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/json.hpp>

// Euclid includes
#include <euclid/core/Configuration.h>
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

        // Falls back to the configuration when no path was set explicitly. Every module that
        // publishes an event is a potential pusher now that EES delivers over websockets, and
        // requiring each one's main() to remember a SetGatewaySocketPath() call is exactly the
        // kind of wiring that gets forgotten in a new module - the symptom being events that are
        // stored but never pushed, which looks like a client bug.
        const std::string &resolvedSocketPath() {
            auto &path = gatewaySocketPath();
            if (path.empty()) {
                path = Configuration::instance().getOr<std::string>("euclid.gateway.event-socket-path", "");
            }
            return path;
        }

        void push(const boost::json::object &payload, const std::string &topic) {

            const auto &socketPath = resolvedSocketPath();
            if (socketPath.empty()) return;

            // Fresh one-shot connection per push, same shape as
            // Core::Monitoring::MetricsPusher::pushMetrics() - deliberately not shared/pooled, so
            // a gateway restart never leaves this holding a stale connection.
            try {
                namespace local = asio::local;
                asio::io_context ioc;
                local::stream_protocol::socket sock(ioc);
                sock.connect(local::stream_protocol::endpoint(socketPath));

                http::request<http::string_body> req{http::verb::post, "/", 11};
                req.set("x-euclid-action", "push-event");
                req.body() = boost::json::serialize(payload);
                req.prepare_payload();
                write(sock, req);

                beast::flat_buffer buf;
                http::response<http::string_body> res;
                read(sock, buf, res);

            } catch (const std::exception &ex) {
                log_warning << "EventPusher: failed to push event '" << topic << "' to " << socketPath << ", error: " << ex.what();
            }
        }
    }// namespace

    void EventPusher::SetGatewaySocketPath(std::string path) {
        gatewaySocketPath() = std::move(path);
    }

    void EventPusher::PushToSubscriber(const std::string &subscriber, const std::string &topic, const std::string &accountId,
                                       const std::string &region, const boost::json::object &body) {
        push(boost::json::object{
                     {"subscriber", subscriber},
                     {"topic", topic},
                     {"accountId", accountId},
                     {"region", region},
                     {"body", body},
             },
             topic);
    }

}// namespace Euclid::Core
