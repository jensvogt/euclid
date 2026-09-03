#pragma once

// C++ includes
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// Euclid includes
#include <euclid/database/entity/emm/Module.h>
#include <euclid/dto/emm/ModuleProcess.h>

namespace Euclid::main {

    bool waitForSocket(const std::string &path, int timeoutMs);

    bool spawnInstance(const std::shared_ptr<Dto::ModuleProcess> &svc);

    /**
     * @brief Derives a per-instance Unix domain socket path from a module's base socketPath
     *        and an instance pid, e.g. "/var/run/euclid/sqs.sock" + 4242 -> "/var/run/euclid/sqs.4242.sock".
     */
    std::string makeInstanceSocketPath(const std::string &base, pid_t pid);

    /**
     * @brief Handle to a single running module instance, returned by acquireInstance().
     */
    struct InstanceHandle {
        std::string socketPath;
        pid_t pid;
    };

    /**
     * @brief Manages and controls core service operations.
     *
     * ServiceController is responsible for handling the startup, shutdown,
     * monitoring, and autoscaling (min/max instances, round-robin dispatch) of the
     * various module process pools within the system.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class ServiceController {
    public:

        /**
         * @brief Stops the watchdog thread so this object can be destroyed safely.
         *
         * std::thread's destructor calls std::terminate() immediately if the thread is still
         * joinable, and nothing else ever stops the watchdog - its own loop only exits once
         * _running is false, which only this destructor sets. Without this, destroying a
         * ServiceController while the watchdog is running (always, in practice, since it runs for
         * the process's whole lifetime) crashes the process right as it would otherwise exit
         * cleanly.
         */
        ~ServiceController();

        /**
         * @brief Registers a module with the system.
         *
         * Creates a pool for this module, pre-populated with `cfg.minInstances` stopped
         * placeholder instances. Actual process spawning happens in startAll()/start().
         *
         * @param cfg configuration
         */
        void registerModule(const Dto::ModuleConfig &cfg);

        /**
         * @brief Removes a module's pool entirely, so nothing restarts it.
         *
         * Only meaningful for modules registered at runtime rather than from the config file -
         * in practice the transfer servers reconcileTransferServers() manages, whose definitions
         * can be deleted while the manager is running.
         *
         * @param name module name.
         * @return true if a pool was removed.
         */
        bool deregisterModule(const std::string &name);

        /**
         * @brief Whether a module pool with this name exists.
         *
         * @par
         * The gateway needs this to route to applications: the modules it can name are a fixed
         * list compiled into it, but an application's name is whatever it was deployed as, and
         * only the controller knows which pools exist at this moment.
         *
         * @param name module name.
         * @return true if a pool is registered under that name.
         */
        [[nodiscard]]
        bool hasService(const std::string &name) const;

        /**
         * @brief Makes the running transfer server processes match the definitions ETS holds.
         *
         * @par
         * Config-declared modules are registered once at startup; transfer servers cannot be,
         * because they are created and destroyed through the ETS module long after the manager
         * has read its config. This closes that gap by treating the ETS repository as the
         * desired state and this controller's pools as the observed one: a definition marked
         * RUNNING that has no pool gets registered and started, one marked STOPPED that is up
         * gets stopped, and a definition that has been deleted outright is stopped and its pool
         * removed.
         *
         * @par
         * Being a reconcile rather than a command is what makes it robust: no message can be
         * missed, the manager recovers the full picture on restart, and ETS being unreachable at
         * the moment a change is requested does not matter, since the change was durably written
         * before it ever needed to be acted on.
         */
        void reconcileTransferServers();

        /**
         * @brief Brings the running application pools in line with what EAP has defined.
         *
         * @par
         * The same reconcile reconcileTransferServers() performs, for applications: every
         * application whose desired state is RUNNING gets a module pool, everything else is torn
         * down. Two things happen here that a transfer server does not need, because an
         * application is foreign code rather than a euclid binary:
         *
         * @par Artifact
         * The artifact is an object in an ESM bucket, so it is materialised onto local disk
         * before the first instance starts - which is what lets a manager on a fresh host bring
         * an application up from nothing but the database and the object store.
         *
         * @par Credentials
         * The application's EAM user's access key is passed in through the environment, so the
         * process can sign its own calls back into euclid (RFC 9421). Nothing else hands it an
         * identity: it holds no token, reads no configuration file and never touches the
         * database.
         */
        void reconcileApplications();

        /**
         * @brief Starts a service by its name.
         *
         * Spawns instances until the module's pool reaches its configured minInstances.
         *
         * @param name The name of the service to be started.
         * @return True if the pool reaches minInstances successfully, otherwise false.
         */
        bool start(const std::string &name);

