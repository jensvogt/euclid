// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE CredentialsFileTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <filesystem>
#include <fstream>
#include <string>

// Euclid includes
#include <euclid/cli/credentials/Credentials.h>

using Euclid::CLI::Credentials;

// Credentials::Load(path) reads a session from a named file rather than from the one belonging to
// the invoking user.
//
// It exists for euclid-wrk running as a Windows service. A service runs as Local System, whose
// USERPROFILE is C:\Windows\system32\config\systemprofile, so the file "euclid-cli eam login" wrote
// at an administrator's own prompt is in a directory the service never reads - and a worker with no
// credentials exits 1 (worker-nodes.md §3.2). The service is given an explicit path instead.
//
// Worth testing rather than assuming, because every failure here looks identical from the outside:
// the worker says "not logged in" and stops, whether the file was absent, unreadable, truncated or
// simply had no token in it. The per-user overload is deliberately not exercised - it reads
// $HOME/.euclid/credentials, and a test that wrote there would clobber the credentials of whoever
// ran it.

namespace {

    // A directory of this test's own, removed on the way out. Named after the test module rather
    // than randomised: a leftover from a previous crashed run should be reused and overwritten, not
    // accumulated one directory per run.
    struct TempDir {
        std::filesystem::path path;

        TempDir() : path(std::filesystem::temp_directory_path() / "euclid-credentials-file-test") {
            std::filesystem::remove_all(path);
            std::filesystem::create_directories(path);
        }

        ~TempDir() {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    };

    void write(const std::filesystem::path &path, const std::string &contents) {
        std::ofstream out(path, std::ios::trunc);
        out << contents;
    }

}// namespace

BOOST_AUTO_TEST_CASE(AFileThatIsNotThereIsNotAnError) {

    const TempDir dir;

    // nullopt rather than a throw: a worker started before anybody logged in is an ordinary state
    // with a sentence to print, not an exception to unwind out of main.
    BOOST_TEST(!Credentials::Load((dir.path / "absent").string()).has_value());
}

BOOST_AUTO_TEST_CASE(AFullSessionIsReadBack) {

    const TempDir dir;
    const auto file = dir.path / "credentials";

    // The shape euclid-cli writes - see Credentials::Save. Spelled out here rather than produced by
    // calling Save(), which would write to the invoking user's home directory.
    write(file, R"({"token":"a-bearer-token",)"
                R"("userId":"node-principal",)"
                R"("accountId":"000000000000",)"
                R"("region":"eu-central-1",)"
                R"("accessKeyId":"AKIAEXAMPLE",)"
                R"("secretAccessKey":"a-secret",)"
                R"("isAdmin":false,)"
                R"("namespace":"production"})");

    const auto entry = Credentials::Load(file.string());
    BOOST_REQUIRE(entry.has_value());

    // Every field, because the worker signs with the token and scopes with the rest: an accountId
    // or namespace silently dropped here is a worker that registers as the wrong principal.
    BOOST_TEST(entry->token == "a-bearer-token");
    BOOST_TEST(entry->userId == "node-principal");
    BOOST_TEST(entry->accountId == "000000000000");
    BOOST_TEST(entry->region == "eu-central-1");
    BOOST_TEST(entry->accessKeyId == "AKIAEXAMPLE");
    BOOST_TEST(entry->secretAccessKey == "a-secret");
    BOOST_TEST(entry->nameSpace == "production");
    BOOST_TEST(entry->isAdmin == false);
}

BOOST_AUTO_TEST_CASE(AFileWithNoTokenIsRefused) {

    const TempDir dir;
    const auto file = dir.path / "credentials";

    // Valid JSON, no token. This is what a half-written or hand-edited file looks like, and
    // accepting it would get the worker past its own "not logged in" check and on to signing
    // requests with an empty bearer token - a 401 from the gateway, well away from the cause.
    write(file, R"({"userId":"node-principal","accountId":"000000000000"})");

    BOOST_TEST(!Credentials::Load(file.string()).has_value());
}

BOOST_AUTO_TEST_CASE(SomethingThatIsNotJsonIsRefused) {

    const TempDir dir;
    const auto file = dir.path / "credentials";

    write(file, "not json at all");

    BOOST_TEST(!Credentials::Load(file.string()).has_value());
}
