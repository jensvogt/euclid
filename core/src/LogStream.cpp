// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//#include <awsmock/core/config/Configuration.h>
#include <euclid/core/LogStream.h>
#include <euclid/core/Configuration.h>
//#include <awsmock/core/logging/LoggingServer.h>

namespace Euclid::Core {

    long LogStream::_logSize = DEFAULT_LOG_SIZE;
    int LogStream::_logCount = DEFAULT_LOG_COUNT;
    std::string LogStream::_logDir;
    std::string LogStream::_logPrefix;
    std::string LogStream::_currentLevel = "info";
    boost::log::trivial::severity_level LogStream::_severity;
    std::atomic<boost::log::trivial::severity_level> LogStream::_defaultSeverity{boost::log::trivial::info};
    std::shared_ptr<const LogStream::ChannelLevels> LogStream::_channelLevels = std::make_shared<const LogStream::ChannelLevels>();
    std::shared_mutex LogStream::_channelMutex;
    std::string LogStream::_processChannel{LogStream::kDefaultChannel};
    boost::shared_ptr<boost::log::sinks::synchronous_sink<boost::log::sinks::text_ostream_backend> > LogStream::_consoleSink;
    boost::shared_ptr<boost::log::sinks::synchronous_sink<boost::log::sinks::text_file_backend> > LogStream::_fileSink;
    // boost::shared_ptr<websocket::stream<beast::tcp_stream> > LogStream::_ws;
    // boost::shared_ptr<LogWebsocketSink> LogStream::webSocketBackend(new LogWebsocketSink(_ws));
    // boost::shared_ptr<webSocketSink_t> LogStream::webSocketSink(new webSocketSink_t(webSocketBackend));

