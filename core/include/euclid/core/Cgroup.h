// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace Euclid::Core {

    /**
     * @brief A cgroup v2 group per application instance, and what each one costs - the first two
     * steps of docs/resource-limits.md.
     *
     * @par Why cgroups
     * A process can be measured by pid, and euclid-wrk already does that for the renewal. What a
     * pid cannot say is what a whole instance has cost at its worst (`memory.peak`), whether the
     * kernel's OOM killer is what ended it (`memory.events`), or - later - hold it to a ceiling. A
     * cgroup answers all three, and the JVM sizes its heap from one when it has a limit.
     *
     * @par What it needs
     * Only `Delegate=memory cpu pids` on the unit that runs the worker: systemd then hands the
     * unit's own cgroup subtree to its user, and everything here happens inside that subtree with
     * no root, no capability and no setuid helper. Without it - a worker started by hand, in a
     * container, under another init, on cgroup v1, or on Windows and macOS - the tree is simply not
     * enabled, Tree::Reason() says why once, and every instance runs exactly as it did before.
     * Nothing here refuses to start.
     *
     * @par The layout
     * Mirrors the pools, so somebody reading /sys/fs/cgroup sees what euclid sees:
     * @code
     *   euclid-wrk.service/            the unit's group: the node's total, readable without delegation
     *     worker/                      euclid-wrk itself - a cgroup with children may hold no processes
     *     applications/
     *       <application>/             nothing set; a place to read an application's total
     *         <instanceId>/            one instance and everything it starts
     * @endcode
     *
     * @author jensvogt47\@gmail.com
     */
    namespace Cgroup {

        /**
         * @brief What one group has cost, as its own files say. A figure the kernel did not
         * provide - `memory.peak` before Linux 5.19, a controller not enabled - is left at -1 and
         * not reported, rather than reported as zero.
         */
        struct Usage {

            /** @brief `memory.current`: everything charged to the group now, page cache included. */
            long long memoryBytes{-1};

            /** @brief `memory.peak`: the most it has been charged since the group was made. */
            long long memoryPeakBytes{-1};

            /** @brief `memory.max`: the ceiling, or -1 for "max", which is none. */
            long long memoryMaxBytes{-1};

            /**
             * @brief `memory.events` `oom_kill`: processes in the group the OOM killer has ended -
             * for exceeding the group's own ceiling, and equally when the whole host ran out and
             * the kernel chose one of them.
             */
            long long oomKills{-1};

            /** @brief `cpu.stat` `usage_usec`. */
            long long cpuUsageUsec{-1};

            /** @brief `cpu.stat` `throttled_usec`: time spent held back by `cpu.max`. */
            long long cpuThrottledUsec{-1};

            /** @brief `cgroup.events` `populated`: whether any process is in the group or below. */
            bool populated{false};
        };

        /**
         * @brief The calling process's cgroup v2 path, from the text of /proc/self/cgroup - the
         * `0::/...` line. Empty when there is none, which is cgroup v1 or no cgroups at all.
         */
        [[nodiscard]] std::optional<std::string> ParseSelfCgroup(std::string_view procSelfCgroup);

        /**
         * @brief One number from a flat keyed file - `memory.events`, `cpu.stat`, `cgroup.events` -
         * where each line is `key value`.
         */
        [[nodiscard]] std::optional<long long> KeyedValue(std::string_view text, std::string_view key);

        /**
         * @brief A single-value file such as `memory.current` or `memory.max`. "max" reads as -1,
         * which is how every limit file says "none".
         */
        [[nodiscard]] std::optional<long long> SingleValue(std::string_view text);

        /**
         * @brief Whether a space-separated controller list - `cgroup.controllers` - names one.
         */
        [[nodiscard]] bool HasController(std::string_view controllers, std::string_view controller);

        /**
         * @brief Reads a group's figures. Empty when the group is not there.
         */
        [[nodiscard]] std::optional<Usage> ReadUsage(const std::filesystem::path &group);

        /**
         * @brief A name safe to use as one cgroup directory: the characters of @p name that are
         * not '/', not a leading '.', and not control characters - the names come from the master,
         * and one must not be able to walk out of the tree.
         */
        [[nodiscard]] std::string SafeName(std::string_view name);

        /**
         * @brief What memory.high is set to for a given memory.max: @p highPercent of it, rounded
         * down to a page, or -1 for none (a percentage of 0, or of 100 and above, which would put
         * it at or over the ceiling and so do nothing).
         */
        [[nodiscard]] long long MemoryHighFor(long long maxBytes, int highPercent);

        /**
         * @brief Puts a memory ceiling on a group - docs/resource-limits.md §4.
         *
         * @par
         * memory.max is the hard limit: going over it OOM-kills a process in this group, not one
         * the kernel picks from the whole host. memory.high, below it, throttles allocation and
         * pushes reclaim first, so an instance drifting towards its ceiling slows down and shows
         * up before it dies. memory.swap.max is 0, because an instance swapping has latency that
         * has nothing to do with its code, and would hide the condition this exists to surface;
         * left alone where the kernel has no swap accounting.
         *
         * @par
         * Set before the process joins, so nothing it allocates is ever outside the limit.
         *
         * @param group the instance's group
         * @param maxBytes the ceiling; must be positive
         * @param highPercent memory.high as a percentage of it; 0 leaves memory.high unset
         * @return empty on success, or what failed, worded for an operator's log
         */
        [[nodiscard]] std::optional<std::string> ApplyMemoryLimit(const std::filesystem::path &group, long long maxBytes, int highPercent);

        /**
         * @brief Opens a group's `cgroup.procs` for the child to join it through - in the parent,
         * before the fork, so the child has nothing to do but write. -1 when it cannot be opened,
         * and always -1 on Windows. Close-on-exec, so the application never inherits it.
         */
        [[nodiscard]] int OpenProcs(const std::filesystem::path &group);

        /**
         * @brief Moves the calling process into the group @p procsFd was opened for. For the
         * forked child between fork() and exec(): one write(2) of "0" - the kernel reads 0 as "the
         * writer" - so it is async-signal-safe, which everything there has to be.
         *
         * @return whether the kernel took it
         */
        bool JoinFromChild(int procsFd) noexcept;

        /**
         * @brief The instance groups of one worker, under the subtree systemd delegated to it.
         *
         * @par
         * Not thread safe; the worker drives it from its one reconciling thread.
         */
        class Tree {

          public:

            /**
             * @brief A tree that is not enabled, with the reason it is not.
             */
            Tree() = default;

            /**
             * @brief Sets the tree up under this process's own cgroup, or explains why it cannot.
             *
             * @par
             * Moves this process into `worker/` before enabling any controller for its children:
             * cgroup v2 allows a group with controllers enabled for its children to hold no
             * processes of its own, so the order is not a choice.
             *
             * @param mount where cgroup2 is mounted - /sys/fs/cgroup, a temporary directory in tests
             * @param self this process's group relative to @p mount; read from /proc/self/cgroup
             * when not given
             */
            [[nodiscard]] static Tree Setup(const std::filesystem::path &mount = "/sys/fs/cgroup",
                                            std::optional<std::string> self = std::nullopt);

            /** @brief Whether instances get a group of their own. */
            [[nodiscard]] bool Enabled() const { return _enabled; }

            /** @brief Why not, when they do not - worded for an operator's log. */
            [[nodiscard]] const std::string &Reason() const { return _reason; }

            /**
             * @brief The unit's own group: the node's total, and readable whether or not it was
             * delegated. Empty when this process is not in a cgroup v2 group at all.
             */
            [[nodiscard]] const std::optional<std::filesystem::path> &Root() const { return _root; }

            /**
             * @brief Makes (or reuses) the group for one instance and returns it. Empty when the
             * tree is not enabled or the directory could not be made.
             */
            [[nodiscard]] std::optional<std::filesystem::path> CreateInstance(const std::string &application, const std::string &instanceId) const;

            /**
             * @brief Removes an instance's group once it is empty. Quietly leaves one that still
             * holds a process - something the instance started may outlive it by a moment - for
             * Sweep() to take on a later tick.
             */
            static bool RemoveInstance(const std::filesystem::path &group);

            /**
             * @brief Removes every empty instance group not named in @p keep, and every empty
             * application group left behind. For groups a previous run of the worker left, and
             * ones RemoveInstance() could not take yet.
             */
            void Sweep(const std::function<bool(const std::string &instanceId)> &keep) const;

          private:

            bool _enabled{false};
            std::string _reason{"cgroups are not set up"};
            std::optional<std::filesystem::path> _root;
            std::filesystem::path _applications;
        };

    }// namespace Cgroup

}// namespace Euclid::Core
