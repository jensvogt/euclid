// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// C++ includes
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

// Euclid includes
#include <euclid/core/ApplicationLaunch.h>
#include <euclid/core/Configuration.h>
#include <euclid/core/CryptoUtils.h>
#include <euclid/core/DateTimeUtils.h>
#include <euclid/core/HttpActionServer.h>
#include <euclid/core/JwtUtils.h>
#include <euclid/core/LogStream.h>

namespace Euclid::Core::Launch {

    std::string RuntimeKey(const std::string &runtime) {
        if (InterpreterPrefix(runtime).empty()) return {};
        std::string key = runtime;
        std::ranges::transform(key, key.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return key;
    }

    bool IsJob(const std::string &type) {
        return type == "JOB";
    }

    std::vector<std::string> InterpreterPrefix(const std::string &runtime) {
        if (runtime == "JAVA") return {"java", "-jar"};
        if (runtime == "JAVA21") return {"java21", "-jar"};
        if (runtime == "JAVA25") return {"java25", "-jar"};
        if (runtime == "PYTHON") return {"python3"};
        if (runtime == "NODEJS") return {"node"};
        return {};
    }

    namespace {

        // Whether this process could exec it, which on POSIX is a question about the effective user
        // and not about the mode bits alone - a JDK under a directory the service account cannot
        // traverse is as unusable as one that is not there.
        bool runnable(const std::filesystem::path &path) {
#ifdef _WIN32
            std::error_code ec;
            return std::filesystem::is_regular_file(path, ec);
#else
            return ::access(path.c_str(), X_OK) == 0;
#endif
        }

        // Where execvp() would look, for the interpreters named rather than located.
        bool onPath(const std::string &name) {

            const char *path = std::getenv("PATH");
            if (path == nullptr) return false;

#ifdef _WIN32
            constexpr char separator = ';';
#else
            constexpr char separator = ':';
#endif
            const std::string_view all{path};
            for (std::size_t at = 0; at <= all.size();) {

                const auto end = all.find(separator, at);
                const auto entry = all.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at);
                at = end == std::string_view::npos ? all.size() + 1 : end + 1;

                if (!entry.empty() && runnable(std::filesystem::path(entry) / name)) return true;
            }
            return false;
        }

    }// namespace

    std::vector<UnusableRuntime> UnusableRuntimes(const Interpreter &interpreter) {

        std::vector<UnusableRuntime> unusable;

        // The runtimes that have an interpreter at all. BINARY is its own command, so there is
        // nothing a host could be told about it and nothing to check.
        for (const auto *runtime: {"JAVA", "JAVA21", "JAVA25", "PYTHON", "NODEJS"}) {

            const auto prefix = InterpreterPrefix(runtime);
            if (prefix.empty()) continue;

            const auto key = RuntimeKey(runtime);
            const auto command = interpreter ? interpreter(key, prefix.front()) : prefix.front();

            if (command.empty()) {
                unusable.push_back({.runtime = key, .command = command, .reason = "is configured as nothing at all"});
                continue;
            }

            // Named rather than located, which is both the shipped python3 and node and the name a
            // runtime falls back to when nothing was configured for it. Either way the PATH decides.
            if (std::filesystem::path(command).filename() == std::filesystem::path(command)) {
                if (!onPath(command)) {
                    unusable.push_back({.runtime = key, .command = command, .reason = "is not on the PATH"});
                }
                continue;
            }

            std::error_code ec;
            if (!std::filesystem::exists(command, ec)) {
                unusable.push_back({.runtime = key, .command = command, .reason = "does not exist"});
            } else if (!runnable(command)) {
                unusable.push_back({.runtime = key, .command = command, .reason = "cannot be executed by this account"});
            }
        }

        return unusable;
    }

    std::vector<std::string> CommandLine(const std::string &runtime, const std::string &command, const std::string &artifact,
                                         const std::vector<std::string> &arguments, const Interpreter &interpreter) {

        std::vector<std::string> line;

        if (!command.empty()) {
            line = {command, artifact};
        } else if (auto prefix = InterpreterPrefix(runtime); !prefix.empty()) {
            if (interpreter) prefix.front() = interpreter(RuntimeKey(runtime), prefix.front());
            line = std::move(prefix);
            line.push_back(artifact);
        } else {
            line = {artifact};
        }

        line.insert(line.end(), arguments.begin(), arguments.end());
        return line;
    }