    static inline std::string processFuncName(const char *func) {
#if (defined(_WIN32) && !defined(__MINGW32__)) || defined(__OBJC__)
        return std::string(func);
#else
        const char *funcBegin = func;
        const char *funcEnd = strchr(funcBegin, '(');
        int foundTemplate = 0;

        if (!funcEnd) {
            return {func};
        }

        for (const char *i = funcEnd - 1; i >= funcBegin; --i)// search backwards for the first space char
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

    // Marks a record that is to be written exactly as it was given - see LogStream::LogVerbatim.
    constexpr auto kVerbatimAttribute = "Verbatim";

    // Reads a level name, including "off" - which is not a severity at all but the absence of one,
    // and is how a channel is silenced rather than merely turned down.
    std::optional<std::optional<boost::log::trivial::severity_level> > ParseLevel(const std::string &lvl) {

        if (lvl == "off" || lvl == "none" || lvl == "silent") return std::optional<boost::log::trivial::severity_level>{};

        boost::log::trivial::severity_level severity;
        if (!from_string(lvl.c_str(), lvl.length(), severity)) return std::nullopt;
        return std::optional{severity};
    }

    static void LogFormatter(boost::log::record_view const &rec, boost::log::formatting_ostream &strm) {

        // Output euclid read back from a process it started, already formatted by whatever wrote
        // it. Putting a second timestamp and a source location from this process in front of an
        // application's own log line helps nobody read either.
        if (boost::log::extract<bool>(kVerbatimAttribute, rec)) {
            strm << rec[boost::log::expressions::smessage];
            return;
        }

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

    std::optional<boost::log::trivial::severity_level> LogStream::SeverityFor(const std::string &channel) {

        // The pointer only: past this the table it points at is immutable, so the lock is not
        // held while it is searched.
        std::shared_ptr<const ChannelLevels> levels;
        {
            std::shared_lock lock(_channelMutex);
            levels = _channelLevels;
        }

        // The channel itself, then each enclosing channel in turn: "app.parser" is answered by a
        // level set for "app.parser", failing that by one set for "app", and failing that by the
        // process-wide level. That is what lets every application be turned down with one entry
        // and one of them be turned back up with a second.
        std::string_view name(channel);
        while (!name.empty()) {
            if (const auto it = levels->find(std::string(name)); it != levels->end()) return it->second;

            const auto dot = name.rfind('.');
            if (dot == std::string_view::npos) break;
            name = name.substr(0, dot);
        }
        return _defaultSeverity.load(std::memory_order_relaxed);
    }

    // The one filter the log core runs, for every record every thread offers. Everything a level
    // decides is decided here - the sinks have no filters of their own, because a sink that
    // filtered too would silently overrule this one (which is exactly what a hard-wired "info" on
    // the console sink used to do to euclid.logging.level: debug).
    static bool ChannelFilter(const boost::log::attribute_value_set &attributes) {

        const auto severity = attributes[boost::log::trivial::severity];
        if (!severity) return true;

        // "::channel" - the attribute keyword is declared at global scope, alongside the logger
        // it belongs to.
        const auto recordChannel = attributes[::channel];
        const auto threshold = LogStream::SeverityFor(recordChannel ? recordChannel.get() : std::string(LogStream::kDefaultChannel));

        // No threshold at all means the channel is off, whatever the record's severity.
        return threshold.has_value() && severity.get() >= *threshold;
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
        _consoleSink->locked_backend()->auto_flush(true);

        // One filter, on the core, so a channel's level is the only thing that decides whether a
        // record is written. The console sink used to carry a second one fixed at "info", which
        // quietly discarded everything euclid.logging.level: debug asked for.
        boost::log::core::get()->set_filter(&ChannelFilter);

        if (!Configuration::instance().getOr<bool>("euclid.logging.console-active", true)) {
            RemoveConsoleLogs();
        }
    }

    void LogStream::SetProcessChannel(const std::string &channel) {
        _processChannel = channel;
    }

    const std::string &LogStream::ProcessChannel() {
        return _processChannel;
    }

    bool LogStream::SetChannelSeverity(const std::string &channel, const std::string &lvl) {

        const auto level = ParseLevel(lvl);
        if (!level.has_value()) {
            log_warning << "Not a log level, channel: " << channel << ", level: " << lvl;
            return false;
        }

        std::unique_lock lock(_channelMutex);
        auto levels = std::make_shared<ChannelLevels>(*_channelLevels);
        (*levels)[channel] = *level;
        _channelLevels = std::move(levels);
        lock.unlock();

        log_info << "Log level set, channel: " << channel << ", level: " << lvl;
        return true;
    }

    std::optional<std::string> LogStream::CanonicalLevel(const std::string &lvl) {

        const auto level = ParseLevel(lvl);
        if (!level.has_value()) return std::nullopt;
        return level->has_value() ? to_string(**level) : "off";
    }

    void LogStream::ClearChannelSeverity(const std::string &channel) {

        std::unique_lock lock(_channelMutex);
        auto levels = std::make_shared<ChannelLevels>(*_channelLevels);
        levels->erase(channel);
        _channelLevels = std::move(levels);
    }

    std::map<std::string, std::string> LogStream::ChannelSeverities() {

        std::shared_ptr<const ChannelLevels> current;
        {
            std::shared_lock lock(_channelMutex);
            current = _channelLevels;
        }

        std::map<std::string, std::string> levels;
        for (const auto &[channel, severity]: *current) {
            levels[channel] = severity.has_value() ? to_string(*severity) : "off";
        }
        return levels;
    }

    void LogStream::ApplyConfiguration(const std::string &fallbackLevel) {

        const auto &configuration = Configuration::instance();
        SetSeverity(configuration.getOr<std::string>("euclid.logging.level", fallbackLevel.empty() ? _currentLevel : fallbackLevel));

        // Replaced rather than merged: a channel taken out of the configuration is meant to go
        // back to the process-wide level, and a reload that could only ever add entries would
        // leave yesterday's override in force with nothing in the file to explain it.
        //
        // Seeded with the output of the processes euclid runs, at info. That output used to
        // bypass the log core altogether and was therefore printed whatever the level was, so
        // leaving it to follow euclid.logging.level would silently hide an application's own
        // logging the first time somebody set the level to warning. It stays visible until
        // somebody says otherwise - which is now something they can say.
        ChannelLevels levels{
                {kApplicationChannel, boost::log::trivial::info},
                {kModuleChannel, boost::log::trivial::info},
        };
        if (constexpr auto channelPath = "euclid.logging.channels"; configuration.has(channelPath)) {
            for (const auto &name: configuration.getKeys(channelPath)) {
                const auto lvl = configuration.getOr<std::string>(std::string(channelPath) + "." + name, "");
                const auto level = ParseLevel(lvl);
                if (!level.has_value()) {
                    log_warning << "Not a log level, channel: " << name << ", level: " << lvl;
                    continue;
                }
                levels[name] = *level;
            }
        }

        std::unique_lock lock(_channelMutex);
        _channelLevels = std::make_shared<const ChannelLevels>(std::move(levels));
    }

    void LogStream::LogVerbatim(const std::string &channel, const boost::log::trivial::severity_level severity, const std::string &message) {
        BOOST_LOG_CHANNEL_SEV(my_logger::get(), channel, severity)
                << boost::log::add_value(kVerbatimAttribute, true) << message;
    }

    std::string LogStream::GetSeverity() {
        return _currentLevel;
    }

    void LogStream::SetSeverity(const std::string &lvl) {

        if (!from_string(lvl.c_str(), lvl.length(), _severity)) {
            log_warning << "Not a log level, keeping " << _currentLevel << ", level: " << lvl;
            return;
        }
        _currentLevel = lvl;
        _defaultSeverity.store(_severity, std::memory_order_relaxed);

        // The filter itself does not change - it reads the level every time - so this is all
        // setting a level involves, on a running process as much as at start-up.
        boost::log::core::get()->set_filter(&ChannelFilter);
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

        // No filter of its own: the core's channel-aware filter has already decided what gets
        // written, and a second one here could only ever discard more than the levels asked for.

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
}// namespace Euclid::Core