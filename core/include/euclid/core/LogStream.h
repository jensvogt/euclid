//
// Created by vogje01 on 20/06/2023.
//

#pragma once

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define BOOST_ASIO_NO_WIN32_LEAN_AND_MEAN
#include <boost/asio.hpp>
#include <windows.h>
#ifdef _DEBUG
#include <crtdbg.h>
#endif
#endif

// C++ standard includes
#include <atomic>
#include <filesystem>
#include <iostream>
#include <istream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

// Boost includes
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/websocket/stream.hpp>
#include <boost/log/attributes.hpp>
#include <boost/log/attributes/scoped_attribute.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/sources/channel_feature.hpp>
#include <boost/log/sources/channel_logger.hpp>
#include <boost/log/sources/global_logger_storage.hpp>
#include <boost/log/sources/logger.hpp>
#include <boost/log/sources/severity_channel_logger.hpp>
#include <boost/log/sources/record_ostream.hpp>
#include <boost/log/support/date_time.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/utility/manipulators/add_value.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/log/utility/setup/console.hpp>
#include <boost/log/utility/setup/file.hpp>

// Euclid includes
//#include <awsmock/core/logging/LogWebsocketSink.h>

#define DEFAULT_LOG_SIZE (10 * 1024 * 1024)
#define DEFAULT_LOG_COUNT 5

// Named rather than written into the macro below, which would otherwise read the comma in the
// template argument list as a second macro argument.
//
// Multithreaded: every module serves its socket from a pool of worker threads, and a
// severity_logger (no _mt) shared between them has no synchronization of its own.
using euclid_logger_t = boost::log::sources::severity_channel_logger_mt<boost::log::trivial::severity_level, std::string>;

BOOST_LOG_INLINE_GLOBAL_LOGGER_DEFAULT(my_logger, euclid_logger_t)

BOOST_LOG_ATTRIBUTE_KEYWORD(process_id, "ProcessID", boost::log::attributes::current_process_id::value_type)
BOOST_LOG_ATTRIBUTE_KEYWORD(thread_id, "ThreadID", boost::log::attributes::current_thread_id::value_type)
BOOST_LOG_ATTRIBUTE_KEYWORD(timestamp, "TimeStamp", boost::posix_time::ptime)
BOOST_LOG_ATTRIBUTE_KEYWORD(line, "Line", int)
BOOST_LOG_ATTRIBUTE_KEYWORD(file, "File", std::string)
BOOST_LOG_ATTRIBUTE_KEYWORD(function, "Function", boost::log::attributes::function<std::string>)
BOOST_LOG_ATTRIBUTE_KEYWORD(channel, "Channel", std::string)

namespace Euclid::Core {

    /**
     * @brief Logging stream
     *
     * @par
     * Based on Boost::Log.
     *
     * @par
     * Every record carries a channel: the name of what is doing the logging, rather than the name
     * of what it is about. A process logs on its own channel ("esm", "eag"), and output the
     * manager reads back from a process it spawned is logged on that process's channel
     * ("app.parser", "module.esm"). Each channel can then be given its own level, or turned off
     * entirely, which is the only way to keep one talkative application from drowning out
     * everything euclid itself has to say - see SetChannelSeverity().
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class LogStream {

    public:
        /**
         * @brief The channel a record carries when nothing else is said: the process's own.
         */
        static constexpr auto kDefaultChannel = "euclid";

        /**
         * @brief Channel prefix for the output of an application the manager runs.
         *
         * @par
         * The full channel is "app.<applicationId>", so one application can be silenced by name
         * and all of them by the prefix - see SeverityFor().
         */
        static constexpr auto kApplicationChannel = "app";

        /**
         * @brief Channel prefix for the output of a euclid module the manager runs, as
         * distinct from what that module logs itself over its own channel.
         */
        static constexpr auto kModuleChannel = "module";

        /**
         * @brief Constructor
         */
        LogStream() = default;

        /**
         * @brief Initialization
         */
        static void Initialize();

        /**
         * @brief Returns the current log level
         *
         * @return current log level
         */
        static std::string GetSeverity();

        /**
         * @brief Set the maximum severity
         *
         * @param lvl PLog severity string
         */
        static void SetSeverity(const std::string &lvl);

        /**
         * @brief The channel this process's own records carry.
         *
         * @par
         * Set once at start-up to the module's name, so that a log aggregated from several
         * processes still says which one each line came from, and so a module can be turned down
         * without turning down what it is running.
         *
         * @param channel channel name, e.g. "esm".
         */
        static void SetProcessChannel(const std::string &channel);

