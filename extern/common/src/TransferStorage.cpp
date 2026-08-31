#include <TransferStorage.h>
#include <TransferContext.h>

// C++ includes
#include <fstream>
#include <set>
#include <sstream>

// Boost includes
#include <boost/json.hpp>

// Euclid includes
#include <euclid/core/LogStream.h>

namespace Euclid::Transfer {

    namespace {

        // ESM streams anything above this inline; a transfer server always wants the bytes in
        // one response, so the limit is set past any object it is expected to serve.
        constexpr long kMaxInlineSize = 512L * 1024 * 1024;

        // list-objects is paged; one page this size covers any directory a transfer client is
        // reasonably going to browse.
        constexpr long kListPageSize = 10000;

        // Normalizes a directory key to the "ends in exactly one slash, no leading slash" form
        // every prefix comparison below assumes. The bucket root normalizes to "".
        std::string asPrefix(const std::string &directory) {
            std::string prefix = directory;
            while (!prefix.empty() && prefix.front() == '/') prefix.erase(prefix.begin());
            while (!prefix.empty() && prefix.back() == '/') prefix.pop_back();
            return prefix.empty() ? "" : prefix + "/";
        }

        std::string stripLeadingSlash(const std::string &key) {
            std::string result = key;
            while (!result.empty() && result.front() == '/') result.erase(result.begin());
            return result;
        }

        bool readFile(const std::filesystem::path &path, std::string &out) {
            std::ifstream in(path, std::ios::binary);
            if (!in) return false;
            std::ostringstream buffer;
            buffer << in.rdbuf();
            out = buffer.str();
            return true;
        }

    }// namespace

    std::vector<std::pair<std::string, std::string> > TransferStorage::scopedHeaders(std::vector<std::pair<std::string, std::string> > headers) const {

        // Sent only when known: an empty header value reads to the module exactly like an absent
        // one, so there is nothing to gain from adding it.
        if (!_region.empty()) headers.emplace_back("x-euclid-region", _region);
        if (!_accountId.empty()) headers.emplace_back("x-euclid-account-id", _accountId);
        return headers;
    }

    std::vector<TransferEntry> TransferStorage::listRaw(const std::string &prefix, std::vector<std::string> &keys) const {

        // Directories are asked for explicitly: ESM leaves them out of a listing by default, so
        // that a bucket reports the files it holds, but this is the one caller that has to see
        // them - an empty directory exists as nothing else.
        const auto body = boost::json::serialize(boost::json::object{
                {"bucketErn", _bucketErn},
                {"prefix", prefix},
                {"pageSize", kListPageSize},
                {"pageIndex", 0},
                {"sortColumn", "key"},
                {"sortDirection", "asc"},
                {"includeDirectories", true}});

        const auto response = CallModule("esm", "list-objects", _token, scopedHeaders({}), body);
        if (!response.ok()) {
            log_warning << "Transfer storage list failed, prefix: " << prefix << ", status: " << response.status;
            return {};
        }

        std::vector<TransferEntry> entries;
        try {
            const auto parsed = boost::json::parse(response.body);
            if (!parsed.is_object()) return {};

            const auto *objects = parsed.as_object().if_contains("objects");
            if (objects == nullptr || !objects->is_array()) return {};

            for (const auto &value: objects->as_array()) {
                if (!value.is_object()) continue;
                const auto &object = value.as_object();

                TransferEntry entry;
                if (const auto *v = object.if_contains("key"); v && v->is_string()) entry.name = v->as_string().c_str();
                if (const auto *v = object.if_contains("size"); v && v->is_number()) entry.size = v->to_number<long>();
                if (entry.name.empty()) continue;

                keys.push_back(entry.name);
                entries.push_back(entry);
            }
        } catch (const std::exception &e) {
            log_warning << "Transfer storage could not parse object list, error: " << e.what();
            return {};
        }

        return entries;
    }

    std::vector<TransferEntry> TransferStorage::List(const std::string &directory) const {

        const auto prefix = asPrefix(directory);

        std::vector<std::string> keys;
        const auto objects = listRaw(prefix, keys);

        // The listing is over every key beneath this directory, at any depth, so the immediate
        // children have to be cut back out of it: a key with no further slash is a file here,
        // and anything deeper contributes only the name of the subdirectory it lies under.
        std::vector<TransferEntry> entries;
        std::set<std::string> directories;

        for (const auto &object: objects) {
            const auto &key = object.name;
            if (!key.starts_with(prefix)) continue;

            const auto relative = key.substr(prefix.size());
            if (relative.empty()) continue;

            if (const auto slash = relative.find('/'); slash != std::string::npos) {
                directories.insert(relative.substr(0, slash));
                continue;
            }

            TransferEntry entry = object;
            entry.name = relative;
            entry.isDirectory = false;
            entries.push_back(entry);
        }

        for (const auto &name: directories) {
            entries.push_back({.name = name, .isDirectory = true, .size = 0});
        }

        return entries;
    }

    std::optional<TransferEntry> TransferStorage::Stat(const std::string &key) const {

        const auto cleanKey = stripLeadingSlash(key);
        if (cleanKey.empty()) {
            // The bucket root always exists and is always a directory.
            return TransferEntry{.name = "/", .isDirectory = true, .size = 0};
        }

        std::vector<std::string> keys;
        const auto objects = listRaw(cleanKey, keys);

        for (const auto &object: objects) {
            if (object.name == cleanKey) {
                TransferEntry entry = object;
                entry.name = cleanKey;
                entry.isDirectory = false;
                return entry;
            }
        }

        // Not a file. It is a directory if anything at all lives beneath it, or if its marker
        // object exists.
        const auto prefix = cleanKey + "/";
        for (const auto &object: objects) {
            if (object.name.starts_with(prefix)) {
                return TransferEntry{.name = cleanKey, .isDirectory = true, .size = 0};
            }
        }

        return std::nullopt;
    }

