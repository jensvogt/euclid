//#include <awsmock/core/config/Configuration.h>
#include <../include/euclid/core/LogStream.h>
#include <../include/euclid/core/Configuration.h>
//#include <awsmock/core/logging/LoggingServer.h>

namespace Euclid::Core {

    long LogStream::_logSize = DEFAULT_LOG_SIZE;
    int LogStream::_logCount = DEFAULT_LOG_COUNT;
    std::string LogStream::_logDir;
    std::string LogStream::_logPrefix;
    std::string LogStream::_currentLevel = "info";
    boost::log::trivial::severity_level LogStream::_severity;
    boost::shared_ptr<boost::log::sinks::synchronous_sink<boost::log::sinks::text_ostream_backend> > LogStream::_consoleSink;
    boost::shared_ptr<boost::log::sinks::synchronous_sink<boost::log::sinks::text_file_backend> > LogStream::_fileSink;
    // boost::shared_ptr<websocket::stream<beast::tcp_stream> > LogStream::_ws;
    // boost::shared_ptr<LogWebsocketSink> LogStream::webSocketBackend(new LogWebsocketSink(_ws));
    // boost::shared_ptr<webSocketSink_t> LogStream::webSocketSink(new webSocketSink_t(webSocketBackend));

    inline std::string processFuncName(const char *func) {
#if (defined(_WIN32) && !defined(__MINGW32__)) || defined(__OBJC__)
        return std::string(func);
#else
        const char *funcBegin = func;
        const char *funcEnd = strchr(funcBegin, '(');
        int foundTemplate = 0;

        if (!funcEnd) {
            return {func};
        }

        for (const char *i = funcEnd - 1; i >= funcBegin; --i) // search backwards for the first space char
        {
            if (*i == '>') {
                foundTemplate++;
            } else if (*i == '<') {
                foundTemplate--;
            } else if (*i == ' ' && foundTemplate == 0) {
                funcBegin = i + 1;
                break;
            }
        }
        auto f = std::string(funcBegin, funcEnd);
        if (const size_t position = f.find("::"); position != std::string::npos) {
            return f.substr(position + 2);
        }
        return f;

#endif
    }

    void LogFormatter(boost::log::record_view const &rec, boost::log::formatting_ostream &strm) {

        std::string func = processFuncName(boost::log::extract<std::string>("Function", rec)->c_str());

        auto date_time_formatter = boost::log::expressions::stream << boost::log::expressions::format_date_time<boost::posix_time::ptime>("TimeStamp", "%Y-%m-%d %H:%M:%S.%f");
        date_time_formatter(rec, strm);

        // The same for the severity
        strm << " [" << rec[boost::log::trivial::severity] << "]";
        strm << " [" << rec[thread_id].get().native_id() << "]";
        strm << " [" << func << ":" << boost::log::extract<int>("Line", rec) << "] ";

        // Finally, put the record message to the stream
        strm << rec[boost::log::expressions::smessage];
    }

    void LogStream::Initialize() {

        namespace net = boost::asio;

#if defined(_WIN32) && defined(_DEBUG)
        // Module processes are spawned hidden (CREATE_NO_WINDOW) by the manager, so the debug
        // CRT's default abort()/assert() dialog is invisible and blocks forever with nothing to
        // click - redirect those reports to stderr instead of a modal window.
        _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
        _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif

        boost::log::add_common_attributes();
        _consoleSink = boost::log::add_console_log(std::cout);

        // No ANSI color codes: under systemd (or any other non-interactive launcher), stdout is
        // a pipe straight into journald, which stores whatever bytes it receives verbatim - a
        // colorizing formatter would leave raw \033[...m escape sequences embedded in the
        // journal. journalctl -o cat prints the MESSAGE field completely unescaped, so viewing
        // that history later dumps raw escape sequences straight at whatever terminal is reading
        // it, which can hang or crash a terminal emulator.
        _consoleSink->set_formatter(&LogFormatter);
        _consoleSink->set_filter(boost::log::trivial::severity >= boost::log::trivial::info);
        _consoleSink->locked_backend()->auto_flush(true);

        if (!Configuration::instance().getOr<bool>("euclid.logging.console-active", true)) {
            RemoveConsoleLogs();
        }
    }

