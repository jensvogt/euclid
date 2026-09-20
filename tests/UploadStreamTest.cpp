// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE UploadStreamTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Boost includes
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/json.hpp>

// Euclid includes
#include <ProxyServer.h>
#include <euclid/core/HttpActionServer.h>
#include <euclid/core/JwtUtils.h>
#include <euclid/database/Database.h>
#include <euclid/database/RepositoryFactory.h>

namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace asio = boost::asio;
using tcp = boost::asio::ip::tcp;

using Euclid::EAG::Protocol;
using Euclid::EAG::ProxyServer;
using Route = Euclid::Database::Entity::EAG::Route;
using Euclid::Database::Entity::EAG::RouteAuthentication;
using Euclid::Database::Entity::EAG::RouteType;

// The gateway used to read every request with http::async_read into an http::request<string_body>,
// which meant two things nobody had chosen: the whole body was held in memory, and Beast's default
// 8 MB body limit applied. Neither is survivable for an upload route - the delivery this was built
// for is 12 GB.
//
// So the read is now header-first: the route is matched from the headers, and only then is it
// decided how the body should be read. What these tests pin down is that the decision is actually
// being made - that a body far past the old ceiling streams through an upload route a part at a
// time, that a route's own limits are applied before the body rather than after, and that a
// proxied request still behaves as it did.

namespace {

    // A port high enough to be free on a developer machine and in CI, and different per test so a
    // socket left in TIME_WAIT by one does not fail the next.
    unsigned short nextPort() {
        static unsigned short port = 28080;
        return ++port;
    }

    // Stands in for euclid's own gateway, and therefore for ESM behind it: it answers the three
    // actions an upload makes and records what it was sent. Everything the gateway does with an
    // upload body ends up here, so what this saw is what actually left the gateway.
    struct FakeEsm {

        explicit FakeEsm(const bool refuseParts = false) : port(nextPort()), _refuseParts(refuseParts) {
            _acceptor.open(tcp::v4());
            _acceptor.set_option(asio::socket_base::reuse_address(true));
            _acceptor.bind(tcp::endpoint(tcp::v4(), port));
            _acceptor.listen();
            _thread = std::thread([this] { run(); });
        }

        ~FakeEsm() {
            // Stopping the context rather than closing the acceptor: a thread blocked in a
            // synchronous accept() is not reliably woken by closing the socket under it, which is
            // a hang rather than a failure and so the worst kind. An async accept on a context
            // somebody else can stop has no such question about it.
            _ioc.stop();
            if (_thread.joinable()) _thread.join();
        }

        unsigned short port;

        std::vector<std::string> actions;
        std::uint64_t partBytes{0};
        std::vector<std::size_t> partSizes;
        std::string key;
        std::mutex mutex;

    private:

        void run() {
            accept();
            _ioc.run();
        }

        void accept() {
            _acceptor.async_accept([this](const boost::system::error_code &ec, tcp::socket socket) {
                if (ec) return;
                handle(std::move(socket));
                accept();
            });
        }

        void handle(tcp::socket socket) {

            beast::flat_buffer buffer;
            http::request_parser<http::string_body> parser;
            parser.body_limit(boost::none);

            boost::system::error_code ec;
            http::read(socket, buffer, parser, ec);
            if (ec) return;

            const auto &req = parser.get();
            const auto action = std::string(req["x-euclid-action"]);

            http::response<http::string_body> res{http::status::ok, req.version()};
            res.set(http::field::content_type, "application/json");
            {
                std::lock_guard lock(mutex);
                actions.push_back(action);
                if (action == "create-upload") {
                    key = std::string(boost::json::parse(req.body()).at("key").as_string());
                    res.body() = R"({"uploadId":"upload-1"})";
                } else if (action == "upload-part") {
                    if (_refuseParts) {
                        res.result(http::status::forbidden);
                        res.body() = R"({"error":"nope"})";
                    } else {
                        partBytes += req.body().size();
                        partSizes.push_back(req.body().size());
                        res.body() = R"({"partNumber":1})";
                    }
                } else if (action == "complete-upload") {
                    // Refused exactly as the real ESM refuses it: an upload with no parts at all
                    // answers 400 "Upload has no parts". A double that accepted it would let a
                    // zero-byte upload pass here and fail against a real installation, which is
                    // precisely what it did until an example script ran against one.
                    if (partSizes.empty()) {
                        res.result(http::status::bad_request);
                        res.body() = R"({"error":"Upload has no parts"})";
                    } else {
                        res.body() = R"({"key":"x"})";
                    }
                } else {
                    res.body() = R"({"key":"x"})";
                }
            }
            res.prepare_payload();
            http::write(socket, res, ec);
            socket.shutdown(tcp::socket::shutdown_both, ec);
        }

