// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace Euclid::Database::Entity::ESM {

    /**
     * @brief Where an object's bytes are, and so whether it can be read.
     *
     * @par
     * STANDARD is online: the file is in ESM's data directory and the object reads as it always
     * has. ARCHIVED is offline: the file has been moved to the archive directory
     * (euclid.modules.esm.archive-dir, which may be a cheaper disk), and reading the object is
     * refused until "esm restore-object" moves it back.
     */
    enum class LifecycleState {
        STANDARD,
        ARCHIVED
    };

    inline std::string LifecycleStateToString(const LifecycleState state) {
        return state == LifecycleState::ARCHIVED ? "ARCHIVED" : "STANDARD";
    }

    /**
     * @brief Reads a stored state. Anything that is not ARCHIVED - including nothing at all, which
     * is every object stored before lifecycle states existed - is STANDARD: an object is online
     * unless something moved it.
     */
    inline LifecycleState LifecycleStateFromString(const std::string &state) {
        return state == "ARCHIVED" ? LifecycleState::ARCHIVED : LifecycleState::STANDARD;
    }

    /**
     * @brief One rule of a bucket's lifecycle: archive what has gone unused for long enough.
     */
    struct LifecycleRule {

        /**
         * @brief Name the rule is managed under, unique within its bucket.
         */
        std::string id;

        /**
         * @brief Keys the rule applies to: those starting with this. Empty for the whole bucket.
         * Where several rules match a key, the one with the longest prefix wins - a general rule
         * for the bucket and a stricter one carved out of it for a folder.
         */
        std::string prefix;

        /**
         * @brief Days an object may go unused before it is archived - see LastUsed().
         */
        long archiveAfterDays = 0;

        /**
         * @brief The grace period: days a restored object stays online. After that, if it has not
         * been used since it was restored, it is archived again; if it has, the rule applies to it
         * as to any other object, counting from its last use.
         */
        long restoreDays = 0;

        bool enabled = true;

        bool operator==(const LifecycleRule &) const = default;
    };

    /**
     * @brief What is wrong with a rule, or nothing.
     */
    inline std::optional<std::string> LifecycleRuleProblem(const LifecycleRule &rule) {
        if (rule.id.empty()) return "a rule needs an id";
        if (rule.archiveAfterDays < 1 || rule.archiveAfterDays > 36500) return "rule " + rule.id + ": archiveAfterDays has to be from 1 to 36500";
        if (rule.restoreDays < 1 || rule.restoreDays > 36500) return "rule " + rule.id + ": restoreDays has to be from 1 to 36500";
        return std::nullopt;
    }

    /**
     * @brief The rule that applies to a key: the enabled one with the longest prefix that matches,
     * or none.
     */
    inline const LifecycleRule *RuleFor(const std::vector<LifecycleRule> &rules, const std::string &key) {
        const LifecycleRule *best = nullptr;
        for (const auto &rule: rules) {
            if (!rule.enabled || !key.starts_with(rule.prefix)) continue;
            if (best == nullptr || rule.prefix.size() > best->prefix.size()) best = &rule;
        }
        return best;
    }

    /**
     * @brief Everything the decision needs to know about one object, so it can be made - and
     * tested - without an object or a bucket in hand.
     */
    struct LifecycleFacts {
        LifecycleState state = LifecycleState::STANDARD;
        /// Readable at all: a complete upload, not a directory marker.
        bool archivable = true;
        /// LastUsed(): the last access, or the last modification for an object with none recorded.
        std::chrono::system_clock::time_point lastUsed;
        std::optional<std::chrono::system_clock::time_point> lastAccessed;
        std::optional<std::chrono::system_clock::time_point> restoredAt;
        std::optional<std::chrono::system_clock::time_point> restoredUntil;
    };

    /**
     * @brief Whether a rule says an object should be archived now.
     *
     * @par
     * Unused for archiveAfterDays, it is. Restored, it is left online until its grace period ends;
     * then it is archived again unless it was used after the restore - a restore is a request to
     * look at something, not a use of it, so an object nobody actually read goes straight back.
     * One that was read is held to the rule like any other, from its last use.
     */
    inline bool ShouldArchive(const LifecycleFacts &facts, const LifecycleRule &rule, const std::chrono::system_clock::time_point now) {
        if (facts.state != LifecycleState::STANDARD || !facts.archivable) return false;

        if (facts.restoredUntil.has_value()) {
            if (now < *facts.restoredUntil) return false;
            const auto usedSinceRestore = facts.lastAccessed.has_value() && facts.restoredAt.has_value()
                                          && *facts.lastAccessed > *facts.restoredAt;
            if (!usedSinceRestore) return true;
        }
        return now - facts.lastUsed >= std::chrono::days(rule.archiveAfterDays);
    }

}// namespace Euclid::Database::Entity::ESM
