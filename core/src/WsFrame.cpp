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

}// namespace Euclid::Core