    std::string LogStream::GetSeverity() {
        return _currentLevel;
    }

    void LogStream::SetSeverity(const std::string &lvl) {
        _currentLevel = lvl;
        from_string(lvl.c_str(), lvl.length(), _severity);
        const boost::shared_ptr<boost::log::core> core = boost::log::core::get();
        core->set_filter(boost::log::trivial::severity >= _severity);
    }

    void LogStream::AddFile(const std::string &dir, const std::string &prefix, long size, int count) {
#ifdef _WIN32
        _fileSink = add_file_log(
            boost::log::keywords::file_name = dir + "\\" + prefix + ".log ", boost::log::keywords::rotation_size = size,
            boost::log::keywords::target_file_name = dir + "\\" + prefix + "_ % N.log ", boost::log::keywords::format = &LogFormatter);
#else
        _fileSink = add_file_log(
            boost::log::keywords::file_name = dir + "/" + prefix + ".log",
            boost::log::keywords::rotation_size = size,
            boost::log::keywords::target_file_name = dir + "/" + prefix + "_%N.log",
            boost::log::keywords::format = &LogFormatter);
#endif

        // Set level
        _fileSink->set_filter(boost::log::trivial::severity >= _severity);

        _fileSink->locked_backend()->set_file_collector(boost::log::sinks::file::make_collector(
            boost::log::keywords::target = dir,
            boost::log::keywords::max_files = count));

        _fileSink->locked_backend()->scan_for_files();

        log_info << "Start logging to file, dir: " << dir << ", prefix: " << prefix << " size: " << size << " count: " << count;
    }

    //
    // void LogStream::AddWebSocket(websocket::stream<beast::tcp_stream> &ws) {
    //     const boost::shared_ptr<boost::log::core> core = boost::log::core::get();
    //     webSocketBackend = boost::make_shared<LogWebsocketSink>(boost::make_shared<websocket::stream<beast::tcp_stream> >(std::move(ws)));
    //     webSocketSink = boost::make_shared<webSocketSink_t>(webSocketBackend);
    //     webSocketSink->set_formatter(&LogFormatter);
    //     webSocketSink->set_filter(boost::log::trivial::severity >= _severity);
    //     core->add_sink(webSocketSink);
    // }

    // void LogStream::AddLoggingWebSocket(boost::asio::io_context &ioc, unsigned int port) {
    //     auto mgr = std::make_shared<Service::Logging::WebSocketSessionManager>();
    //
    //     // Start WebSocket Server in a background thread
    //     boost::thread([mgr, &ioc, port]() {
    //         RunLoggingWebSocketServer(ioc, port, mgr);
    //     }).detach();
    //
    //     // Setup Boost.Log Sink
    //     auto backend = boost::make_shared<Service::Logging::WebSocketSinkBackend>(mgr);
    //     using sink_t = boost::log::sinks::synchronous_sink<Service::Logging::WebSocketSinkBackend>;
    //     const auto sink = boost::make_shared<sink_t>(backend);
    //     sink->set_formatter(&LogFormatter);
    //     sink->set_filter(boost::log::trivial::severity >= boost::log::trivial::info);
    //
    //     boost::log::core::get()->add_sink(sink);
    // }

    // void LogStream::RemoveWebSocketSink() {
    //     const boost::shared_ptr<boost::log::core> core = boost::log::core::get();
    //     core->remove_sink(webSocketSink);
    // }

    void LogStream::RemoveConsoleLogs() {
        boost::log::core::get()->remove_sink(_consoleSink);
    }

    void LogStream::LogRaw(const std::string &message) {
        std::cout << message << '\n';
        std::cout.flush();
    }
} // namespace Euclid::Core