        /**
         * @brief Stops a running service by its name.
         *
         * Stops every instance in the module's pool.
         *
         * @param name The name of the service to be stopped.
         * @param timeoutMs The maximum time, in milliseconds, to wait for each instance to stop. Default is 5000 ms.
         * @return True if all instances were stopped successfully, otherwise false.
         */
        bool stop(const std::string &name, int timeoutMs = 5000);

        /**
         * @brief Restarts the service by shutting it down and starting it again.
         *
         * @param name The name of the service to be stopped.
         * @return True if the restart operation succeeds, false otherwise.
         */
        bool restart(const std::string &name);

        /**
         * @brief The order registered modules should be started in, dependencies first.
         *
         * @par
         * A topological sort of ModuleConfig::dependencies. Modules that depend on nothing keep
         * the order they are held in (alphabetical), so an installation that declares no
         * dependencies behaves exactly as it did before. A dependency naming a module that is not
         * registered - inactive, misspelled - is reported and ignored rather than waited for, and
         * a cycle is reported rather than followed.
         *
         * @return every registered module's name, each after everything it depends on.
         */
        [[nodiscard]]
        std::vector<std::string> startOrder() const;


        /**
         * @brief Whether every module this configuration depends on has a running instance.
         *
         * @par
         * "Running" means the gateway would route to it. Used to hold an application back until
         * the installation it is about to call is actually there - see reconcileApplications().
         *
         * @param config the module or application configuration to check
         * @return true if every named dependency has at least one RUNNING instance.
         */
        [[nodiscard]]
        bool dependenciesRunning(const Dto::ModuleConfig &config) const;

        /**
         * @brief Applies the instance limits and thread counts asked for through EMM.
         *
         * @par
         * The limits the autoscaler works within live in this controller's memory, and EMM runs in
         * another process - so it records what was asked for on the module's database document and
         * this is what makes it real, on the same reconcile tick as transfer servers and
         * applications. Raising a floor spawns up to it here; lowering a ceiling is left to the
         * ordinary scale-down, so nothing is killed mid-request to obey a new number.
         *
         * @par
         * Also applies what "emm stop-module" asked for, ahead of the limits: a module being
         * stopped has its instances stopped and is then left alone by the autoscaler, the
         * crash-restart and the next manager start, until "emm start-module" brings it back.
         *
         * @par
         * Reads every module document once and hands the same list to reconcileWorkerThreads(),
         * rather than each reading the collection for itself on every tick.
         */
        void reconcileModuleSettings();

        /**
         * @brief Restarts instances whose module's thread count has been changed since they started.
         *
         * @par
         * Unlike an instance limit, a thread count cannot be applied to a process that is already
         * running - the threads are created once, when the module builds its io_context - so the
         * only way to honour a new one is to start the process again. Instances are restarted one
         * per reconcile tick: a pool of any size keeps serving throughout, and the watchdog thread
         * this runs on is never held for longer than a single restart.
         *
         * @param modules every module document, as read by reconcileModuleSettings().
         */
        void reconcileWorkerThreads(const std::vector<Database::Entity::Module> &modules);

        /**
         * @brief Queues the instances of any module somebody asked to restart through EMM.
         *
         * @par
         * The one EMM request that is an event rather than a setting, which is why it is a
         * timestamp rather than a flag: the group remembers which request it has carried out, so
         * asking twice restarts twice and reading the same request every tick restarts nothing.
         * A stopped module is skipped - there is nothing running to restart, and starting it would
         * undo what stop-module asked for.
         *
         * @param modules every module document, as read by reconcileModuleSettings().
         */
        void reconcileRestarts(const std::vector<Database::Entity::Module> &modules);

        /**
         * @brief Restarts one queued instance, if there is one.
         *
         * @par
         * One per reconcile tick, across every module: this runs on the watchdog thread, so a
         * whole pool cycling must not hold it, and a pool of any size keeps serving from its
         * remaining instances while it is worked through.
         */
        void rollQueuedInstance();

        /**
         * @brief Whether every module the installation is made of has a running instance.
         *
         * @par
         * Only ModuleConfig::core services count - the modules from euclid.json, not the transfer
         * servers and applications the manager spawns from the database. This is what an
         * application waits for before it is started, since it does not say which modules it
         * calls and may call any of them the moment it comes up.
         *
         * @return true if every core module has at least one RUNNING instance.
         */
        [[nodiscard]]
        bool modulesRunning() const;

        /**
         * @brief Initiates the startup process for all registered services.
         */
        void startAll();

        /**
         * @brief Stops all currently running services.
         */
        void stopAll();