    bool TransferStorage::Download(const std::string &key, const std::filesystem::path &spoolPath) const {

        const auto response = CallModule("esm", "get-object", _token,
                                         scopedHeaders({{"x-euclid-bucket-ern", _bucketErn},
                                                        {"x-euclid-key", stripLeadingSlash(key)},
                                                        {"x-euclid-part-size", std::to_string(kMaxInlineSize)}}),
                                         "");
        if (!response.ok()) {
            log_warning << "Transfer storage download failed, key: " << key << ", status: " << response.status;
            return false;
        }

        std::ofstream out(spoolPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            log_error << "Transfer storage could not write spool file: " << spoolPath.string();
            return false;
        }
        out.write(response.body.data(), static_cast<std::streamsize>(response.body.size()));
        return out.good();
    }

    bool TransferStorage::Upload(const std::string &key, const std::filesystem::path &spoolPath) const {

        std::string data;
        if (!readFile(spoolPath, data)) {
            log_error << "Transfer storage could not read spool file: " << spoolPath.string();
            return false;
        }

        const auto response = CallModule("esm", "put-object", _token,
                                         scopedHeaders({{"x-euclid-bucket-ern", _bucketErn},
                                                        {"x-euclid-key", stripLeadingSlash(key)}}),
                                         data);
        if (!response.ok()) {
            log_warning << "Transfer storage upload failed, key: " << key << ", status: " << response.status;
            return false;
        }

        log_info << "Transfer storage stored object, key: " << key << ", size: " << data.size();
        return true;
    }

    std::optional<std::string> TransferStorage::ernOf(const std::string &key) const {

        const auto cleanKey = stripLeadingSlash(key);
        // Directories included: RemoveDirectory() resolves a marker's own ERN through here.
        const auto body = boost::json::serialize(boost::json::object{
                {"bucketErn", _bucketErn},
                {"prefix", cleanKey},
                {"pageSize", kListPageSize},
                {"pageIndex", 0},
                {"sortColumn", "key"},
                {"sortDirection", "asc"},
                {"includeDirectories", true}});

        const auto response = CallModule("esm", "list-objects", _token, scopedHeaders({}), body);
        if (!response.ok()) return std::nullopt;

        try {
            const auto parsed = boost::json::parse(response.body);
            const auto *objects = parsed.is_object() ? parsed.as_object().if_contains("objects") : nullptr;
            if (objects == nullptr || !objects->is_array()) return std::nullopt;

            for (const auto &value: objects->as_array()) {
                if (!value.is_object()) continue;
                const auto &object = value.as_object();
                const auto *objectKey = object.if_contains("key");
                const auto *objectErn = object.if_contains("ern");
                if (objectKey == nullptr || !objectKey->is_string() || objectErn == nullptr || !objectErn->is_string()) continue;
                if (std::string(objectKey->as_string().c_str()) == cleanKey) {
                    return std::string(objectErn->as_string().c_str());
                }
            }
        } catch (const std::exception &e) {
            log_warning << "Transfer storage could not resolve object ERN, key: " << key << ", error: " << e.what();
        }
        return std::nullopt;
    }

    bool TransferStorage::Remove(const std::string &key) const {

        // delete-object is addressed by object ERN rather than by key, so the key has to be
        // resolved first.
        const auto ern = ernOf(key);
        if (!ern.has_value()) {
            log_warning << "Transfer storage remove failed, no object at key: " << key;
            return false;
        }

        const auto response = CallModule("esm", "delete-object", _token, scopedHeaders({}),
                                         boost::json::serialize(boost::json::object{{"ern", *ern}}));
        if (!response.ok()) {
            log_warning << "Transfer storage remove failed, key: " << key << ", status: " << response.status;
            return false;
        }
        return true;
    }

    bool TransferStorage::MakeDirectory(const std::string &directory) const {

        const auto prefix = asPrefix(directory);
        if (prefix.empty()) return false;

        // The marker is the directory: a zero-byte object whose key ends in "/", which List()
        // skips as an entry but which keeps an otherwise empty directory in existence.
        const auto response = CallModule("esm", "put-object", _token,
                                         scopedHeaders({{"x-euclid-bucket-ern", _bucketErn}, {"x-euclid-key", prefix}}), "");
        if (!response.ok()) {
            log_warning << "Transfer storage mkdir failed, directory: " << directory << ", status: " << response.status;
            return false;
        }
        return true;
    }

    bool TransferStorage::RemoveDirectory(const std::string &directory) const {

        const auto prefix = asPrefix(directory);
        if (prefix.empty()) return false;

        std::vector<std::string> keys;
        const auto objects = listRaw(prefix, keys);

        // Anything beneath the marker means the directory is not empty; refusing here matches
        // what both protocols expect from RMDIR, and avoids silently deleting a subtree.
        for (const auto &object: objects) {
            if (object.name != prefix && object.name.starts_with(prefix)) {
                log_warning << "Transfer storage rmdir refused, directory not empty: " << directory;
                return false;
            }
        }

        return Remove(prefix);
    }

    bool TransferStorage::Rename(const std::string &fromKey, const std::string &toKey) const {

        const auto spool = std::filesystem::temp_directory_path() / ("euclid-transfer-rename-" + std::to_string(std::hash<std::string>{}(fromKey)));

        if (!Download(fromKey, spool)) return false;

        const auto uploaded = Upload(toKey, spool);

        std::error_code ec;
        std::filesystem::remove(spool, ec);

        if (!uploaded) return false;
        return Remove(fromKey);
    }

}// namespace Euclid::Transfer
