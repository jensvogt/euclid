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
#include <euclid/dto/module/ModuleProcess.h>

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
         * @brief Registers a module with the system.
         *
         * Creates a pool for this module, pre-populated with `cfg.minInstances` stopped
         * placeholder instances. Actual process spawning happens in startAll()/start().
         *
         * @param cfg configuration
         */
        void registerModule(const Dto::ModuleConfig &cfg);

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
        };

        /**
         * @brief Maintains a collection of service module pools managed by the system, keyed by module name.
         */
        std::map<std::string, ServiceGroup> _services;

        /**
         * @brief Ensures thread-safe access to shared resources.
         */
        std::recursive_mutex _mutex;

        /**
         * @brief Thread responsible for monitoring and managing system processes.
         */
        std::thread _watchdog;

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

} // namespace Euclid::main
