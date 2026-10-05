// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE WindowsProcessTest
#include <boost/test/unit_test.hpp>

#if defined(_WIN32)

// C++ includes
#include <chrono>
#include <filesystem>
#include <string>

#include <io.h>

// Euclid includes
#include <euclid/core/UnixSocketServer.h>
#include <euclid/core/WindowsProcess.h>

namespace WindowsProcess = Euclid::Core::WindowsProcess;

// What the manager starts modules and applications with on Windows, and what euclid-wrk starts the
// applications placed on a Windows node with. Run against real child processes - cmd.exe, which
// every Windows has - because what is being tested is the process boundary itself: what crosses it,
// and what does not.

namespace {

    // Everything a child wrote to one pipe, read until it closed it.
    std::string readAll(const int fd) {
        std::string text;
        char buffer[512];
        int got;
        while ((got = _read(fd, buffer, sizeof(buffer))) > 0) text.append(buffer, static_cast<std::size_t>(got));
        _close(fd);
        return text;
    }

    std::string trimmed(std::string text) {
        while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ')) text.pop_back();
        return text;
    }

    struct Closed {
        WindowsProcess::Spawned spawned;
        ~Closed() {
            if (spawned.process) CloseHandle(spawned.process);
            if (spawned.stopEvent) CloseHandle(spawned.stopEvent);
        }
    };

}// namespace

BOOST_AUTO_TEST_CASE(OutputArrivesOnTheTwoPipes) {

    const auto spawned = WindowsProcess::Spawn("echo", {"cmd.exe", "/c", "echo out& echo err 1>&2"}, {}, "");
    BOOST_TEST_REQUIRE(spawned.has_value());
    const Closed guard{*spawned};

    BOOST_TEST(trimmed(readAll(spawned->stdoutFd)) == "out");
    BOOST_TEST(trimmed(readAll(spawned->stderrFd)) == "err");
}

BOOST_AUTO_TEST_CASE(TheEnvironmentIsAddedToNotReplaced) {

    // A block passed to CreateProcess replaces the environment: passing only the additions would
    // start the child without PATH, which is how cmd.exe could not have been found here.
    const auto spawned = WindowsProcess::Spawn("env", {"cmd.exe", "/c", "echo %EUCLID_TEST_VALUE%"},
                                               {{"EUCLID_TEST_VALUE", "from the starter"}}, "");
    BOOST_TEST_REQUIRE(spawned.has_value());
    const Closed guard{*spawned};

    BOOST_TEST(trimmed(readAll(spawned->stdoutFd)) == "from the starter");
    std::ignore = readAll(spawned->stderrFd);
}

BOOST_AUTO_TEST_CASE(EveryChildIsToldItsStopEvent) {

    const auto spawned = WindowsProcess::Spawn("stop-event", {"cmd.exe", "/c", std::string("echo %") + Euclid::Core::STOP_EVENT_VARIABLE + "%"}, {}, "");
    BOOST_TEST_REQUIRE(spawned.has_value());
    const Closed guard{*spawned};

    BOOST_TEST(trimmed(readAll(spawned->stdoutFd)).starts_with("Local\\euclid-stop-"));
    std::ignore = readAll(spawned->stderrFd);
    BOOST_TEST(spawned->stopEvent != nullptr);
}

BOOST_AUTO_TEST_CASE(ItStartsInTheDirectoryItIsGiven) {

    const auto directory = std::filesystem::temp_directory_path();
    const auto spawned = WindowsProcess::Spawn("cd", {"cmd.exe", "/c", "cd"}, {}, directory.string());
    BOOST_TEST_REQUIRE(spawned.has_value());
    const Closed guard{*spawned};

    BOOST_TEST(std::filesystem::equivalent(trimmed(readAll(spawned->stdoutFd)), directory));
    std::ignore = readAll(spawned->stderrFd);
}

BOOST_AUTO_TEST_CASE(AnArgumentWithSpacesArrivesAsOneArgument) {

    // Quoted per argument, the way the child's own argv parsing undoes it.
    const auto spawned = WindowsProcess::Spawn("quote", {"cmd.exe", "/c", "echo", "two words"}, {}, "");
    BOOST_TEST_REQUIRE(spawned.has_value());
    const Closed guard{*spawned};

    BOOST_TEST(trimmed(readAll(spawned->stdoutFd)) == "\"two words\"");
    std::ignore = readAll(spawned->stderrFd);
}

BOOST_AUTO_TEST_CASE(AnExitCodeIsReadOnceItHasExited) {

    const auto spawned = WindowsProcess::Spawn("exit", {"cmd.exe", "/c", "exit 3"}, {}, "");
    BOOST_TEST_REQUIRE(spawned.has_value());
    const Closed guard{*spawned};

    std::ignore = readAll(spawned->stdoutFd);
    std::ignore = readAll(spawned->stderrFd);
    WaitForSingleObject(spawned->process, 10000);

    BOOST_TEST(!WindowsProcess::IsRunning(spawned->process));
    BOOST_TEST_REQUIRE(WindowsProcess::ExitCode(spawned->process).has_value());
    BOOST_TEST(*WindowsProcess::ExitCode(spawned->process) == 3U);
}

BOOST_AUTO_TEST_CASE(ARunningProcessHasNoExitCodeUntilItIsKilled) {

    // ping waits a second per count; this one is meant to outlive the assertions.
    const auto spawned = WindowsProcess::Spawn("sleep", {"cmd.exe", "/c", "ping -n 30 127.0.0.1 >nul"}, {}, "");
    BOOST_TEST_REQUIRE(spawned.has_value());
    const Closed guard{*spawned};

    BOOST_TEST(WindowsProcess::IsRunning(spawned->process));
    BOOST_TEST(!WindowsProcess::ExitCode(spawned->process).has_value());

    // A process that ignores its stop event - as cmd does, and as a JVM does - is ended by the kill.
    BOOST_TEST(WindowsProcess::RequestStop(spawned->stopEvent));
    WindowsProcess::Kill(spawned->process);
    BOOST_TEST(WaitForSingleObject(spawned->process, 5000) == WAIT_OBJECT_0);
    BOOST_TEST(!WindowsProcess::IsRunning(spawned->process));

    // The kill ends cmd itself; the pipes close once its child (ping) is gone as well, which the job
    // only guarantees when this process exits - so they are not read here.
    _close(spawned->stdoutFd);
    _close(spawned->stderrFd);
}

BOOST_AUTO_TEST_CASE(ACommandThatCannotBeFoundIsRefusedNotStarted) {

    BOOST_TEST(!WindowsProcess::Spawn("missing", {"euclid-no-such-program-anywhere.exe"}, {}, "").has_value());
    BOOST_TEST(!WindowsProcess::Spawn("empty", {}, {}, "").has_value());
}

#else

BOOST_AUTO_TEST_CASE(OnlyOnWindows) {
    BOOST_TEST(true);
}

#endif
