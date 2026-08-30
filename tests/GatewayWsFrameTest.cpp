#define BOOST_TEST_MODULE GatewayWsFrameTest
#include <boost/test/unit_test.hpp>

// Euclid includes
#include <euclid/core/WsFrame.h>

using Euclid::Core::WsFrame;
namespace http = boost::beast::http;

// ── ParseRequest() ──────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(ParseRequest_ValidFrame_ReturnsRequest) {
    std::string error;
    const auto parsed = WsFrame::ParseRequest(
            R"({"type":"request","id":"abc-123","target":"ekm","action":"create-key","body":{"algorithm":"AES","length":256}})", error);

    BOOST_REQUIRE(parsed.has_value());
    BOOST_CHECK_EQUAL(parsed->id, "abc-123");
    BOOST_CHECK_EQUAL(parsed->target, "ekm");
    BOOST_CHECK_EQUAL(parsed->action, "create-key");
    BOOST_CHECK(parsed->body.is_object());
    BOOST_CHECK_EQUAL(parsed->body.as_object().at("algorithm").as_string(), "AES");
}

BOOST_AUTO_TEST_CASE(ParseRequest_MissingBody_DefaultsToEmptyObject) {
    std::string error;
    const auto parsed = WsFrame::ParseRequest(R"({"type":"request","id":"1","target":"eqs","action":"list-queues"})", error);

    BOOST_REQUIRE(parsed.has_value());
    BOOST_CHECK(parsed->body.is_object());
    BOOST_CHECK(parsed->body.as_object().empty());
}

BOOST_AUTO_TEST_CASE(ParseRequest_MalformedJson_ReturnsNullopt) {
    std::string error;
    const auto parsed = WsFrame::ParseRequest("not json", error);

    BOOST_CHECK(!parsed.has_value());
    BOOST_CHECK(!error.empty());
}

BOOST_AUTO_TEST_CASE(ParseRequest_WrongType_ReturnsNullopt) {
    std::string error;
    const auto parsed = WsFrame::ParseRequest(R"({"type":"response","id":"1"})", error);

    BOOST_CHECK(!parsed.has_value());
}

BOOST_AUTO_TEST_CASE(ParseRequest_MissingTarget_ReturnsNullopt) {
    std::string error;
    const auto parsed = WsFrame::ParseRequest(R"({"type":"request","id":"1","action":"create-key"})", error);

    BOOST_CHECK(!parsed.has_value());
}

// ── ToHttpRequest() ──────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(ToHttpRequest_MapsFieldsToHeaders) {
    const WsFrame::Request request{.id = "abc", .target = "ekm", .action = "create-key", .body = boost::json::object{{"algorithm", "AES"}}};

    const auto req = WsFrame::ToHttpRequest(request, "Bearer eyJ...", "acct-1", "eu-central-1", "dev");

    BOOST_CHECK_EQUAL(req.method(), http::verb::post);
    BOOST_CHECK_EQUAL(req["x-euclid-target"], "ekm");
    BOOST_CHECK_EQUAL(req["x-euclid-action"], "create-key");
    BOOST_CHECK_EQUAL(req["authorization"], "Bearer eyJ...");
    BOOST_CHECK_EQUAL(req["x-euclid-account-id"], "acct-1");
    BOOST_CHECK_EQUAL(req["x-euclid-region"], "eu-central-1");
    BOOST_CHECK_EQUAL(req["x-euclid-namespace"], "dev");
    BOOST_CHECK_EQUAL(req.body(), R"({"algorithm":"AES"})");
}

BOOST_AUTO_TEST_CASE(ToHttpRequest_EmptyScopeFields_OmitsHeaders) {
    const WsFrame::Request request{.id = "abc", .target = "eam", .action = "login", .body = boost::json::object{}};

    const auto req = WsFrame::ToHttpRequest(request, "", "", "", "");

    BOOST_CHECK(req["authorization"].empty());
    BOOST_CHECK(req["x-euclid-account-id"].empty());
    BOOST_CHECK(req["x-euclid-region"].empty());
    BOOST_CHECK(req["x-euclid-namespace"].empty());
}

// ── BuildResponseFrame() ─────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(BuildResponseFrame_JsonBody_EmbedsParsedBody) {
    http::response<http::string_body> res{http::status::ok, 11};
    res.body() = R"({"ern":"ern:euclid:keys:..."})";
    res.prepare_payload();

    const auto frame = WsFrame::BuildResponseFrame("abc", res);
    const auto jv = boost::json::parse(frame);

    BOOST_CHECK_EQUAL(jv.at("type").as_string(), "response");
    BOOST_CHECK_EQUAL(jv.at("id").as_string(), "abc");
    BOOST_CHECK_EQUAL(jv.at("status").as_int64(), 200);
    BOOST_CHECK_EQUAL(jv.at("body").at("ern").as_string(), "ern:euclid:keys:...");
}

BOOST_AUTO_TEST_CASE(BuildResponseFrame_EmptyBody_EmbedsNull) {
    http::response<http::string_body> res{http::status::no_content, 11};
    res.prepare_payload();

    const auto frame = WsFrame::BuildResponseFrame("abc", res);
    const auto jv = boost::json::parse(frame);

    BOOST_CHECK(jv.at("body").is_null());
}

// ── BuildErrorFrame() / BuildEventFrame() ────────────────────────────────────

BOOST_AUTO_TEST_CASE(BuildErrorFrame_HasExpectedShape) {
    const auto frame = WsFrame::BuildErrorFrame("abc", 400, "bad request");
    const auto jv = boost::json::parse(frame);

    BOOST_CHECK_EQUAL(jv.at("type").as_string(), "error");
    BOOST_CHECK_EQUAL(jv.at("id").as_string(), "abc");
    BOOST_CHECK_EQUAL(jv.at("status").as_int64(), 400);
    BOOST_CHECK_EQUAL(jv.at("error").as_string(), "bad request");
}

BOOST_AUTO_TEST_CASE(BuildEventFrame_HasExpectedShape) {
    const auto frame = WsFrame::BuildEventFrame("ekm.key.created", "acct-1", "eu-central-1", boost::json::object{{"ern", "ern:euclid:keys:..."}});
    const auto jv = boost::json::parse(frame);

    BOOST_CHECK_EQUAL(jv.at("type").as_string(), "event");
    BOOST_CHECK(jv.at("id").as_string().size() > 0);
    BOOST_CHECK_EQUAL(jv.at("topic").as_string(), "ekm.key.created");
    BOOST_CHECK_EQUAL(jv.at("accountId").as_string(), "acct-1");
    BOOST_CHECK_EQUAL(jv.at("region").as_string(), "eu-central-1");
    BOOST_CHECK_EQUAL(jv.at("body").at("ern").as_string(), "ern:euclid:keys:...");
}
