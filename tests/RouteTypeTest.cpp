// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE RouteTypeTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <string>

// MongoDB includes
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>

// Euclid includes
#include <euclid/database/entity/eag/Route.h>

using Euclid::Database::Entity::EAG::Route;
using Euclid::Database::Entity::EAG::RouteAuthentication;
using Euclid::Database::Entity::EAG::RouteType;
using Euclid::Database::Entity::EAG::RouteTypeFromString;
using Euclid::Database::Entity::EAG::RouteTypeToString;

// A route's type decides which of two entirely different request paths serves it: PROXY reads a
// request and forwards it, UPLOAD terminates it and streams the body into a bucket. Two things
// have to hold for that to be safe to store in a database:
//
//   - the type and the upload settings survive a round trip, or a route configured as an upload
//     endpoint comes back as something else after a restart;
//   - a stored value nobody can parse falls back to PROXY, which fails safe - such a route names
//     no application and no module, so it finds no backend and answers 503 rather than becoming
//     an upload endpoint by accident.

namespace {

    Route uploadRoute() {
        Route route;
        route.routeId = "onix-drop";
        route.path = "/upload/onix";
        route.type = RouteType::UPLOAD;
        route.authentication = RouteAuthentication::EUCLID;
        route.upload.bucket = "ern:esm:eu-central-1:000000000000:development:bucket:onix3-incoming";
        route.upload.keyPrefix = "suppliers/jvo/";
        route.upload.maxBytes = 20L * 1024 * 1024 * 1024;
        route.upload.partSize = 5L * 1024 * 1024;
        route.upload.contentTypes = {"application/xml", "application/octet-stream"};
        return route;
    }

}// namespace

// ── Parsing ─────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(TheTwoTypesParseInAnyCase) {
    BOOST_TEST((RouteTypeFromString("PROXY") == RouteType::PROXY));
    BOOST_TEST((RouteTypeFromString("proxy") == RouteType::PROXY));
    BOOST_TEST((RouteTypeFromString("Upload") == RouteType::UPLOAD));
    BOOST_TEST((RouteTypeFromString("UPLOAD") == RouteType::UPLOAD));
}

// Returning nothing rather than a default is what lets the two callers disagree: a request naming
// a type nobody knows is refused, a stored one is read as PROXY.
BOOST_AUTO_TEST_CASE(AnUnknownTypeParsesToNothing) {
    BOOST_TEST(!RouteTypeFromString("download").has_value());
    BOOST_TEST(!RouteTypeFromString("").has_value());
    BOOST_TEST(!RouteTypeFromString("UNKNOWN").has_value());
}

BOOST_AUTO_TEST_CASE(TypesRenderAsTheyParse) {
    BOOST_TEST(RouteTypeToString(RouteType::PROXY) == "PROXY");
    BOOST_TEST(RouteTypeToString(RouteType::UPLOAD) == "UPLOAD");
}

// ── Round trip ──────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(AnUploadRouteSurvivesARoundTrip) {
    const auto original = uploadRoute();
    const auto stored = original.toDocument();
    const auto read = Route::fromDocument(stored.view());

    BOOST_TEST((read.type == RouteType::UPLOAD));
    BOOST_TEST(read.upload.bucket == original.upload.bucket);
    BOOST_TEST(read.upload.keyPrefix == original.upload.keyPrefix);
    BOOST_TEST(read.upload.maxBytes == original.upload.maxBytes);
    BOOST_TEST(read.upload.partSize == original.upload.partSize);
    BOOST_REQUIRE(read.upload.contentTypes.size() == 2U);
    BOOST_TEST(read.upload.contentTypes[0] == "application/xml");
    BOOST_TEST(read.upload.contentTypes[1] == "application/octet-stream");
}

// A size that does not fit in 32 bits is the ordinary case here, not the edge one: the delivery
// this was built for is 12 GB.
BOOST_AUTO_TEST_CASE(SizesBeyondFourGigabytesSurvive) {
    auto original = uploadRoute();
    original.upload.maxBytes = 21474836480L;

    const auto read = Route::fromDocument(original.toDocument().view());

    BOOST_TEST(read.upload.maxBytes == 21474836480L);
}

BOOST_AUTO_TEST_CASE(AProxyRouteRoundTripsAsProxy) {
    Route route;
    route.routeId = "orders";
    route.path = "/orders";
    route.applicationId = "order-service";

    const auto read = Route::fromDocument(route.toDocument().view());

    BOOST_TEST((read.type == RouteType::PROXY));
    BOOST_TEST(read.applicationId == "order-service");
    BOOST_TEST(read.upload.bucket.empty());
}

// ── Reading what was stored before uploads existed ──────────────────────────

// Every route written before this field existed has no "type" at all, and has to keep working -
// so absent means PROXY, which is what it always was.
BOOST_AUTO_TEST_CASE(ARouteStoredWithoutATypeIsAProxy) {
    const auto document = bsoncxx::builder::basic::make_document(
            bsoncxx::builder::basic::kvp("routeId", "legacy"),
            bsoncxx::builder::basic::kvp("path", "/legacy"),
            bsoncxx::builder::basic::kvp("applicationId", "legacy-service"));

    const auto read = Route::fromDocument(document.view());

    BOOST_TEST((read.type == RouteType::PROXY));
    BOOST_TEST(read.applicationId == "legacy-service");
}

BOOST_AUTO_TEST_CASE(AStoredTypeNobodyCanParseFallsBackToProxy) {
    const auto document = bsoncxx::builder::basic::make_document(
            bsoncxx::builder::basic::kvp("routeId", "corrupt"),
            bsoncxx::builder::basic::kvp("path", "/corrupt"),
            bsoncxx::builder::basic::kvp("type", "UPLAOD"));

    const auto read = Route::fromDocument(document.view());

    // Not UPLOAD, which is the point: a typo in the database must not produce a write endpoint.
    BOOST_TEST((read.type == RouteType::PROXY));
}

// An upload sub-document that is missing fields reads as zeroes and empties rather than throwing,
// the same way every other partially-written document in the store does.
BOOST_AUTO_TEST_CASE(APartialUploadDocumentReadsAsEmpty) {
    const auto document = bsoncxx::builder::basic::make_document(
            bsoncxx::builder::basic::kvp("routeId", "partial"),
            bsoncxx::builder::basic::kvp("type", "UPLOAD"),
            bsoncxx::builder::basic::kvp("upload", bsoncxx::builder::basic::make_document(
                                                           bsoncxx::builder::basic::kvp("bucket", "ern:esm:x"))));

    const auto read = Route::fromDocument(document.view());

    BOOST_TEST((read.type == RouteType::UPLOAD));
    BOOST_TEST(read.upload.bucket == "ern:esm:x");
    BOOST_TEST(read.upload.keyPrefix.empty());
    BOOST_TEST(read.upload.maxBytes == 0L);
    BOOST_TEST(read.upload.contentTypes.empty());
}
