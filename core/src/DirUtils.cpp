// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/7/26.
//

// C++ includes
#include <algorithm>
#include <cctype>

// Euclid includes
#include <euclid/core/DirUtils.h>
#include <euclid/core/LogStream.h>

namespace Euclid::Core {

    namespace {

        // Whether a name may be fanned out. A name is a filename and nothing else: one that
        // carries a separator, or is "." or "..", would build a path pointing somewhere other than
        // where the caller believes it does, and one too short to take the levels out of has
        // nothing to fan out on. Such a name keeps the flat layout, which is exactly as safe as it
        // was before and never silently escapes the storage directory.
        bool canShard(const std::string &name) {

            if (name.size() < DirUtils::kShardLength * DirUtils::kShardLevels) return false;
            if (name == "." || name == "..") return false;

            return std::ranges::none_of(name, [](const unsigned char c) {
                return c == '/' || c == '\\' || c == '\0';
            });
        }

    }// namespace

    std::filesystem::path DirUtils::ShardedFilePath(const std::filesystem::path &rootDir, const std::string &name) {

        if (!canShard(name)) return rootDir / name;

        auto path = rootDir / kObjectDir;
        for (std::size_t level = 0; level < kShardLevels; ++level) {
            path /= name.substr(level * kShardLength, kShardLength);
        }
        return path / name;
    }

    std::filesystem::path DirUtils::CreateFilePath(const std::filesystem::path &rootDir, const std::string &name) {

        auto path = ShardedFilePath(rootDir, name);

        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (!ec) return path;

        // The storage root itself is the likely reason - it is the directory that fills up - so
        // there is nowhere better to put the file than where it would have gone before. Said as a
        // warning because the installation is then back to accumulating files in one directory,
        // which is the condition this exists to get out of.
        log_warning << "Could not create the object directory, storing flat, path: " << path.parent_path().string()
                    << ", error: " << ec.message();
        return rootDir / name;
    }

    std::filesystem::path DirUtils::FindFilePath(const std::filesystem::path &rootDir, const std::string &name) {

        auto sharded = ShardedFilePath(rootDir, name);

        std::error_code ec;
        if (std::filesystem::exists(sharded, ec)) return sharded;

        // Everything written before the fan-out existed, and anything a failed CreateFilePath()
        // had to put there.
        return rootDir / name;
    }

    bool DirUtils::RemoveFile(const std::filesystem::path &rootDir, const std::string &name, std::error_code &ec) {

        ec.clear();
        bool removed = false;

        for (const auto &path: {ShardedFilePath(rootDir, name), rootDir / name}) {
            std::error_code removeEc;
            if (std::filesystem::remove(path, removeEc)) {
                removed = true;
            } else if (removeEc) {
                ec = removeEc;
            }
        }
        return removed;
    }

    DirUtils::DiskUsage DirUtils::MeasureDiskUsage(const std::filesystem::path &root, const std::atomic<bool> *cancel) {

        DiskUsage usage;
        std::error_code ec;
        std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec);
        if (ec) return usage;

        const std::filesystem::recursive_directory_iterator end;
        while (it != end) {
            if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) return usage;

            const auto &entry = *it;
            std::error_code statError;
            const auto status = entry.symlink_status(statError);
            if (statError) {
                ++usage.unreadable;
            } else if (std::filesystem::is_directory(status)) {
                ++usage.directories;
            } else if (std::filesystem::is_regular_file(status)) {
                const auto size = entry.file_size(statError);
                if (statError) {
                    ++usage.unreadable;
                } else {
                    usage.bytes += size;
                    ++usage.files;
                }
            }

            it.increment(ec);
            if (ec) {
                // A directory that went away, or cannot be listed, under the entry just counted.
                // Stepped out of rather than retried: an increment that failed may leave the
                // iterator where it was, and retrying it would be a walk that never ends.
                ++usage.unreadable;
                ec.clear();
                // Short of the end either way, so not complete.
                if (it == end || it.depth() == 0) return usage;
                it.pop(ec);
                if (ec) return usage;
            }
        }
        usage.complete = true;
        return usage;
    }

}// namespace Euclid::Core
