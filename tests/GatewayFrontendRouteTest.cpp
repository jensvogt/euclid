// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE GatewayFrontendRouteTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <filesystem>
#include <fstream>

// Euclid includes
#include <euclid/manager/FrontendRoute.h>

// The gateway serves euclid-web out of a directory on the manager's own disk, without a token -
// there is nobody to authenticate yet on the page where somebody logs in. That makes
// Frontend::ResolveFile() the boundary between a URL anyone can write and the filesystem of the
// host the manager runs on, and the cases below are the ways a path that looks like it stays
// inside that directory does not.

namespace fs = std::filesystem;
using namespace Euclid::main;

namespace {

    // A minimal euclid-web build: an entry document, one hashed bundle, one asset - and, next to
    // the directory rather than in it, the file every traversal case below is trying to reach.
    struct FrontendBuild {

        FrontendBuild() {
            root = fs::temp_directory_path() / "euclid-frontend-route-test";
            fs::remove_all(root);
            fs::create_directories(root / "assets");
            write(root / "index.html", "<!doctype html>");
            write(root / "main-A1B2C3.js", "console.log(1)");
            write(root / "assets" / "logo.png", "\x89PNG");
            write(root.parent_path() / "euclid-frontend-route-secret.txt", "secret");
        }

        ~FrontendBuild() {
            std::error_code ec;
            fs::remove_all(root, ec);
            fs::remove(root.parent_path() / "euclid-frontend-route-secret.txt", ec);
        }

        static void write(const fs::path &path, const std::string &content) {
            std::ofstream out(path, std::ios::binary);
            out << content;
        }

        fs::path root;
    };

}// namespace

BOOST_AUTO_TEST_SUITE(GatewayFrontendRouteTest)

BOOST_AUTO_TEST_CASE(TheRootPathIsTheIndexDocument) {

    const FrontendBuild frontend;

    const auto file = Frontend::ResolveFile(frontend.root, "/");
    BOOST_REQUIRE(file.has_value());
    BOOST_TEST(file->filename().string() == "index.html");
}

BOOST_AUTO_TEST_CASE(AFileThatExistsIsServed) {

    const FrontendBuild frontend;

    const auto bundle = Frontend::ResolveFile(frontend.root, "/main-A1B2C3.js");
    BOOST_REQUIRE(bundle.has_value());
    BOOST_TEST(bundle->filename().string() == "main-A1B2C3.js");

    const auto asset = Frontend::ResolveFile(frontend.root, "/assets/logo.png");
    BOOST_REQUIRE(asset.has_value());
    BOOST_TEST(asset->filename().string() == "logo.png");
}

BOOST_AUTO_TEST_CASE(AnApplicationRouteFallsBackToTheIndexDocument) {

    const FrontendBuild frontend;

    // No such file, and there is not meant to be: euclid-web resolves these in the browser.
    for (const auto *route: {"/buckets", "/users/42", "/monitoring/queues"}) {
        BOOST_TEST_CONTEXT("route " << route) {
            const auto file = Frontend::ResolveFile(frontend.root, route);
            BOOST_REQUIRE(file.has_value());
            BOOST_TEST(file->filename().string() == "index.html");
        }
    }
}

BOOST_AUTO_TEST_CASE(AMissingAssetIsNotTheIndexDocument) {

    const FrontendBuild frontend;

    // A named file that is not there is a broken deploy, and answering it with HTML hides that
    // behind a parse error somewhere else entirely.
    BOOST_TEST(!Frontend::ResolveFile(frontend.root, "/main-D4E5F6.js").has_value());
    BOOST_TEST(!Frontend::ResolveFile(frontend.root, "/assets/missing.png").has_value());
}

BOOST_AUTO_TEST_CASE(NothingOutsideTheDirectoryIsReachable) {

    const FrontendBuild frontend;

    const char *escapes[] = {
            "/../euclid-frontend-route-secret.txt",
            "/assets/../../euclid-frontend-route-secret.txt",
            "/%2e%2e/euclid-frontend-route-secret.txt",// only becomes ".." after decoding
            "/%2E%2E%2Feuclid-frontend-route-secret.txt",
            "/....//euclid-frontend-route-secret.txt",
            "//../euclid-frontend-route-secret.txt",
            "/assets/./../../euclid-frontend-route-secret.txt",
    };

    for (const auto *escape: escapes) {
        BOOST_TEST_CONTEXT("path " << escape) {
            const auto file = Frontend::ResolveFile(frontend.root, escape);
            const bool escaped = file.has_value() && file->filename().string() == "euclid-frontend-route-secret.txt";
            BOOST_TEST(!escaped, std::string(escape) + " resolved to a file outside the frontend directory");
        }
    }
}

BOOST_AUTO_TEST_CASE(AnAbsolutePathIsNotAnEscapeEither) {

    const FrontendBuild frontend;

    // The leading slashes are stripped before the join, so a doubled one is not an escape by
    // itself - it is an application route like any other, and comes back as the index document.
    const auto doubled = Frontend::ResolveFile(frontend.root, "//etc/passwd");
    BOOST_TEST((!doubled.has_value() || doubled->filename().string() == "index.html"));

    // A drive letter or a UNC host survives the stripping and would replace the root rather than
    // extend it, so it is refused before the join happens.
    BOOST_TEST(!Frontend::ResolveFile(frontend.root, "/C:/Windows/win.ini").has_value());
    BOOST_TEST(!Frontend::ResolveFile(frontend.root, "/%00/../euclid-frontend-route-secret.txt").has_value());
}

BOOST_AUTO_TEST_CASE(AMissingDirectoryResolvesToNothing) {

    // An installation without the web frontend deployed: every path has to come back empty so the
    // gateway answers as it did before there was a frontend at all.
    const auto absent = fs::temp_directory_path() / "euclid-frontend-route-test-absent";
    BOOST_TEST(!Frontend::ResolveFile(absent, "/").has_value());
    BOOST_TEST(!Frontend::ResolveFile(absent, "/main-A1B2C3.js").has_value());
    BOOST_TEST(!Frontend::ResolveFile(fs::path(), "/").has_value());
}

BOOST_AUTO_TEST_CASE(ScriptsAndStylesAreServedAsWhatTheyAre) {

    // A browser refuses a module script or a stylesheet that arrives as anything else, so these
    // three are the ones that decide whether the application starts at all.
    BOOST_TEST(Frontend::ContentType("main-A1B2C3.js") == "text/javascript; charset=utf-8");
    BOOST_TEST(Frontend::ContentType("styles-A1B2C3.css") == "text/css; charset=utf-8");
    BOOST_TEST(Frontend::ContentType("index.html") == "text/html; charset=utf-8");

    BOOST_TEST(Frontend::ContentType("logo.PNG") == "image/png");
    BOOST_TEST(Frontend::ContentType("inter.woff2") == "font/woff2");
    BOOST_TEST(Frontend::ContentType("data.unknown-extension") == "application/octet-stream");
    BOOST_TEST(Frontend::ContentType("LICENSE") == "application/octet-stream");
}

BOOST_AUTO_TEST_CASE(OnlyTheEntryDocumentIsTheEntryDocument) {

    BOOST_TEST(Frontend::IsIndex(fs::path("/usr/local/euclid/frontend/index.html")));
    BOOST_TEST(!Frontend::IsIndex(fs::path("/usr/local/euclid/frontend/main-A1B2C3.js")));
}

BOOST_AUTO_TEST_SUITE_END()
