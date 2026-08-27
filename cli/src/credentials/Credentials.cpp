#include <euclid/cli/credentials/Credentials.h>

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/core/SystemUtils.h>

namespace Euclid::CLI {

    std::string Credentials::FilePath() {
        return Core::SystemUtils::GetHomeDir() + "/.euclid/credentials";
    }

    void Credentials::Save(const Entry &entry) {
        const std::filesystem::path path(FilePath());

        std::filesystem::create_directories(path.parent_path());
        std::filesystem::permissions(path.parent_path(), std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);

        const boost::json::object body{
                {"token", entry.token},
                {"userId", entry.userId},
                {"accountId", entry.accountId},
                {"region", entry.region},
                {"accessKeyId", entry.accessKeyId},
                {"secretAccessKey", entry.secretAccessKey},
                {"isAdmin", entry.isAdmin},
                {"namespace", entry.nameSpace},
        };

        std::ofstream out(path, std::ios::trunc);
        if (!out) {
            throw std::runtime_error("failed to open " + path.string() + " for writing");
        }
        out << boost::json::serialize(body);
        out.close();

        std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, std::filesystem::perm_options::replace);
    }

    std::optional<Credentials::Entry> Credentials::Load() {

        std::ifstream in(FilePath());
        if (!in) return std::nullopt;

        std::ostringstream buffer;
        buffer << in.rdbuf();

        boost::system::error_code ec;
        const auto jv = boost::json::parse(buffer.str(), ec);
        if (ec || !jv.is_object() || !Core::AttributeExists(jv, "token")) return std::nullopt;

        Entry entry;
        entry.token = Core::GetStringValue(jv, "token");
        entry.userId = Core::GetStringValue(jv, "userId");
        entry.accountId = Core::GetStringValue(jv, "accountId");
        entry.region = Core::GetStringValue(jv, "region");
        entry.accessKeyId = Core::GetStringValue(jv, "accessKeyId");
        entry.secretAccessKey = Core::GetStringValue(jv, "secretAccessKey");
        entry.isAdmin = Core::GetBoolValue(jv, "isAdmin");
        entry.nameSpace = Core::GetStringValue(jv, "namespace");
        return entry;
    }

    std::optional<std::string> Credentials::LoadToken() {
        const auto entry = Load();
        if (!entry.has_value()) return std::nullopt;
        return entry->token;
    }

    void Credentials::ClearToken() {
        std::error_code ec;
        std::filesystem::remove(FilePath(), ec);
    }

}