        /**
         * @brief Retrieves the aggregate state of a specified service module.
         *
         * RUNNING if at least one instance is RUNNING, otherwise the most "active"
         * transitional state present among its instances, otherwise STOPPED.
         *
         * @param name The name of the service module whose state is to be retrieved.
         * @return The aggregate state of the specified service module.
         */
        Database::Entity::ModuleState getState(const std::string &name);

        /**
         * @brief One row of listModules() output - a single instance within a module's pool.
         */
        struct ModuleInstanceInfo {
            std::string name;
            pid_t pid;
            Database::Entity::ModuleState state;
            std::string socketPath;
            int activeRequests;
        };

        /**
         * @brief Returns a snapshot of every instance of every registered module.
         *
         * Thread-safe; acquires the internal mutex before reading the service map.
         *
         * @return Vector of per-instance info, one entry per running or known instance.
         */
        std::vector<ModuleInstanceInfo> listModules();

        /**
         * @brief Acquires the next instance for the named service, round-robin, and marks it busy.
         *
         * Advances the group's round-robin cursor across RUNNING instances and increments the
         * chosen instance's activeRequests counter (feeds the autoscaler's load signal). Callers
         * must pair this with releaseInstance() once the request completes.
         *
         * @param name Service name (e.g. "access", "queues").
         * @return Handle to the chosen instance's socket path and pid, or nullopt if the service
         *         is not registered or has no running instances.
         */
        std::optional<InstanceHandle> acquireInstance(const std::string &name);

        /**
         * @brief Releases an instance previously returned by acquireInstance().
         *
         * Decrements the instance's activeRequests counter and, once it reaches zero, stamps
         * lastIdleAt so the autoscaler can consider scaling the instance down after being idle.
         *
         * @param name Service name the instance belongs to.
         * @param pid  Pid of the instance to release, as returned in the InstanceHandle.
         */
        void releaseInstance(const std::string &name, pid_t pid);

        /**
         * @brief Lets a client proactively declare how many instances of a service it's about to
         * need, instead of the autoscaler having to infer demand from sampling activeRequests.
         *
         * Reactive scaling has an inherent blind spot for this kind of workload: a request's
         * activeRequests>0 window is often much shorter than the watchdog's 1s sampling interval
         * (e.g. a storage part upload's gateway-to-module hop is typically single-digit
         * milliseconds), so as a pool grows, the odds of the sample catching any given instance
         * mid-request keep dropping - scale-up can stall well below what real throughput would
         * justify. A client that already knows its own concurrency (e.g. the CLI's upload-file
         * --concurrency) can just say so up front.
         *
         * Takes the max of the current desired count and this call's value, so multiple
         * concurrent declarations (e.g. two uploads in flight at once) don't undercut each other.
         * The declared target is dropped back to minInstances the next time the group is deemed
         * idle enough to scale down - see evaluateScaling() - so it doesn't become a permanent
         * floor once the work it was declared for is done.
         *
         * @param name Service name (e.g. "storage").
         * @param desired Instance count the caller expects to use soon.
         * @return False if the service is registered and `desired` exceeds its configured
         *         maxInstances (caller should reject the request rather than silently clamp, so
         *         the client finds out its concurrency won't be honored instead of just seeing
         *         degraded throughput). True if accepted, or if the service isn't registered at
         *         all (that's a different failure, left for acquireInstance() to report).
         */
        [[nodiscard]] bool declareExpectedConcurrency(const std::string &name, int desired);

        /**
         * @brief Waits for a process to terminate or for a timeout to elapse.
         *
         * @param pid The process ID of the target process to monitor.
         * @param timeoutMs The maximum time in milliseconds to wait for the process to exit.
         * @return True if the process exited within the timeout duration, or false if the wait
         *         timed out before the process exited.
         */
        static bool waitForExit(pid_t pid, int timeoutMs);

        /**
         * @brief Initializes and starts the watchdog mechanism.
         *
         * Each tick (1s) also drives crash-restart backoff and autoscaling (evaluateScaling()).
         */
        void startWatchdog();

        /**
         * @brief Stops the watchdog thread and joins it. Safe to call more than once (a no-op
         * if the watchdog isn't running, e.g. already stopped or never started).
         *
         * Callers doing an orderly shutdown must call this - and let it fully join - before
         * stopAll(): otherwise the watchdog can still be mid-tick, autoscaling a module up in
         * response to still-arriving traffic, while stopAll() is concurrently working through
         * its own (already-taken) snapshot of instances to stop. Any instance the watchdog
         * spawns after that snapshot was taken is invisible to that stopAll() call and is left
         * running - the module most likely to be caught this way is whichever one both runs
         * multiple instances and has the slowest-draining in-flight requests (e.g. esm, mid
         * large-file transfer), since that's what keeps the autoscaler's "busy" signal alive
         * long enough to overlap a long sequential stop.
         */
        void stopWatchdog();