    bool ArtifactIsCurrent(const std::filesystem::path &path, const std::string &md5Sum, const long size) {

        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) return false;

        try {
            if (!md5Sum.empty()) return CryptoUtils::md5SumFile(path.string()) == md5Sum;
            if (size > 0) return static_cast<long>(std::filesystem::file_size(path, ec)) == size && !ec;
        } catch (const std::exception &e) {
            // Unreadable for whatever reason - fetching over it is the safe answer, and the fetch
            // reports its own failure.
            log_warning << "Could not check the artifact already on disk, path: " << path.string() << ", error: " << e.what();
        }
        return false;
    }

    void PrepareArtifact(const std::filesystem::path &path, const std::string &runtime, const std::string &command) {
#ifndef _WIN32
        if (!command.empty() || !InterpreterPrefix(runtime).empty()) return;
        std::error_code ec;
        std::filesystem::permissions(path, std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec,
                                     std::filesystem::perm_options::add, ec);
        if (ec) log_warning << "Could not make the artifact executable, path: " << path.string() << ", error: " << ec.message();
#else
        (void) path;
        (void) runtime;
        (void) command;
#endif
    }

    int PickHttpPort(const std::map<std::string, int> &held, const std::string &instanceId, const long first, const long last) {

        if (first <= 0 || last < first || last > 65535) return 0;
        if (const auto mine = held.find(instanceId); mine != held.end()) return mine->second;

        for (long port = first; port <= last; ++port) {
            const bool taken = std::ranges::any_of(held, [port](const auto &entry) { return entry.second == static_cast<int>(port); });
            if (!taken) return static_cast<int>(port);
        }
        return 0;
    }

    std::string OutputChannel(const std::string &runtimeName) {
        return std::string(LogStream::kApplicationChannel) + "." + runtimeName;
    }

    std::string OutputFields(const std::string &runtimeName, const std::string &nameSpace, const std::string &accountId) {
        boost::json::object fields{{"service.name", runtimeName}, {"application.id", runtimeName}};
        if (!nameSpace.empty()) fields["namespace"] = nameSpace;
        if (!accountId.empty()) fields["account.id"] = accountId;
        return boost::json::serialize(fields);
    }

    std::string SanitizeOutput(const std::string &raw) {
        std::string out;
        out.reserve(raw.size());
        for (const char ch: raw) {
            // Unsigned deliberately: the >= 0x20 test has to reject control bytes, not accept every
            // byte over 0x7f because it happened to be a negative char.
            const auto c = static_cast<unsigned char>(ch);
            if (c == '\t' || (c >= 0x20 && c != 0x7f)) {
                out += static_cast<char>(c);
            } else {
                char buf[5];
                std::snprintf(buf, sizeof(buf), "\\x%02x", c);
                out += buf;
            }
        }
        return out;
    }

    namespace {

        using Severity = boost::log::trivial::severity_level;

        // A level as Logback, Log4j, java.util.logging, Python's logging and Boost.Log spell it, in
        // capitals only - "Error" or "error" in the first words of a line is far more often part
        // of what it says than what it is.
        std::optional<Severity> levelWord(std::string_view word) {
            while (!word.empty() && word.front() == '[') word.remove_prefix(1);
            // "INFO:root:message" (Python) and "ERROR:" alike: the level is what comes before.
            if (const auto colon = word.find(':'); colon != std::string_view::npos) word = word.substr(0, colon);
            while (!word.empty() && word.back() == ']') word.remove_suffix(1);
            if (word == "TRACE" || word == "FINEST" || word == "FINER") return Severity::trace;
            if (word == "DEBUG" || word == "FINE") return Severity::debug;
            if (word == "INFO") return Severity::info;
            if (word == "WARN" || word == "WARNING") return Severity::warning;
            if (word == "ERROR" || word == "SEVERE") return Severity::error;
            if (word == "FATAL" || word == "CRITICAL") return Severity::fatal;
            return std::nullopt;
        }

        // The level a plain-text line says it has, if it says one where layouts put it: among the
        // first few words, after a date, a time, a thread or a pid. Spring Boot's default
        // ("2026-10-18T00:54:56.811+02:00  INFO 4711 --- [main] ..."), a Logback pattern
        // ("2026-10-18 00:54:56.811 [worker-0] DEBUG d.l.p.Client : ..."), Log4j's "[INFO ]".
        // Not further in: a level word deep in a message is the message talking about one.
        std::optional<Severity> textLevel(const std::string &line) {
            constexpr std::size_t kWords = 6;
            constexpr std::size_t kChars = 160;
            const std::string_view head(line.data(), std::min(line.size(), kChars));
            std::size_t words = 0;
            std::size_t i = 0;
            while (i < head.size() && words < kWords) {
                while (i < head.size() && (head[i] == ' ' || head[i] == '\t')) ++i;
                const auto start = i;
                while (i < head.size() && head[i] != ' ' && head[i] != '\t') ++i;
                if (i == start) break;
                ++words;
                if (const auto level = levelWord(head.substr(start, i - start))) return level;
            }
            return std::nullopt;
        }

        // A line that carries on the record before it rather than starting one: a stack frame, a
        // cause, anything indented. It belongs at the level of the line it continues - an ERROR's
        // trace recorded as info would be split across two levels, and a channel at "error" would
        // show the error without the trace that explains it.
        // An application's timestamp in euclid's form: UTC, milliseconds, "Z". Empty if it does not
        // parse. Logback writes local time with an offset and nanoseconds
        // ("2026-10-18T01:26:51.899898343+02:00"), Python "2026-10-18 01:26:51,899" - and the
        // collector's parser takes one format.
        std::string normalizedTimestamp(const std::string &value) {

            // yyyy-mm-ddThh:mm:ss, an optional fraction, then Z, +hh:mm, +hhmm or nothing (UTC).
            if (value.size() < 19 || value[4] != '-' || value[7] != '-' || (value[10] != 'T' && value[10] != ' ') ||
                value[13] != ':' || value[16] != ':') {
                return {};
            }
            const auto number = [&](const std::size_t at, const std::size_t length, int &out) {
                out = 0;
                for (std::size_t i = at; i < at + length; ++i) {
                    if (value[i] < '0' || value[i] > '9') return false;
                    out = out * 10 + (value[i] - '0');
                }
                return true;
            };
            int year, month, day, hour, minute, second;
            if (!number(0, 4, year) || !number(5, 2, month) || !number(8, 2, day) ||
                !number(11, 2, hour) || !number(14, 2, minute) || !number(17, 2, second)) {
                return {};
            }

            std::size_t i = 19;
            int millis = 0;
            if (i < value.size() && (value[i] == '.' || value[i] == ',')) {
                ++i;
                int digits = 0;
                while (i < value.size() && value[i] >= '0' && value[i] <= '9') {
                    if (digits < 3) millis = millis * 10 + (value[i] - '0');
                    ++digits;
                    ++i;
                }
                if (digits == 0) return {};
                for (; digits < 3; ++digits) millis *= 10;
            }

            int offsetMinutes = 0;
            if (i < value.size()) {
                if (value[i] == 'Z' || value[i] == 'z') {
                    ++i;
                } else if (value[i] == '+' || value[i] == '-') {
                    const int sign = value[i] == '-' ? -1 : 1;
                    int oh, om;
                    if (i + 3 > value.size() || !number(i + 1, 2, oh)) return {};
                    std::size_t at = i + 3;
                    if (at < value.size() && value[at] == ':') ++at;
                    if (at + 2 > value.size() || !number(at, 2, om)) return {};
                    offsetMinutes = sign * (oh * 60 + om);
                    i = at + 2;
                }
            }
            if (i != value.size()) return {};

            using namespace std::chrono;
            const year_month_day date{std::chrono::year{year}, std::chrono::month{static_cast<unsigned>(month)},
                                      std::chrono::day{static_cast<unsigned>(day)}};
            if (!date.ok() || hour > 23 || minute > 59 || second > 60) return {};
            const auto utc = sys_days{date} + hours{hour} + minutes{minute} + seconds{second} + milliseconds{millis} -
                             minutes{offsetMinutes};

            const auto utcDays = floor<days>(utc);
            const year_month_day utcDate{utcDays};
            const hh_mm_ss time{floor<milliseconds>(utc - utcDays)};
            char out[32];
            std::snprintf(out, sizeof(out), "%04d-%02u-%02uT%02d:%02d:%02d.%03dZ",
                          static_cast<int>(utcDate.year()), static_cast<unsigned>(utcDate.month()), static_cast<unsigned>(utcDate.day()),
                          static_cast<int>(time.hours().count()), static_cast<int>(time.minutes().count()),
                          static_cast<int>(time.seconds().count()), static_cast<int>(time.subseconds().count()));
            return out;
        }

        // A JSON record's fields one level deep, nested objects spelled with dots - Spring Boot's ECS
        // format nests some ({"log":{"logger":...}}), Logstash's encoder writes them flat, and from
        // here on both look the same.
        void flatten(const boost::json::object &object, const std::string &prefix, std::vector<std::pair<std::string, boost::json::value> > &out) {
            for (const auto &field: object) {
                auto key = prefix.empty() ? std::string(field.key()) : prefix + "." + std::string(field.key());
                if (field.value().is_object()) {
                    flatten(field.value().as_object(), key, out);
                } else {
                    out.emplace_back(std::move(key), field.value());
                }
            }
        }

        bool oneOf(const std::string &key, const std::initializer_list<std::string_view> names) {
            return std::ranges::find(names, std::string_view(key)) != names.end();
        }

        bool continuesPrevious(const std::string &line) {
            if (line.empty()) return false;
            if (line.front() == ' ' || line.front() == '\t') return true;
            return line.starts_with("Caused by:") || line.starts_with("Suppressed:");
        }

    }// namespace

    boost::log::trivial::severity_level OutputSeverity(const std::string &line, const bool fromStderr) {
        return OutputSeverity(line, fromStderr, std::nullopt);
    }

    std::optional<ApplicationRecord> ParseApplicationRecord(const std::string &line) {

        // Cheap enough to be worth doing before parsing: almost every line from a program that does
        // not log JSON fails this, and parsing each of those would be the cost of the feature for
        // everybody who does not use it.
        if (line.size() < 2 || line.front() != '{') return std::nullopt;
        boost::system::error_code ec;
        const auto parsed = boost::json::parse(line, ec);
        if (ec || !parsed.is_object()) return std::nullopt;

        std::vector<std::pair<std::string, boost::json::value> > fields;
        flatten(parsed.as_object(), "", fields);

        ApplicationRecord record;
        std::string callerClass;
        std::string callerMethod;
        bool haveMessage = false;

        for (auto &[key, value]: fields) {
            const auto *text = value.if_string();

            // What a record is: its message, its level, its moment. Taken from wherever the format
            // at hand keeps them, and not stored again as a field of their own.
            if (oneOf(key, {"message", "msg"}) && text != nullptr && !haveMessage) {
                record.message = std::string(*text);
                haveMessage = true;
                continue;
            }
            if (oneOf(key, {"level", "log.level", "levelname", "severity"}) && text != nullptr) {
                if (!record.severity) {
                    auto name = std::string(*text);
                    std::ranges::transform(name, name.begin(), [](const unsigned char c) { return static_cast<char>(std::toupper(c)); });
                    record.severity = levelWord(name);
                }
                continue;
            }
            if (oneOf(key, {"@timestamp", "timestamp", "time", "asctime"}) && text != nullptr && record.timestamp.empty()) {
                if (auto normalized = normalizedTimestamp(std::string(*text)); !normalized.empty()) {
                    record.timestamp = std::move(normalized);
                    continue;
                }
            }

            // Bookkeeping of the encoder's own, which says nothing about the record.
            if (oneOf(key, {"@version", "level_value", "ecs.version"})) continue;

            // The rest of what Logback, Spring Boot and Python's JSON formatters write, under the
            // ECS names euclid's own records use - so "which logger" is one field whoever logged.
            if (oneOf(key, {"logger_name", "logger", "log.logger"})) { record.fields["log.logger"] = value; continue; }
            if (oneOf(key, {"thread_name", "thread", "threadName", "process.thread.name"})) { record.fields["process.thread.name"] = value; continue; }
            if (oneOf(key, {"stack_trace", "exception", "exc_info", "error.stack_trace"})) { record.fields["error.stack_trace"] = value; continue; }
            if (key == "caller_class_name" && text != nullptr) { callerClass = std::string(*text); continue; }
            if (key == "caller_method_name" && text != nullptr) { callerMethod = std::string(*text); continue; }
            if (key == "caller_file_name") { record.fields["log.origin.file.name"] = value; continue; }
            if (key == "caller_line_number") { record.fields["log.origin.file.line"] = value; continue; }

            // Fields already in ECS's vocabulary, from an application logging ECS itself. Kept where
            // they are: they mean what euclid's would.
            if (key.starts_with("log.") || key.starts_with("process.thread.") || key.starts_with("error.") ||
                key.starts_with("event.") || key.starts_with("trace.") || key.starts_with("span.") ||
                key.starts_with("transaction.") || key.starts_with("service.") || key.starts_with("labels.") ||
                key == "tags") {
                record.fields[key] = value;
                continue;
            }

            // Anything else is the application's own: under "labels.", where ECS keeps custom values.
            // Never at the top level, where a name of its choosing ("service" next to euclid's
            // "service.name") can make OpenSearch refuse the whole document.
            auto label = key;
            std::ranges::replace(label, '.', '_');
            record.fields["labels." + label] = value;
        }

        if (!callerMethod.empty()) {
            record.fields["log.origin.function"] = callerClass.empty() ? callerMethod : callerClass + "." + callerMethod;
        }
        // A JSON line without a message is still a line somebody wanted written.
        if (!haveMessage) record.message = line;
        return record;
    }

    boost::log::trivial::severity_level OutputSeverity(const std::string &line, const bool fromStderr,
                                                       const std::optional<boost::log::trivial::severity_level> previous) {

        const auto fallback = fromStderr ? boost::log::trivial::error : boost::log::trivial::info;

        if (const auto record = ParseApplicationRecord(line)) return record->severity.value_or(fallback);
        if (const auto level = textLevel(line)) return *level;
        if (previous && continuesPrevious(line)) return *previous;
        return fallback;
    }

    void EmitOutput(const std::string &channel, const std::string &line, const bool fromStderr, const std::string &fields) {
        std::optional<boost::log::trivial::severity_level> previous;
        EmitOutput(channel, line, fromStderr, fields, previous);
    }

    void EmitOutput(const std::string &channel, const std::string &line, const bool fromStderr, const std::string &fields,
                    std::optional<boost::log::trivial::severity_level> &previous) {

        // Sanitised first, and everything after read from what the program actually wrote.
        const auto sanitized = SanitizeOutput(line);

        // A JSON line is translated into a record of euclid's own rather than passed on as text:
        // its message, its level and its moment become the record's, the rest becomes ECS fields.
        // Parsed once, here - the formatters then write what they are given.
        if (auto record = ParseApplicationRecord(sanitized)) {
            const auto severity = record->severity.value_or(fromStderr ? boost::log::trivial::error : boost::log::trivial::info);
            previous = severity;

            // euclid's own fields last, so an application cannot claim another application's id
            // or namespace by logging a field of the same name.
            auto merged = std::move(record->fields);
            if (!fields.empty()) {
                boost::system::error_code ec;
                if (const auto own = boost::json::parse(fields, ec); !ec && own.is_object()) {
                    for (const auto &field: own.as_object()) merged[field.key()] = field.value();
                }
            }
            LogStream::LogVerbatim(channel, severity, record->message, boost::json::serialize(merged), record->timestamp);
            return;
        }

        const auto severity = OutputSeverity(sanitized, fromStderr, previous);
        previous = severity;
        LogStream::LogVerbatim(channel, severity, sanitized, fields);
    }

    std::chrono::seconds CredentialsTtl() {
        constexpr long kDefaultTtlSeconds = 3600;
        return std::chrono::seconds(std::max(60L, Configuration::instance().getOr<long>("euclid.modules.eap.credentials-ttl-seconds", kDefaultTtlSeconds)));
    }

    std::string GatewayEndpoint() {
        const auto &configuration = Configuration::instance();
        const auto host = configuration.getOr<std::string>("euclid.gateway.http.host", "localhost");
        const auto port = configuration.getOr<long>("euclid.gateway.http.port", 5566);
        const auto scheme = configuration.getOr<bool>("euclid.gateway.tls.enabled", true) ? "https" : "http";
        return scheme + std::string("://") + host + ":" + std::to_string(port);
    }

    void AddHostEnvironment(std::map<std::string, std::string> &environment, const std::string &endpoint,
                            const std::string &caCertPath, const std::string &credentialsFile) {

        environment["EUCLID_ENDPOINT"] = endpoint;
        environment["EUCLID_BASE_URL"] = endpoint;

        // RFC 9421 rather than SigV4: an application is whatever language its author reached for,
        // and HTTP Message Signatures is the one of the two schemes with off-the-shelf libraries in
        // all of them.
        environment["EUCLID_SIGNATURE"] = "rfc9421";

        if (!caCertPath.empty()) environment["EUCLID_CA_CERT_PATH"] = caCertPath;
        environment["EUCLID_CREDENTIALS_FILE"] = credentialsFile;
    }

    boost::json::object IssueCredentials(const Principal &principal) {

        const auto ttl = CredentialsTtl();
        const auto expiresAt = std::chrono::system_clock::now() + ttl;

        return boost::json::object{
                {"token", JwtUtils::CreateToken(principal.userId, HttpActionServer::JwtSecret(), ttl)},
                {"expiresAt", DateTimeUtils::ToISO8601(expiresAt)},
                {"userId", principal.userId},
                {"accountId", principal.accountId},
                {"region", principal.region},
                {"namespace", principal.nameSpace},
                {"endpoint", GatewayEndpoint()},
        };
    }

    std::optional<std::chrono::system_clock::time_point> CredentialsExpiry(const boost::json::value &credentials) {
        try {
            const auto *expiresAt = credentials.is_object() ? credentials.as_object().if_contains("expiresAt") : nullptr;
            if (expiresAt == nullptr || !expiresAt->is_string() || expiresAt->as_string().empty()) return std::nullopt;
            return DateTimeUtils::FromISO8601(std::string(expiresAt->as_string()));
        } catch (const std::exception &) {
            return std::nullopt;
        }
    }

    std::optional<std::chrono::system_clock::time_point> ReadCredentialsExpiry(const std::filesystem::path &path) {

        std::ifstream in(path);
        if (!in) return std::nullopt;

        try {
            std::ostringstream buffer;
            buffer << in.rdbuf();
            return CredentialsExpiry(boost::json::parse(buffer.str()));
        } catch (const std::exception &) {
            return std::nullopt;
        }
    }

    std::chrono::system_clock::time_point RefreshAt(const std::chrono::system_clock::time_point expiresAt,
                                                    const std::chrono::system_clock::time_point issuedAt) {
        if (expiresAt <= issuedAt) return issuedAt;
        return issuedAt + (expiresAt - issuedAt) / 2;
    }

    bool WriteCredentials(const std::filesystem::path &path, const boost::json::value &credentials) {

        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);

        const auto temporary = std::filesystem::path(path.string() + ".new");
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            if (!out) {
                log_error << "Could not write credentials, path: " << temporary.string();
                return false;
            }
            out << boost::json::serialize(credentials);
            if (!out) {
                log_error << "Could not write credentials, path: " << temporary.string();
                return false;
            }
        }
        std::filesystem::permissions(temporary, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::replace, ec);
        std::filesystem::rename(temporary, path, ec);
        if (ec) {
            log_error << "Could not move credentials into place, path: " << path.string() << ", error: " << ec.message();
            return false;
        }
        return true;
    }

}// namespace Euclid::Core::Launch