        /**
         * @brief The channel this process's own records carry.
         */
        [[nodiscard]]
        static const std::string &ProcessChannel();

        /**
         * @brief Gives one channel its own level, overriding the process-wide one.
         *
         * @param channel channel name, e.g. "app.parser" for one application, or "app" for every
         * application at once.
         * @param lvl "trace", "debug", "info", "warning", "error", "fatal", or "off" to silence
         * the channel entirely. An unparsable level leaves the channel alone and is reported.
         * @return true if the level was understood and applied.
         */
        static bool SetChannelSeverity(const std::string &channel, const std::string &lvl);

        /**
         * @brief Removes a channel's own level, putting it back under the process-wide one.
         *
         * @param channel channel name.
         */
        static void ClearChannelSeverity(const std::string &channel);

        /**
         * @brief Reads a level name and gives back its canonical spelling.
         *
         * @par
         * For whatever stores a level rather than applying it - an application's own level is kept
         * in its database row - so that what is stored is refused at the point somebody typed it,
         * and so that "none" and "off" do not end up looking like two different settings to
         * whatever later compares them.
         *
         * @param lvl a level name, or one of "off"/"none"/"silent".
         * @return the canonical name ("trace" ... "fatal", or "off"), or nothing if it is not a
         * level at all.
         */
        [[nodiscard]]
        static std::optional<std::string> CanonicalLevel(const std::string &lvl);

        /**
         * @brief The levels currently in force, by channel, for whatever wants to report them.
         */
        [[nodiscard]]
        static std::map<std::string, std::string> ChannelSeverities();

        /**
         * @brief The level a channel logs at, and how that is decided.
         *
         * @par
         * A channel's own level if it has one; otherwise the level of the nearest enclosing
         * channel, cut back a dot at a time - so "app" covers "app.parser" without naming it -
         * and the process-wide level if none of them says anything. Nothing means the channel is
         * off and nothing it logs is recorded at all.
         *
         * @param channel channel name.
         * @return the level at and above which this channel is recorded, or nothing if it is off.
         */
        [[nodiscard]]
        static std::optional<boost::log::trivial::severity_level> SeverityFor(const std::string &channel);

        /**
         * @brief Reads euclid.logging.level and euclid.logging.channels and applies both.
         *
         * @par
         * Called at start-up and again whenever the configuration is re-read, which is what makes
         * a level changeable on a running installation: edit the file, signal the process, and the
         * next record is filtered by the new levels. Nothing else about the process is disturbed.
         *
         * @param fallbackLevel level to use when the configuration names none - normally what the
         * process was given on its command line. Empty keeps the level already in force, which is
         * what a reload wants.
         */
        static void ApplyConfiguration(const std::string &fallbackLevel = {});

        /**
         * @brief Logs one line exactly as it was given, on a channel of its own.
         *
         * @par
         * For output read back from a process euclid started, which arrives already formatted by
         * whatever wrote it - a second timestamp and source location in front of an application's
         * own log line helps nobody. It still goes through the log core, so the channel's level
         * decides whether it is recorded, which is the whole point.
         *
         * @param channel channel to log on, e.g. "app.parser".
         * @param severity severity to record it at.
         * @param message the line, verbatim.
         */
        static void LogVerbatim(const std::string &channel, boost::log::trivial::severity_level severity, const std::string &message);

        /**
         * @brief Add a file logging sink
         *
         * @par
         * The filename is constructed from <logDirectory>/<logPrefix>_nn.log
         *
         * @param dir log directory
         * @param prefix log file name prefix
         * @param size log size
         * @param count log count
         */
        static void AddFile(const std::string &dir, const std::string &prefix, long size = DEFAULT_LOG_SIZE, int count = DEFAULT_LOG_COUNT);

        /**
         * @brief Adds a web socket sink
         *
         * @param ws websocket
         */
        //        static void AddWebSocket(boost::beast::websocket::stream<boost::beast::tcp_stream> &ws);

        /**
         * @brief Add a logging websocket
         *
         * @param ioc boost IOC
         * @param port websocket poit
         */
        //        static void AddLoggingWebSocket(boost::asio::io_context &ioc, unsigned int port);

        /**
         * @brief Remove web socket sink
         */
        static void RemoveWebSocketSink();

        /**
         * Removes the console log sink
         */
        static void RemoveConsoleLogs();

