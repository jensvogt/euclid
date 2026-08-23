#if defined(_WIN32)

// C++ includes
#include <vector>

// Euclid includes
#include <euclid/manager/ControllerPlatform.h>

namespace Euclid::main::Platform {

    // Standard MSVC/CommandLineToArgvW quoting algorithm: only quotes when needed, and
    // doubles up backslashes that immediately precede either a literal quote or the
    // closing quote, so the child's own argv parser recovers exactly the original string.
    std::string QuoteArg(const std::string &arg) {
        if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos) {
            return arg;
        }

        std::string result = "\"";
        for (auto it = arg.begin();; ++it) {
            unsigned backslashes = 0;
            while (it != arg.end() && *it == '\\') {
                ++it;
                ++backslashes;
            }

            if (it == arg.end()) {
                result.append(backslashes * 2, '\\');
                break;
            }
            if (*it == '"') {
                result.append(backslashes * 2 + 1, '\\');
                result.push_back('"');
            } else {
                result.append(backslashes, '\\');
                result.push_back(*it);
            }
        }
        result.push_back('"');
        return result;
    }

    namespace {
        // Restricts handle inheritance to exactly the two pipe write-ends, so the child
        // doesn't also inherit every other inheritable handle the manager happens to have
        // open (listening sockets, other modules' pipes, ...) - the default
        // bInheritHandles=TRUE behavior without this attribute list.
        struct AttributeList {
            std::vector<std::byte> buffer;
            LPPROC_THREAD_ATTRIBUTE_LIST list = nullptr;

            bool init(HANDLE handles[], const DWORD count) {
                SIZE_T size = 0;
                InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
                buffer.resize(size);
                list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buffer.data());
                if (!InitializeProcThreadAttributeList(list, 1, 0, &size)) {
                    list = nullptr;
                    return false;
                }
                if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                                handles, count * sizeof(HANDLE), nullptr, nullptr)) {
                    DeleteProcThreadAttributeList(list);
                    list = nullptr;
                    return false;
                }
                return true;
            }

            ~AttributeList() {
                if (list) DeleteProcThreadAttributeList(list);
            }
        };
    }// namespace

    bool SpawnInstance(const Dto::ModuleConfig &config, const std::string &instanceSocket,
                        pid_t &outPid, HANDLE &outProcessHandle, int &outStdoutFd, int &outStderrFd) {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE outRead = nullptr, outWrite = nullptr, errRead = nullptr, errWrite = nullptr;
        if (!CreatePipe(&outRead, &outWrite, &sa, 0)) return false;
        SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);

        if (!CreatePipe(&errRead, &errWrite, &sa, 0)) {
            CloseHandle(outRead);
            CloseHandle(outWrite);
            return false;
        }
        SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);

        std::string cmdLine = QuoteArg(config.executable);
        for (auto &a: config.args) {
            cmdLine += ' ';
            cmdLine += QuoteArg(a);
        }
        cmdLine += " --socket ";
        cmdLine += QuoteArg(instanceSocket);

        std::vector<char> cmdLineBuf(cmdLine.begin(), cmdLine.end());
        cmdLineBuf.push_back('\0');

        HANDLE inheritHandles[2] = {outWrite, errWrite};
        AttributeList attrs;
        const bool haveAttrs = attrs.init(inheritHandles, 2);

        STARTUPINFOEXA siex{};
        siex.StartupInfo.cb = sizeof(siex);
        siex.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        siex.StartupInfo.hStdOutput = outWrite;
        siex.StartupInfo.hStdError = errWrite;
        siex.StartupInfo.hStdInput = nullptr;
        siex.lpAttributeList = attrs.list;

        PROCESS_INFORMATION pi{};
        DWORD flags = CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW;
        if (haveAttrs) flags |= EXTENDED_STARTUPINFO_PRESENT;

        const BOOL ok = CreateProcessA(
                nullptr, cmdLineBuf.data(), nullptr, nullptr, TRUE, flags,
                nullptr, nullptr, &siex.StartupInfo, &pi);

        // The child (if created) now holds its own copies of the write ends; the parent's
        // copies must be closed or the pipe's read end never sees EOF once the child exits.
        CloseHandle(outWrite);
        CloseHandle(errWrite);

        if (!ok) {
            CloseHandle(outRead);
            CloseHandle(errRead);
            return false;
        }
        CloseHandle(pi.hThread);

        const int stdoutFd = _open_osfhandle(reinterpret_cast<intptr_t>(outRead), _O_RDONLY);
        const int stderrFd = _open_osfhandle(reinterpret_cast<intptr_t>(errRead), _O_RDONLY);
        if (stdoutFd == -1 || stderrFd == -1) {
            if (stdoutFd == -1) CloseHandle(outRead); else _close(stdoutFd);
            if (stderrFd == -1) CloseHandle(errRead); else _close(stderrFd);
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hProcess);
            return false;
        }

        outPid = static_cast<pid_t>(pi.dwProcessId);
        outProcessHandle = pi.hProcess;
        outStdoutFd = stdoutFd;
        outStderrFd = stderrFd;
        return true;
    }

    void RequestGracefulStop(const pid_t pid) {
        // CTRL_BREAK_EVENT targets a process GROUP id, which for a process created with
        // CREATE_NEW_PROCESS_GROUP is its own pid.
        GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, static_cast<DWORD>(pid));
    }

    // ReSharper disable once CppParameterMayBeConst
    void ForceKill(HANDLE processHandle) {
        if (processHandle) TerminateProcess(processHandle, 1);
    }

    // ReSharper disable once CppParameterMayBeConst
    bool IsRunning(HANDLE processHandle) {
        if (!processHandle) return false;
        DWORD exitCode = 0;
        if (!GetExitCodeProcess(processHandle, &exitCode)) return false;
        return exitCode == STILL_ACTIVE;
    }

}// namespace Euclid::main::Platform

#endif
