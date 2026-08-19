#pragma once

// C++ includes
#include <functional>
#include <string>
#include <vector>

namespace Euclid::Core::Monitoring {

    /**
     * @brief Periodically pushes this process' collected metrics to the monitoring module,
     * instead of the monitoring module polling this process.
     *
     * @par
     * On a fixed schedule, drains Core::Monitoring::MonitoringCollector::instance().Collect()
     * and POSTs whatever it returns to every currently-running instance of the "monitoring"
     * module, resolved via SetModuleSocketLookup(). Because each producer decides for itself
     * when to push, there's no window where the monitoring module has to reach out to a
     * socket whose process the autoscaler is in the middle of tearing down - the producer
     * simply stops pushing (its destructor cancels the scheduled task) when it shuts down.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class MetricsPusher {

    public:
        /**
         * @brief Starts pushing this process' metrics under the given module name.
         *
         * @param moduleName name reported alongside every pushed sample, and the name this
         * process is registered under in the module repository (e.g. "queues", "access").
         */
        explicit MetricsPusher(std::string moduleName);

        /**
         * @brief Cancels the scheduled push task.
         */
        ~MetricsPusher();

        MetricsPusher(const MetricsPusher &) = delete;
        MetricsPusher &operator=(const MetricsPusher &) = delete;

        /**
         * @brief Resolves the live Unix socket path(s) of every running instance of the named
         * module - here, always "monitoring".
         *
         * core can't depend on database (database depends on core), so this is the same
         * lookup-injection pattern as HttpActionServer::SetAccessKeyLookup(): each process
         * wires in a Database::RepositoryFactory-backed implementation at startup, via
         * Database::WireModuleSocketLookup().
         */
        using ModuleSocketLookup = std::function<std::vector<std::string>(const std::string &moduleName)>;

        /**
         * @brief Registers the lookup Push() uses to find the monitoring module's socket(s).
         *
         * Until this is called, MetricsPusher has nowhere to push to and every tick is a no-op.
         *
         * @param lookup resolves a module name to the socket path(s) of its running instances.
         */
        static void SetModuleSocketLookup(ModuleSocketLookup lookup);

    private:
        /**
         * @brief One push tick: collects and, if non-empty, pushes to every monitoring instance.
         */
        void push() const;

        std::string _moduleName;
        std::string _taskId;
    };

}// namespace Euclid::Core::Monitoring