        /**
         * @brief Logs a raw message without timestamp, severity, or other decorators
         *
         * @param message message to write verbatim to stdout
         */
        static void LogRaw(const std::string &message);

    private:

        /**
         * @brief A channel's own level, or nothing when the channel is off.
         */
        using ChannelLevels = std::map<std::string, std::optional<boost::log::trivial::severity_level> >;

        /**
         * @brief Levels by channel, replaced whole rather than edited in place.
         *
         * @par
         * The filter reads this for every record any thread offers, and a level changes perhaps
         * twice in a process's life. So a reader takes a copy of the pointer and is then free of
         * everything a writer does, and a writer builds a new table and swaps it in - no lock on
         * the path that runs millions of times, one on the path that runs twice.
         */
        static std::atomic<std::shared_ptr<const ChannelLevels> > _channelLevels;

        /**
         * @brief Serializes the writers, which have to read the current table before replacing it.
         */
        static std::mutex _channelMutex;

        /**
         * @brief The level for a channel that has none of its own.
         */
        static std::atomic<boost::log::trivial::severity_level> _defaultSeverity;

        /**
         * @brief The channel this process's own records carry.
         */
        static std::string _processChannel;

        /**
         * Log size
         */
        static long _logSize;

        /**
         * Log count
         */
        static int _logCount;

        /**
         * Log directory
         */
        static std::string _logDir;

        /**
         * Log filename prefix
         */
        static std::string _logPrefix;

        /**
         * Current log level
         */
        static std::string _currentLevel;

        /**
         * Severity
         */
        static boost::log::trivial::severity_level _severity;

        /**
         * Console appender
         */
        static boost::shared_ptr<boost::log::sinks::synchronous_sink<boost::log::sinks::text_ostream_backend> > _consoleSink;

        /**
         * File appender
         */
        static boost::shared_ptr<boost::log::sinks::synchronous_sink<boost::log::sinks::text_file_backend> > _fileSink;

        /**
         * Web socket
         */
        static boost::shared_ptr<boost::beast::websocket::stream<boost::beast::tcp_stream> > _ws;

        /**
         * Web socket backend
         */
        //        static boost::shared_ptr<LogWebsocketSink> webSocketBackend;

        /**
         * Web socket sink
         */
        //        static boost::shared_ptr<webSocketSink_t> webSocketSink;
    };

} // namespace Euclid::Core

#if defined(_WIN32) || defined(CYGWIN)
#define EUCLID_LOG_FUNCTION __FUNCTION__
#else
#define EUCLID_LOG_FUNCTION __PRETTY_FUNCTION__
#endif

// One record, on a named channel. Written once and used for both macro families below, so the
// channel-carrying form cannot drift from the plain one about what a record contains.
#define log_channel(chan, sev)                                    \
    BOOST_LOG_CHANNEL_SEV(my_logger::get(), chan, sev)            \
            << boost::log::add_value("Line", __LINE__)            \
            << boost::log::add_value("File", __FILE__)            \
            << boost::log::add_value("Function", EUCLID_LOG_FUNCTION)

// The ordinary macros, unchanged in use: they log on the process's own channel, which is its
// module name once SetProcessChannel() has been called and "euclid" until then.
#define log_fatal   log_channel(Euclid::Core::LogStream::ProcessChannel(), boost::log::trivial::fatal)
#define log_error   log_channel(Euclid::Core::LogStream::ProcessChannel(), boost::log::trivial::error)
#define log_warning log_channel(Euclid::Core::LogStream::ProcessChannel(), boost::log::trivial::warning)
#define log_info    log_channel(Euclid::Core::LogStream::ProcessChannel(), boost::log::trivial::info)
#define log_debug   log_channel(Euclid::Core::LogStream::ProcessChannel(), boost::log::trivial::debug)
#define log_trace   log_channel(Euclid::Core::LogStream::ProcessChannel(), boost::log::trivial::trace)

// The same, on a channel the caller names - for what a process logs on behalf of something else.
#define log_fatal_ch(chan)   log_channel(chan, boost::log::trivial::fatal)
#define log_error_ch(chan)   log_channel(chan, boost::log::trivial::error)
#define log_warning_ch(chan) log_channel(chan, boost::log::trivial::warning)
#define log_info_ch(chan)    log_channel(chan, boost::log::trivial::info)
#define log_debug_ch(chan)   log_channel(chan, boost::log::trivial::debug)
#define log_trace_ch(chan)   log_channel(chan, boost::log::trivial::trace)

#define log_raw(msg) Euclid::Core::LogStream::LogRaw(msg)