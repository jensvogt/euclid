// Boost includes
#include <boost/json.hpp>

// Euclid includes
#include <euclid/manager/GatewayEventIngest.h>
#include <euclid/manager/GatewayWsRegistry.h>

namespace Euclid::main {

    namespace beast = boost::beast;
    namespace http = beast::http;

    namespace {
        http::response<http::string_body> JsonResponse(const http::request<http::string_body> &req, const http::status status, std::string body = {}) {
            http::response<http::string_body> res{status, req.version()};
            res.set(http::field::content_type, "application/json");
            res.keep_alive(req.keep_alive());
            res.body() = std::move(body);
            res.prepare_payload();
            return res;
        }

        http::response<http::string_body> ErrorResponse(const http::request<http::string_body> &req, const http::status status, const std::string_view message) {
            return JsonResponse(req, status, boost::json::serialize(boost::json::object{{"error", std::string(message)}}));
        }
    }// namespace

    GatewayEventIngest::GatewayEventIngest(std::string socketPath, const int threads) : UnixSocketServer("GatewayEventIngest", std::move(socketPath), threads) {}

    http::response<http::string_body> GatewayEventIngest::Dispatch(const http::request<http::string_body> &req) {

        if (std::string(req["x-euclid-action"]) != "push-event") {
            return ErrorResponse(req, http::status::not_found, "Action not implemented");
        }

        boost::system::error_code ec;
        const auto jv = boost::json::parse(req.body(), ec);
        if (ec || !jv.is_object()) {
            return ErrorResponse(req, http::status::bad_request, "Invalid JSON body");
        }

        const auto &obj = jv.as_object();
        const auto *topic = obj.if_contains("topic");
        const auto *accountId = obj.if_contains("accountId");
        const auto *region = obj.if_contains("region");
        if (!topic || !topic->is_string() || !accountId || !accountId->is_string() || !region || !region->is_string()) {
            return ErrorResponse(req, http::status::bad_request, "Missing topic/accountId/region");
        }

        boost::json::object body;
        if (const auto *b = obj.if_contains("body"); b && b->is_object()) body = b->as_object();

        // Always addressed to a subscriber: who receives an event is decided where it is
        // published, by the subscription that matched it, and a push with nobody to deliver to
        // is a bug in the caller rather than something to broadcast at every connection.
        std::string subscriber;
        if (const auto *s = obj.if_contains("subscriber"); s && s->is_string()) subscriber = std::string(s->as_string());
        if (subscriber.empty()) {
            return ErrorResponse(req, http::status::bad_request, "Missing subscriber");
        }

        const auto delivered = GatewayWsRegistry::instance().DeliverToSubscriber(
                subscriber, std::string(topic->as_string()), std::string(accountId->as_string()), std::string(region->as_string()), body);

        // Reported back so the publisher can tell "nobody was connected" from "delivered" - for a
        // durable subscription that is the difference between a client that will be told and one
        // that will have to come and ask.
        return JsonResponse(req, http::status::ok, boost::json::serialize(boost::json::object{{"delivered", delivered}}));
    }

}// namespace Euclid::main
