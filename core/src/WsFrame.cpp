// Euclid includes
#include <euclid/core/UuidUtils.h>
#include <euclid/core/WsFrame.h>

namespace Euclid::Core {

    namespace http = boost::beast::http;

    std::optional<WsFrame::Request> WsFrame::ParseRequest(const std::string &text, std::string &error) {

        boost::system::error_code ec;
        const auto jv = boost::json::parse(text, ec);
        if (ec || !jv.is_object()) {
            error = "Invalid JSON frame";
            return std::nullopt;
        }

        const auto &obj = jv.as_object();
        const auto *type = obj.if_contains("type");
        if (!type || !type->is_string() || type->as_string() != "request") {
            error = R"(Expected a "request" frame)";
            return std::nullopt;
        }

        const auto *id = obj.if_contains("id");
        const auto *target = obj.if_contains("target");
        const auto *action = obj.if_contains("action");
        if (!id || !id->is_string() || !target || !target->is_string() || !action || !action->is_string()) {
            error = "Request frame missing id/target/action";
            return std::nullopt;
        }

        Request r;
        r.id = std::string(id->as_string());
        r.target = std::string(target->as_string());
        r.action = std::string(action->as_string());
        if (const auto *body = obj.if_contains("body")) r.body = *body;
        else r.body = boost::json::object{};
        return r;
    }

    http::request<http::string_body> WsFrame::ToHttpRequest(const Request &request, const std::string &authorization, const std::string &accountId, const std::string &region, const std::string &ns) {
        http::request<http::string_body> req{http::verb::post, "/", 11};
        req.set("x-euclid-target", request.target);
        req.set("x-euclid-action", request.action);
        if (!authorization.empty()) req.set(http::field::authorization, authorization);
        if (!accountId.empty()) req.set("x-euclid-account-id", accountId);
        if (!region.empty()) req.set("x-euclid-region", region);
        if (!ns.empty()) req.set("x-euclid-namespace", ns);
        req.set(http::field::content_type, "application/json");
        req.body() = boost::json::serialize(request.body);
        req.prepare_payload();
        return req;
    }

    std::string WsFrame::BuildResponseFrame(const std::string &id, const http::response<http::string_body> &res) {
        boost::json::value body;
        if (!res.body().empty()) {
            boost::system::error_code ec;
            body = boost::json::parse(res.body(), ec);
            // Backend response wasn't JSON (shouldn't normally happen - every module responds
            // with application/json) - pass the raw text through rather than silently dropping it.
            if (ec) body = res.body();
        }
        return boost::json::serialize(boost::json::object{
                {"type", "response"},
                {"id", id},
                {"status", res.result_int()},
                {"body", body},
        });
    }

    std::string WsFrame::BuildErrorFrame(const std::string &id, const unsigned status, const std::string_view message) {
        return boost::json::serialize(boost::json::object{
                {"type", "error"},
                {"id", id},
                {"status", status},
                {"error", std::string(message)},
        });
    }

    std::string WsFrame::BuildEventFrame(const std::string &topic, const std::string &accountId, const std::string &region, const boost::json::object &body) {
        return boost::json::serialize(boost::json::object{
                {"type", "event"},
                {"id", UuidUtils::CreateRandomUuid()},
                {"topic", topic},
                {"accountId", accountId},
                {"region", region},
                {"body", body},
        });
    }

    std::optional<std::string> WsFrame::FrameType(const std::string &text) {
        boost::system::error_code ec;
        const auto jv = boost::json::parse(text, ec);
        if (ec || !jv.is_object()) return std::nullopt;

        const auto *type = jv.as_object().if_contains("type");
        if (!type || !type->is_string()) return std::nullopt;
        return std::string(type->as_string());
    }

    std::optional<WsFrame::ParsedSubscription> WsFrame::ParseSubscription(const std::string &text, std::string &error) {

        boost::system::error_code ec;
        const auto jv = boost::json::parse(text, ec);
        if (ec || !jv.is_object()) {
            error = "Invalid JSON frame";
            return std::nullopt;
        }

        const auto &obj = jv.as_object();
        const auto *type = obj.if_contains("type");
        if (!type || !type->is_string() || (type->as_string() != "subscribe" && type->as_string() != "unsubscribe")) {
            error = R"(Expected a "subscribe" or "unsubscribe" frame)";
            return std::nullopt;
        }

        const auto *id = obj.if_contains("id");
        const auto *topic = obj.if_contains("topic");
        const auto *name = obj.if_contains("name");
        const bool named = name != nullptr && name->is_string() && !name->as_string().empty();
        // A topic is what there is to subscribe to, so it is required - except on a frame that
        // names an existing subscription, which is attaching to one rather than describing one.
        if (!id || !id->is_string() || ((!topic || !topic->is_string()) && !named)) {
            error = "Subscription frame missing id/topic";
            return std::nullopt;
        }

        ParsedSubscription r;
        r.id = std::string(id->as_string());
        r.type = std::string(type->as_string());
        if (topic && topic->is_string()) r.topic = std::string(topic->as_string());
        if (const auto *filter = obj.if_contains("filter"); filter && filter->is_object()) {
            r.filter = filter->as_object();
        }
        // Optional: naming an EES subscriber attaches this connection to it, so what arrives is
        // what that subscription matched rather than what this frame's topic/filter did.
        if (named) r.name = std::string(name->as_string());
        if (const auto *mode = obj.if_contains("mode"); mode && mode->is_string()) {
            r.mode = std::string(mode->as_string());
        }
        return r;
    }

    std::string WsFrame::BuildSubscriptionAckFrame(const std::string &id, const std::string &ackType, const std::string &name) {
        boost::json::object frame{
                {"type", ackType},
                {"id", id},
        };
        // Echoed because a client that named nothing still has a subscription, and this is the
        // only place it learns the name its events arrive under.
        if (!name.empty()) frame["name"] = name;
        return boost::json::serialize(frame);
    }

}// namespace Euclid::Core
