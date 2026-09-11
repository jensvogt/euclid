// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE UnixSocketServerBindTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <filesystem>
#include <string>

// Euclid includes
#include <euclid/core/UnixSocketServer.h>

// A module binds its socket wherever the configuration says, and that directory is routinely not
// there: /var/run is a tmpfs on Linux and is empty again after every reboot, and a container image
// has only the directories it was built with. bind() answers ENOENT for a missing *parent*, which
// took down every module at once and read as though the socket were expected to already exist.

namespace {

    // A concrete server, since UnixSocketServer dispatches through a pure virtual.
    class TestServer final : public Euclid::Core::UnixSocketServer {
    public:
        TestServer(const std::string &name, const std::string &path) : UnixSocketServer(name, path, 1) {}

    private:
        boost::beast::http::response<boost::beast::http::string_body>
        Dispatch(const boost::beast::http::request<boost::beast::http::string_body> &request) override {
            return boost::beast::http::response<boost::beast::http::string_body>{boost::beast::http::status::ok, request.version()};
        }
    };

    std::filesystem::path uniqueRoot() {
        return std::filesystem::temp_directory_path() / ("euclid-socket-test-" + std::to_string(::getpid()));
    }

}// namespace

BOOST_AUTO_TEST_CASE(ASocketDirectoryThatIsNotThereIsCreated) {

    const auto root = uniqueRoot();
    std::filesystem::remove_all(root);

    // Two levels below anything that exists, which is what a fresh host looks like: neither
    // /var/run/euclid nor a socket-dir under it has been made by anybody.
    const auto socket = root / "run" / "euclid" / "euclid-test.sock";
    BOOST_TEST_REQUIRE(!std::filesystem::exists(socket.parent_path()));

    {
        BOOST_CHECK_NO_THROW(TestServer("test", socket.string()));
        BOOST_TEST(std::filesystem::exists(socket.parent_path()));
        BOOST_TEST(std::filesystem::is_socket(socket));
    }

    std::filesystem::remove_all(root);
}

BOOST_AUTO_TEST_CASE(ASocketLeftBehindByAPreviousRunIsReplaced) {

    const auto root = uniqueRoot();
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    // A process killed rather than stopped leaves its socket on disk, and bind() refuses to
    // replace a path that exists - so the file is removed first, and creating the directory must
    // not have disturbed that.
    const auto socket = root / "euclid-test.sock";
    { TestServer first("first", socket.string()); }
    BOOST_TEST_REQUIRE(std::filesystem::exists(socket));

    BOOST_CHECK_NO_THROW(TestServer("second", socket.string()));

    std::filesystem::remove_all(root);
}
