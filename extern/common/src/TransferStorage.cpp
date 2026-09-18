// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <TransferStorage.h>
#include <TransferContext.h>

// C++ includes
#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>

// Boost includes
#include <boost/json.hpp>

// Euclid includes
#include <euclid/core/Configuration.h>
#include <euclid/core/LogStream.h>

namespace Euclid::Transfer {

    namespace {

        // Objects up to this size are transferred in a single request; bigger ones are split into
        // parts. Kept well under euclid.gateway.http.max-body (512MB by default, and the point at
        // which a module simply refuses the request): a single-request transfer holds the whole
        // object in memory in this process and again in ESM, so the ceiling that matters in
        // practice is memory, not the configured limit.
        long InlineMaxSize() {
            constexpr long kDefaultInlineMax = 64L * 1024 * 1024;
            return Core::Configuration::instance().getOr<long>("euclid.modules.ets.inline-max", kDefaultInlineMax);
        }

        // How much of a large object travels per request. Every part is held in memory on both
        // sides for the length of one call, and lands as one file in ESM's staging directory, so
        // this trades round trips against footprint.
        long PartSize() {
            constexpr long kDefaultPartSize = 8L * 1024 * 1024;
            return std::max(1L, Core::Configuration::instance().getOr<long>("euclid.modules.ets.part-size", kDefaultPartSize));
        }

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

        // The namespace belongs with the other two and was missing from this list until
        // 2026-09-13. Two things went wrong without it, both silent: an object stored through a
        // transfer server was recorded with no namespace, so the esm.object.created event it
        // raised carried an empty one and a subscriber filtering on it never matched; and the
        // authorization gate was asked about namespace "" while the transfer server's own check
        // asked about the server's real namespace, so a grant scoped to a namespace passed the
        // FTP command and was refused on the storage call behind it.
        if (!_nameSpace.empty()) headers.emplace_back("x-euclid-namespace", _nameSpace);
        return headers;
    }

    std::string AttributesHeader(const std::map<std::string, std::string> &attributes) {

        boost::json::object object;
        for (const auto &[name, value]: attributes) {
            if (name.empty()) continue;
            object[name] = boost::json::object{{"type", "string"}, {"value", value}};
        }
        if (object.empty()) return {};
        return serialize(object);
    }

