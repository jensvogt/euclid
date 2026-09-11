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

}// namespace Euclid::Core
