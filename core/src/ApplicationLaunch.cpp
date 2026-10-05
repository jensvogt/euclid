// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// C++ includes
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>

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

    std::vector<std::string> InterpreterPrefix(const std::string &runtime) {
        if (runtime == "JAVA") return {"java", "-jar"};
        if (runtime == "JAVA21") return {"java21", "-jar"};
        if (runtime == "JAVA25") return {"java25", "-jar"};
        if (runtime == "PYTHON") return {"python3"};
        if (runtime == "NODEJS") return {"node"};
        return {};
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

    boost::log::trivial::severity_level OutputSeverity(const std::string &line, const bool fromStderr) {

        const auto fallback = fromStderr ? boost::log::trivial::error : boost::log::trivial::info;

        // Cheap enough to be worth doing before parsing: almost every line from a program that does
        // not log JSON fails this, and parsing each of those would be the cost of the feature for
        // everybody who does not use it.
        if (line.size() < 2 || line.front() != '{') return fallback;

        boost::system::error_code ec;
        const auto parsed = boost::json::parse(line, ec);
        if (ec || !parsed.is_object()) return fallback;

        const auto *level = parsed.as_object().if_contains("level");
        if (level == nullptr || !level->is_string()) return fallback;

        auto name = std::string(level->as_string());
        std::ranges::transform(name, name.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });

        // Logback's names, and the two SLF4J spells differently from Boost.Log.
        if (name == "trace") return boost::log::trivial::trace;
        if (name == "debug") return boost::log::trivial::debug;
        if (name == "info") return boost::log::trivial::info;
        if (name == "warn" || name == "warning") return boost::log::trivial::warning;
        if (name == "error" || name == "severe") return boost::log::trivial::error;
        if (name == "fatal") return boost::log::trivial::fatal;
        return fallback;
    }

    void EmitOutput(const std::string &channel, const std::string &line, const bool fromStderr, const std::string &fields) {
        // Sanitised first, and the severity read from what the program actually wrote.
        const auto sanitized = SanitizeOutput(line);
        LogStream::LogVerbatim(channel, OutputSeverity(sanitized, fromStderr), sanitized, fields);
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