        asio::io_context _ioc;
        tcp::acceptor _acceptor{_ioc};
        std::thread _thread;
        bool _refuseParts;
    };

    Route uploadRoute(const std::string &path = "/upload", const long maxBytes = 0,
                      const std::vector<std::string> &contentTypes = {}) {
        Route route;
        route.routeId = "upload-test";
        route.path = path;
        route.type = RouteType::UPLOAD;
        route.authentication = RouteAuthentication::EUCLID;
        route.active = true;
        route.upload.bucket = "ern:esm:eu-central-1:000000000000:development:bucket:incoming";
        route.upload.partSize = 64 * 1024;
        route.upload.maxBytes = maxBytes;
        route.upload.contentTypes = contentTypes;
        return route;
    }

    // A gateway with one route in it, listening on its own port. The in-memory store stands in for
    // MongoDB, which is what lets this run anywhere.
    struct Gateway {

        explicit Gateway(const Route &route, const unsigned short esmPort = 0) : port(nextPort()) {
            Euclid::Database::Database::instance().initializeMemory();

            auto stored = route;
            Euclid::Database::RepositoryFactory::instance().eagRepository()->upsertRoute(stored);

            server = std::make_unique<ProxyServer>(
                    std::vector<ProxyServer::Listener>{{.port = port, .nameSpace = "", .protocol = Protocol::HTTP}},
                    // A one-second refresh, because stop() joins the refresh thread and that
                    // thread sleeps for a whole interval between ticks - so the interval is also
                    // how long shutting the gateway down can take.
                    1, 1, 60, esmPort, false, "");
            server->start();
        }

        ~Gateway() {
            if (server) server->stop();
        }

        unsigned short port;
        std::unique_ptr<ProxyServer> server;
    };

    // Sends one request and reads the answer, with the body written in pieces so the gateway is
    // really being asked to read a stream rather than one buffer that happens to be large.
    // A session for somebody the gateway will accept. Minted rather than faked: this is the
    // credential an upload route with --authentication euclid actually takes.
    std::string bearerToken() {
        return Euclid::Core::JwtUtils::CreateToken("jens", Euclid::Core::HttpActionServer::JwtSecret());
    }

    http::response<http::string_body> put(const unsigned short port, const std::string &target,
                                          const std::size_t bodySize, const std::string &contentType = "application/octet-stream",
                                          const std::string &token = "") {

        asio::io_context ioc;
        tcp::socket socket(ioc);
        socket.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));

        http::request<http::empty_body> header(http::verb::put, target, 11);
        header.set(http::field::host, "127.0.0.1");
        header.set(http::field::content_type, contentType);
        if (!token.empty()) header.set(http::field::authorization, "Bearer " + token);
        header.content_length(bodySize);

        http::request_serializer<http::empty_body> serializer(header);
        beast::error_code ec;
        http::write_header(socket, serializer, ec);

        // Written in chunks rather than in one go, which is the whole point: the gateway has to
        // cope with a body that arrives over many reads.
        const std::string chunk(16 * 1024, 'x');
        for (std::size_t sent = 0; sent < bodySize;) {
            const auto piece = std::min(chunk.size(), bodySize - sent);
            asio::write(socket, asio::buffer(chunk.data(), piece), ec);
            if (ec) break;
            sent += piece;
        }

        beast::flat_buffer buffer;
        http::response<http::string_body> response;
        http::read(socket, buffer, response, ec);

        boost::system::error_code ignored;
        socket.shutdown(tcp::socket::shutdown_both, ignored);
        return response;
    }

}// namespace


