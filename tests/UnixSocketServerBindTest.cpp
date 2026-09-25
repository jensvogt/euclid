// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE UnixSocketServerBindTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <filesystem>
#include <string>

#ifdef _WIN32
#include <windows.h>    // GetFileAttributesW, DeleteFileW - see socketFileExists()
#include <process.h>    // _getpid
#endif

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

    int processId() {
#ifdef _WIN32
        return _getpid();
#else
        return ::getpid();
#endif
    }

    std::filesystem::path uniqueRoot() {
        return std::filesystem::temp_directory_path() / ("euclid-socket-test-" + std::to_string(processId()));
    }

    /**
     * @brief Whether a socket file was left on disk at this path.
     *
     * std::filesystem cannot answer that on Windows. An AF_UNIX socket there is a reparse point,
     * and exists()/is_socket() go through a stat that fails on it with ERROR_CANT_ACCESS_FILE: the
     * throwing overloads throw, and the error_code ones answer "no" about a file that is plainly
     * there. GetFileAttributes does not follow the reparse point, and answers.
     *
     * On Linux the stronger question is asked, because it can be: not merely that something is
     * there, but that it is a socket.
     */
    bool socketFileExists(const std::filesystem::path &path) {
#ifdef _WIN32
        return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
#else
        return std::filesystem::is_socket(path);
#endif
    }

    /**
     * @brief Removes the test's directory, socket files and all.
     *
     * remove_all() is no use for the same reason exists() is not: it stats what it walks, and on
     * Windows it fails on the socket file rather than deleting it. The sockets go first, by name
     * and through the API that can, and what is left is an ordinary directory.
     */
    void removeTree(const std::filesystem::path &root, const std::vector<std::filesystem::path> &sockets = {}) {
#ifdef _WIN32
        for (const auto &socket: sockets) DeleteFileW(socket.c_str());
#else
        (void) sockets;
#endif
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

}// namespace

BOOST_AUTO_TEST_CASE(ASocketDirectoryThatIsNotThereIsCreated) {

    const auto root = uniqueRoot();
    removeTree(root);

    // Two levels below anything that exists, which is what a fresh host looks like: neither
    // /var/run/euclid nor a socket-dir under it has been made by anybody.
    const auto socket = root / "run" / "euclid" / "euclid-test.sock";
    BOOST_TEST_REQUIRE(!std::filesystem::exists(socket.parent_path()));

    {
        BOOST_CHECK_NO_THROW(TestServer("test", socket.string()));
        BOOST_TEST(std::filesystem::exists(socket.parent_path()));
        BOOST_TEST(socketFileExists(socket));
    }

    removeTree(root, {socket});
}

BOOST_AUTO_TEST_CASE(ASocketLeftBehindByAPreviousRunIsReplaced) {

    const auto root = uniqueRoot();
    removeTree(root);
    std::filesystem::create_directories(root);

    // A process killed rather than stopped leaves its socket on disk, and bind() refuses to
    // replace a path that exists - so the file is removed first, and creating the directory must
    // not have disturbed that.
    //
    // The constructor is what does it, so the first server is destroyed without having stopped:
    // stop() would take the socket with it and there would be nothing left behind to replace.
    const auto socket = root / "euclid-test.sock";
    { TestServer first("first", socket.string()); }
    BOOST_TEST_REQUIRE(socketFileExists(socket));

    BOOST_CHECK_NO_THROW(TestServer("second", socket.string()));

    removeTree(root, {socket});
}