        /**
         * @brief Handles the termination of a child process.
         *
         * Invoked when a child process exits, this method manages cleanup and
         * performs necessary actions based on the process's exit status.
         */
        void onChildExit();

        /**
         * @brief Restarts all active services in the system.
         */
        void restartAll();

    private:

        /**
         * @brief One module's pool: its spawn template config plus its live instances.
         */
        struct ServiceGroup {
            Dto::ModuleConfig config;
            std::vector<std::shared_ptr<Dto::ModuleProcess> > instances;
            size_t rrCursor = 0;

            /**
             * @brief Last time any instance in this group was acquireInstance()'d, i.e. the
             * group's own last-busy time as opposed to any single instance's. Scale-down is
             * gated on this: round-robin can leave one instance unpicked for a while even while
             * the group overall stays busy, and per-instance idle time alone can't tell the
             * difference between "nobody wants this instance" and "nobody wants any instance."
             */
            std::chrono::steady_clock::time_point lastActivityAt = std::chrono::steady_clock::now();

            /**
             * @brief Instance count a client has proactively declared it expects to use soon, via
             * declareExpectedConcurrency(). evaluateScaling() ramps toward this as an additional
             * scale-up trigger alongside busy>0, and it's reset to minInstances the next time the
             * group actually scales down, so it doesn't outlive the workload that declared it.
             */
            int desiredCount = 0;

            /**
             * @brief Whether somebody stopped this module through "emm stop-module".
             *
             * @par
             * Not the same as "no instance is running": it is why nothing is running, and it is
             * what keeps the autoscaler, the crash-restart and the next manager start from
             * quietly putting it back.
             */
            bool stopped = false;

            /**
             * @brief "Nobody has looked at this module's stored thread count yet", as distinct
             * from -1, which is a module nobody has ever set one for.
             */
            static constexpr int kThreadsUnobserved = -2;

            /**
             * @brief The thread count this group's running instances were started with, as far as
             * reconcileWorkerThreads() knows. Held here rather than read back from the instances
             * because only the module process itself ever sees the figure it started with.
             */
            int appliedThreads = kThreadsUnobserved;

            /**
             * @brief Instances still to be restarted - because the thread count changed, or
             * because somebody asked for a restart - one per reconcile tick. Empty in the
             * ordinary case.
             */
            std::vector<std::shared_ptr<Dto::ModuleProcess> > pendingRoll;

            /**
             * @brief Whether this group's stored restart request has been read at least once.
             *
             * @par
             * Needed because "never asked" is a legal stored value (the epoch) that a manager
             * which has just started cannot tell apart from a request it should act on. The first
             * read records whatever is stored without acting: a restart asked for before this
             * manager existed has already been served by it starting.
             */
            bool restartObserved = false;

            /**
             * @brief The restart request this group has already carried out, so the same one is
             * not carried out again on every following tick.
             */
            std::chrono::system_clock::time_point appliedRestartAt{};
        };

        /**
         * @brief Queues every running instance of a group to be restarted, replacing anything
         * already queued for it. Caller holds _mutex.
         *
         * @param group the group whose instances should be cycled.
         */
        static void queueRoll(ServiceGroup &group);


        /**
         * @brief Maintains a collection of service module pools managed by the system, keyed by module name.
         */
        std::map<std::string, ServiceGroup> _services;

        /**
         * @brief Ensures thread-safe access to shared resources.
         */
        mutable std::recursive_mutex _mutex;

        /**
         * @brief Thread responsible for monitoring and managing system processes.
         */
        std::thread _watchdog;

        /**
         * @brief Watchdog ticks since the last transfer server reconcile, and how many to let
         *        pass between reconciles. The watchdog ticks once a second; spawning a transfer
         *        server is not urgent enough to justify re-reading every definition that often.
         */
        int _transferReconcileTick = 0;
        static constexpr int kTransferReconcileTicks = 5;

        /**
         * @brief Indicates whether the service is currently running.
         */
        std::atomic<bool> _running{true};

        /**
         * @brief Seconds a service GROUP must have zero acquireInstance() activity before the
         *        autoscaler will scale one of its idle instances down, provided the pool is above
         *        minInstances. Group-level rather than per-instance, so round-robin skipping one
         *        instance for a while doesn't get it killed while its siblings stay busy. Read
         *        once from euclid.scaling.scale-down-idle-seconds when startWatchdog() is called.
         */
        long _scaleDownIdleSeconds = 60;