// ── Streaming ───────────────────────────────────────────────────────────────

// 32 MB is four times the limit the gateway used to have without anybody choosing it, and 512
// parts of the 64 KB this route asks for. What the fake ESM saw is what actually left the gateway,
// so this says both that the body arrived whole and that it arrived in pieces.
BOOST_AUTO_TEST_CASE(ALargeBodyIsStreamedToEsmInParts) {

    const FakeEsm esm;
    const Gateway gateway(uploadRoute(), esm.port);

    constexpr std::size_t size = 32 * 1024 * 1024;
    const auto response = put(gateway.port, "/upload/onix/big.xml", size, "application/octet-stream", bearerToken());

    BOOST_TEST(response.result_int() == 201);

    std::lock_guard lock(const_cast<FakeEsm &>(esm).mutex);
    BOOST_TEST(esm.partBytes == size);
    BOOST_TEST(esm.partSizes.size() >= 512U);

    // Never more than one part in memory, whatever the size of the object going through.
    for (const auto part: esm.partSizes) BOOST_TEST(part <= 64U * 1024U);

    // Staged, filled, then completed - in that order.
    BOOST_REQUIRE(esm.actions.size() >= 3U);
    BOOST_TEST(esm.actions.front() == "create-upload");
    BOOST_TEST(esm.actions.back() == "complete-upload");
}

// The key is the part of the path below the route, with the route's prefix in front of it - and
// nothing of the route itself.
BOOST_AUTO_TEST_CASE(TheKeyComesFromThePathBeneathTheRoute) {

    const FakeEsm esm;
    auto route = uploadRoute();
    route.upload.keyPrefix = "suppliers/jvo";
    const Gateway gateway(route, esm.port);

    const auto response = put(gateway.port, "/upload/onix/2026-09.xml", 1024, "application/octet-stream", bearerToken());

    BOOST_TEST(response.result_int() == 201);
    std::lock_guard lock(const_cast<FakeEsm &>(esm).mutex);
    BOOST_TEST(esm.key == "suppliers/jvo/onix/2026-09.xml");
}

// An empty object is a real object, and the key should hold it. ESM will not complete an upload
// with no parts at all, so a body of no bytes has to arrive as one part of no bytes.
BOOST_AUTO_TEST_CASE(AnEmptyBodyIsStoredAsOneEmptyPart) {

    const FakeEsm esm;
    const Gateway gateway(uploadRoute(), esm.port);

    const auto response = put(gateway.port, "/upload/empty.xml", 0, "application/octet-stream", bearerToken());

    BOOST_TEST(response.result_int() == 201);

    std::lock_guard lock(const_cast<FakeEsm &>(esm).mutex);
    BOOST_TEST(esm.partBytes == 0U);
    BOOST_REQUIRE(esm.partSizes.size() == 1U);
    BOOST_TEST(esm.partSizes[0] == 0U);
    BOOST_TEST(esm.actions.back() == "complete-upload");
}

// A part ESM refuses ends the upload there: nothing is completed, so the object is not left as a
// half-written version of itself - and what was staged is thrown away rather than left for
// somebody to find.
BOOST_AUTO_TEST_CASE(APartEsmRefusesEndsTheUploadAndAbandonsIt) {

    const FakeEsm esm(true);
    const Gateway gateway(uploadRoute(), esm.port);

    const auto response = put(gateway.port, "/upload/big.xml", 1024 * 1024, "application/octet-stream", bearerToken());

    BOOST_TEST(response.result_int() == 500);
    std::lock_guard lock(const_cast<FakeEsm &>(esm).mutex);
    BOOST_TEST((std::ranges::find(esm.actions, "complete-upload") == esm.actions.end()));
    BOOST_TEST((std::ranges::find(esm.actions, "abort-upload") != esm.actions.end()));
}