    std::string TransferStorage::provenanceHeader() const {

        std::map<std::string, std::string> attributes;
        if (!_serverId.empty()) attributes["transferServer"] = _serverId;
        if (!_userId.empty()) attributes["transferUser"] = _userId;
        return AttributesHeader(attributes);
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
                                                        {"x-euclid-part-size", std::to_string(InlineMaxSize())}}),
                                         "");

        // ESM answers 413 for anything it will not serve in one response, which is how this finds
        // out an object is too big without asking for its size first - the common case stays one
        // round trip.
        if (response.status == 413) {
            return downloadInParts(key, spoolPath);
        }
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

    TransferStorage::UploadStream TransferStorage::BeginUpload(const std::string &key) const {
        return UploadStream{*this, stripLeadingSlash(key)};
    }

    TransferStorage::UploadStream::UploadStream(const TransferStorage &storage, std::string key)
        : _storage(&storage), _key(std::move(key)) {}

    TransferStorage::UploadStream::~UploadStream() {
        if (!_finished && !_failed && _partNumber > 0) {
            // ESM has no action to abandon an upload, so the staged parts stay where they are.
            // Nothing to do but say so, loudly enough to be found next to the directory it names.
            log_warning << "Transfer storage upload abandoned without finishing, key: " << _key
                        << ", uploadId: " << _uploadId << ", parts staged: " << _partNumber;
        }
    }

    bool TransferStorage::UploadStream::beginParts() {

        const auto created = CallModuleSticky(_socket, "esm", "create-upload", _storage->_token,
                                              _storage->scopedHeaders({}),
                                              boost::json::serialize(boost::json::object{
                                                      {"bucketErn", _storage->_bucketErn}, {"key", _key}}));
        if (!created.ok()) {
            log_warning << "Transfer storage could not start multipart upload, key: " << _key
                        << ", status: " << created.status;
            return false;
        }

        try {
            _uploadId = std::string(boost::json::parse(created.body).at("uploadId").as_string());
        } catch (const std::exception &e) {
            log_error << "Transfer storage could not read upload id, key: " << _key << ", error: " << e.what();
            return false;
        }
        return true;
    }

    bool TransferStorage::UploadStream::sendPart(const std::size_t size) {

        if (_uploadId.empty() && !beginParts()) return false;

        // Parts are numbered from one, matching what download-part expects on the way back. A part
        // is a numbered file in the upload's directory, so one retried against another instance
        // overwrites rather than duplicates - which is what makes CallModuleSticky() safe here.
        ++_partNumber;
        const auto response = CallModuleSticky(_socket, "esm", "upload-part", _storage->_token,
                                               _storage->scopedHeaders({{"x-euclid-upload-id", _uploadId},
                                                                        {"x-euclid-part-number", std::to_string(_partNumber)}}),
                                               _buffer.substr(0, size));
        if (!response.ok()) {
            log_warning << "Transfer storage upload part failed, key: " << _key << ", part: " << _partNumber
                        << ", status: " << response.status;
            return false;
        }

        _buffer.erase(0, size);
        _total += static_cast<long>(size);
        return true;
    }

    bool TransferStorage::UploadStream::Write(const char *data, const std::size_t size) {

        if (_failed) return false;
        _buffer.append(data, size);

        // Held whole until it outgrows the inline limit, so anything that would have been one
        // put-object still is. Past it, this is a multipart upload from here on.
        if (_uploadId.empty() && static_cast<long>(_buffer.size()) <= InlineMaxSize()) return true;

        const auto partSize = static_cast<std::size_t>(PartSize());
        while (_buffer.size() >= partSize) {
            if (!sendPart(partSize)) {
                _failed = true;
                return false;
            }
        }
        return true;
    }

    bool TransferStorage::UploadStream::Finish() {

        if (_failed) return false;
        _finished = true;

        auto headers = _storage->scopedHeaders({});
        if (auto provenance = _storage->provenanceHeader(); !provenance.empty()) {
            headers.emplace_back("x-euclid-attributes", std::move(provenance));
        }

        // Never grew past the inline limit, so it goes up the way it always did: one call, one
        // object, no parts to assemble.
        if (_uploadId.empty()) {
            auto inlineHeaders = _storage->scopedHeaders({{"x-euclid-bucket-ern", _storage->_bucketErn},
                                                          {"x-euclid-key", _key}});
            if (auto provenance = _storage->provenanceHeader(); !provenance.empty()) {
                inlineHeaders.emplace_back("x-euclid-attributes", std::move(provenance));
            }

            const auto response = CallModule("esm", "put-object", _storage->_token, inlineHeaders, _buffer);
            if (!response.ok()) {
                log_warning << "Transfer storage upload failed, key: " << _key << ", status: " << response.status;
                return false;
            }
            log_info << "Transfer storage stored object, key: " << _key << ", size: " << _buffer.size();
            return true;
        }

        // Whatever is left over is the last, short part.
        if (!_buffer.empty() && !sendPart(_buffer.size())) {
            _failed = true;
            return false;
        }

        // Returns as soon as ESM has taken responsibility for the parts; assembling and hashing
        // them happens in the background there, so the object reaches COMPLETED shortly after this
        // call, not during it - which is what lets the caller answer its client straight away.
        const auto completed = CallModuleSticky(_socket, "esm", "complete-upload", _storage->_token, headers,
                                                boost::json::serialize(boost::json::object{{"uploadId", _uploadId}}));
        if (!completed.ok()) {
            log_warning << "Transfer storage could not complete multipart upload, key: " << _key
                        << ", status: " << completed.status;
            return false;
        }

        log_info << "Transfer storage stored object in parts, key: " << _key << ", size: " << _total
                 << ", parts: " << _partNumber;
        return true;
    }

    bool TransferStorage::Upload(const std::string &key, const std::filesystem::path &spoolPath) const {

        // The spooled form, for callers that already have the whole file - SFTP, whose clients may
        // write at any offset and so cannot be streamed. Reading it back through the same stream is
        // what keeps one ingest path rather than two that drift.
        std::ifstream in(spoolPath, std::ios::binary);
        if (!in) {
            log_error << "Transfer storage could not read spool file: " << spoolPath.string();
            return false;
        }

        auto stream = BeginUpload(key);
        std::string chunk(static_cast<std::size_t>(PartSize()), '\0');
        while (in) {
            in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            const auto read = in.gcount();
            if (read <= 0) break;
            if (!stream.Write(chunk.data(), static_cast<std::size_t>(read))) return false;
        }
        return stream.Finish();
    }

    bool TransferStorage::downloadInParts(const std::string &key, const std::filesystem::path &spoolPath) const {

        // Same arrangement as an upload, and for the same reason: the meta file recording which
        // object is being read is under the shared data directory, so a download carries on
        // against another instance rather than ending when one goes away.
        const auto sockets = ModuleSockets("esm");
        if (sockets.empty()) {
            log_warning << "No running instance of module 'esm' to download key: " << key;
            return false;
        }
        std::string socket = sockets.front();

        const auto cleanKey = stripLeadingSlash(key);
        const auto created = CallModuleSticky(socket, "esm", "create-download", _token, scopedHeaders({}),
                                          boost::json::serialize(boost::json::object{{"bucketErn", _bucketErn}, {"key", cleanKey}}));
        if (!created.ok()) {
            log_warning << "Transfer storage could not start multipart download, key: " << key << ", status: " << created.status;
            return false;
        }

        std::string downloadId;
        long size = 0;
        try {
            const auto parsed = boost::json::parse(created.body);
            downloadId = std::string(parsed.at("downloadId").as_string());
            size = parsed.at("size").to_number<long>();
        } catch (const std::exception &e) {
            log_error << "Transfer storage could not read download id, key: " << key << ", error: " << e.what();
            return false;
        }

        std::ofstream out(spoolPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            log_error << "Transfer storage could not write spool file: " << spoolPath.string();
            return false;
        }

        const auto partSize = PartSize();
        const auto parts = size > 0 ? (size + partSize - 1) / partSize : 0;
        for (long partNumber = 1; partNumber <= parts; ++partNumber) {
            const auto response = CallModuleSticky(socket, "esm", "download-part", _token,
                                                   scopedHeaders({{"x-euclid-download-id", downloadId},
                                                                  {"x-euclid-part-number", std::to_string(partNumber)},
                                                                  {"x-euclid-part-size", std::to_string(partSize)}}),
                                                   "");
            if (!response.ok()) {
                log_warning << "Transfer storage download part failed, key: " << key << ", part: " << partNumber << ", status: " << response.status;
                return false;
            }
            out.write(response.body.data(), static_cast<std::streamsize>(response.body.size()));
            if (!out) {
                log_error << "Transfer storage could not write spool file: " << spoolPath.string();
                return false;
            }
        }
        out.close();

        // Best effort: the parts are already on disk here, and ESM discards a download's scratch
        // state on its own schedule, so a failure to tell it we are done is not worth failing the
        // transfer the client is waiting on.
        std::ignore = CallModuleSticky(socket, "esm", "complete-download", _token, scopedHeaders({}),
                                   boost::json::serialize(boost::json::object{{"downloadId", downloadId}}));

        log_info << "Transfer storage read object in parts, key: " << key << ", size: " << size << ", parts: " << parts;
        return true;
    }

    std::optional<std::string> TransferStorage::ernOf(const std::string &key) const {

        const auto cleanKey = stripLeadingSlash(key);
        // Directories included: DeleteDirectory() resolves a marker's own ERN through here.
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

    long TransferStorage::EnsureDirectories(const std::vector<std::string> &keys) const {

        long created = 0;
        for (const auto &key: keys) {
            if (Stat(key).has_value()) continue;
            if (!MakeDirectory(key)) {
                log_warning << "Could not create home directory, key: " << key;
                continue;
            }
            log_info << "Home directory created, key: " << key;
            created++;
        }
        return created;
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

    bool TransferStorage::DeleteDirectory(const std::string &directory) const {

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