        /**
         * @brief True if this deployment uses the MongoDB backend (euclid.database.backend), read
         *        once from config when startWatchdog() is called. Gates whether the watchdog
         *        bothers checking database reachability at all - a memory-backend deployment has
         *        no such dependency to check.
         */
        bool _usesMongoBackend = true;

        /**
         * @brief Tracks the database's reachability across ticks so transitions (outage start/end)
         *        can be logged once instead of every watchdog tick.
         */
        bool _databaseWasReachable = true;

        /**
         * @brief Checks whether the configured database backend is currently reachable.
         *
         * A crashed/unreachable MongoDB makes every module fail its startup health checks at
         * once, and without this check the watchdog would keep respawning all of them in
         * lockstep every tick, forking dozens of short-lived processes per second across the
         * whole module set - a thundering herd that has been observed to spike real memory
         * pressure severely enough for the kernel's OOM killer to start killing unrelated
         * processes system-wide (including MongoDB itself, compounding the outage, and on one
         * occasion the caller's desktop session). Always true for the memory backend, which has
         * no such external dependency.
         *
         * @return True if spawning/restarting should proceed this tick.
         */
        [[nodiscard]] bool isDatabaseReachable() const;

        /**
         * @brief Retrieves a service group by name.
         *
         * @param name The name of the service module to retrieve.
         * @return Pointer to the ServiceGroup, or nullptr if not registered. Caller must hold _mutex.
         */
        ServiceGroup *getGroup(const std::string &name);

        /**
         * @brief Finds a module instance by its process ID (PID), across all service groups.
         *
         * @param pid The process ID of the module instance to locate.
         * @return A shared pointer to the corresponding ModuleProcess instance, or `nullptr` if
         *         no instance is found with the specified PID.
         */
        std::shared_ptr<Dto::ModuleProcess> findByPid(pid_t pid);

        /**
         * @brief Shared cleanup for an instance whose process has already exited, run by
         *        onChildExit() once it learns about the exit - via waitpid() on POSIX, or by
         *        polling GetExitCodeProcess() on Windows (see reapExitedWindows()).
         *
         * Distinguishes a deliberate stop (state already STOPPING/STOPPED) from a crash,
         * unlinks the instance's socket file, and schedules a restart for the latter if the
         * module's config allows it.
         *
         * @param svc The instance whose process has exited.
         */
        static void handleExitedInstance(const std::shared_ptr<Dto::ModuleProcess> &svc);

#if defined(_WIN32)
        /**
         * @brief Windows has no SIGCHLD/waitpid(-1) equivalent to learn about child exits
         *        asynchronously, so onChildExit() instead polls every tracked instance's
         *        process handle here; called once per watchdog tick (see startWatchdog()).
         */
        void reapExitedWindows();
#endif

        /**
         * @brief Calculates the stable runtime of a module instance.
         *
         * @param svc A shared pointer to the module instance for which the stable runtime is to be calculated.
         * @return The stable runtime of the module instance in seconds.
         */
        static long stableRuntime(const std::shared_ptr<Dto::ModuleProcess> &svc);

        /**
         * @brief Schedules a restart for the specified module instance.
         *
         * @param svc A shared pointer to the module instance that needs to be restarted.
         */
        static void scheduleRestart(const std::shared_ptr<Dto::ModuleProcess> &svc);

        /**
         * @brief Sends SIGTERM (escalating to SIGKILL) to a single instance, waits for exit,
         *        marks it STOPPED and unlinks its instance socket file. Blocking - callers must
         *        not hold _mutex while calling this.
         *
         * @param svc    Instance to stop.
         * @param timeoutMs Timeout for graceful shutdown before escalating to SIGKILL.
         */
        static bool stopInstance(const std::shared_ptr<Dto::ModuleProcess> &svc, int timeoutMs = 5000);

        /**
         * @brief Decides, for every service group, whether to grow or shrink its pool by at most
         *        one instance, within [minInstances, maxInstances]. Called once per watchdog tick
         *        while holding _mutex. Only decides and mutates the group's instance list; the
         *        actual (blocking) spawn/stop calls are returned via the out-params so the caller
         *        can perform them after releasing _mutex.
         *
         * @param toSpawn Newly appended placeholder instances that still need spawnInstance().
         * @param toStop  Instances already removed from their group that still need stopInstance().
         */
        void evaluateScaling(std::vector<std::shared_ptr<Dto::ModuleProcess> > &toSpawn,
                             std::vector<std::shared_ptr<Dto::ModuleProcess> > &toStop);
    };

}// namespace Euclid::main