// ── Authentication ──────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(AnUploadWithNoCredentialIsRefused) {

    const FakeEsm esm;
    const Gateway gateway(uploadRoute(), esm.port);

    const auto response = put(gateway.port, "/upload/x.xml", 1024);

    BOOST_TEST(response.result_int() == 401);

    // And nothing was staged: the refusal happens before ESM is told anything at all.
    std::lock_guard lock(const_cast<FakeEsm &>(esm).mutex);
    BOOST_TEST(esm.actions.empty());
}

BOOST_AUTO_TEST_CASE(AnUploadWithAnUnreadableTokenIsRefused) {

    const FakeEsm esm;
    const Gateway gateway(uploadRoute(), esm.port);

    const auto response = put(gateway.port, "/upload/x.xml", 1024, "application/octet-stream", "not-a-token");

    BOOST_TEST(response.result_int() == 401);
}

// ── Refused before the body, not after ──────────────────────────────────────

BOOST_AUTO_TEST_CASE(ABodyLargerThanTheRouteAcceptsIsRefused) {

    const FakeEsm esm;
    const Gateway gateway(uploadRoute("/upload", 1024 * 1024), esm.port);

    const auto response = put(gateway.port, "/upload/big.xml", 4 * 1024 * 1024, "application/octet-stream", bearerToken());

    BOOST_TEST(response.result_int() == 413);
    std::lock_guard lock(const_cast<FakeEsm &>(esm).mutex);
    BOOST_TEST(esm.actions.empty());
}

BOOST_AUTO_TEST_CASE(AContentTypeTheRouteDoesNotTakeIsRefused) {

    const FakeEsm esm;
    const Gateway gateway(uploadRoute("/upload", 0, {"application/xml"}), esm.port);

    const auto response = put(gateway.port, "/upload/x.json", 1024, "application/json", bearerToken());

    BOOST_TEST(response.result_int() == 415);
}

// The parameters after a content type are not part of it: "application/xml; charset=utf-8" is the
// type the route named, and refusing it would be a surprise nobody could debug from the message.
BOOST_AUTO_TEST_CASE(AContentTypeWithParametersMatchesTheBareType) {

    const FakeEsm esm;
    const Gateway gateway(uploadRoute("/upload", 0, {"application/xml"}), esm.port);

    const auto response = put(gateway.port, "/upload/x.xml", 1024, "application/xml; charset=utf-8", bearerToken());

    BOOST_TEST(response.result_int() == 201);
}

// A key that climbs out of the route's prefix is refused rather than normalised - see
// ResolveUploadKey.
BOOST_AUTO_TEST_CASE(AKeyThatClimbsOutOfThePrefixIsRefused) {

    const FakeEsm esm;
    auto route = uploadRoute();
    route.upload.keyPrefix = "suppliers/jvo";
    const Gateway gateway(route, esm.port);

    const auto response = put(gateway.port, "/upload/../../etc/passwd", 16, "application/octet-stream", bearerToken());

    BOOST_TEST(response.result_int() == 400);
    std::lock_guard lock(const_cast<FakeEsm &>(esm).mutex);
    BOOST_TEST(esm.actions.empty());
}

BOOST_AUTO_TEST_CASE(APutToTheRouteRootNamesNoObject) {

    const FakeEsm esm;
    const Gateway gateway(uploadRoute(), esm.port);

    const auto response = put(gateway.port, "/upload", 16, "application/octet-stream", bearerToken());

    BOOST_TEST(response.result_int() == 400);
}

// ── Routing still works ─────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(APathNoRouteClaimsIsStillFourOhFour) {

    const Gateway gateway(uploadRoute());

    const auto response = put(gateway.port, "/nowhere/x.xml", 16);

    BOOST_TEST(response.result_int() == 404);
}

// A proxy route whose application has no running instance answers 503 - which means the request
// was read whole, matched, and taken down the proxy path rather than the upload one.
BOOST_AUTO_TEST_CASE(AProxyRouteStillTakesTheProxyPath) {

    Route route;
    route.routeId = "proxy-test";
    route.path = "/api";
    route.applicationId = "nothing-is-running";
    route.active = true;

    const Gateway gateway(route);

    const auto response = put(gateway.port, "/api/orders", 1024);

    BOOST_TEST(response.result_int() == 503);
}
