// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// euclid-worker: runs applications euclid placed on this host.
//
// A euclid client, not a module. It reaches the gateway over signed HTTP, holds a role, and has no
// MongoDB credentials, no EMD, no Unix sockets anyone connects to, and nothing listening that the
// internet can reach. See docs/worker-nodes.md.
//
// It decides nothing. How many instances an application runs, and which node the next one goes on,
// are the master's - this reconciles toward what it is told, reports what it did, and stops its own
// work when its lease runs out.
//

// C++ includes
#include <atomic>
#include <csignal>
#include <iostream>
#include <thread>

// Boost includes
#include <boost/program_options.hpp>

// Euclid includes
#include <WorkerClient.h>
#include <WorkerReconciler.h>
#include <euclid/cli/credentials/Credentials.h>
#include <euclid/core/Configuration.h>
#include <euclid/core/LogStream.h>
#include <euclid/core/SystemUtils.h>

namespace po = boost::program_options;

namespace {

    std::atomic_bool g_shutdownRequested{false};

    void onSignal(int) { g_shutdownRequested = true; }

    std::map<std::string, std::string> parseLabels(const std::vector<std::string> &values) {

        std::map<std::string, std::string> labels;
        for (const auto &value: values) {
            if (const auto equals = value.find('='); equals != std::string::npos) {
                labels.emplace(value.substr(0, equals), value.substr(equals + 1));
            } else {
                // A bare label is a label whose presence is the point - "gpu" rather than
                // "gpu=true" - and is kept as such rather than refused.
                labels.emplace(value, "true");
            }
        }
        return labels;
    }

}// namespace

int main(const int argc, char **argv) {

    po::options_description options("euclid-worker options");
    options.add_options()
            ("help,h", "show this help")
            ("endpoint,e", po::value<std::string>(), "gateway to reach euclid through, e.g. https://euclid.example:5566")
            ("node,n", po::value<std::string>(), "what this node calls itself; defaults to the host name")
            ("label,l", po::value<std::vector<std::string> >()->composing(), "a placement label, key=value; repeatable")
            ("data-dir,d", po::value<std::string>()->default_value("/var/lib/euclid-worker"), "where artifacts, credentials and logs go")
            ("tick,t", po::value<long>()->default_value(10), "seconds between renewals")
            ("ca-cert", po::value<std::string>()->default_value(""), "a CA certificate to trust in addition to the system store")
            ("config,c", po::value<std::string>()->default_value(""), "configuration file");

    po::variables_map vm;
    try {
        po::store(po::command_line_parser(argc, argv).options(options).run(), vm);
        po::notify(vm);
    } catch (const po::error &ex) {
        std::cerr << "error: " << ex.what() << "\n\n"
                  << options << std::endl;
        return 2;
    }

    if (vm.contains("help")) {
        std::cout << options << std::endl;
        return 0;
    }

    if (!vm["config"].as<std::string>().empty()) {
        try {
            Euclid::Core::Configuration::instance().load(vm["config"].as<std::string>());
        } catch (const std::exception &e) {
            std::cerr << "error: could not load the configuration: " << e.what() << std::endl;
            return 1;
        }
    }

    // A worker must not hold the installation's signing secret, and this is what makes that a
    // guarantee rather than a convention - docs/worker-nodes.md §10 step 6: "It is not optional,
    // and a worker must refuse to start without it."
    //
    // Anything holding euclid.modules.eam.jwt-secret can mint a bearer token for any principal in
    // the installation - including admin - offline, with no call to EAM and nothing in any audit
    // trail. A worker runs on a host the control plane does not own, and often one an operator
    // trusts less than the manager's. Distributing the secret there would make every worker host a
    // full compromise of the installation, and no amount of care elsewhere would get that back.
    //
    // Refused on the key's *presence*, whatever its value, because the realistic way it arrives is
    // somebody copying the manager's configuration file onto a worker host - and that file holds
    // far more than this. The check is cheap and the failure it prevents is unrecoverable.
    //
    // There is nothing to configure instead: a worker asks the master for each instance's
    // credentials through eap:issue-instance-credentials, and the master mints them.
    if (Euclid::Core::Configuration::instance().has("euclid.modules.eam.jwt-secret")) {
        std::cerr << "error: this configuration contains euclid.modules.eam.jwt-secret.\n"
                  << "       A worker must never hold the installation's signing secret: anything holding it\n"
                  << "       can mint a token for any principal, including admin, offline and unaudited.\n"
                  << "       Remove the key - a worker asks the master for each instance's credentials.\n"
                  << "       This usually means the manager's configuration file was copied here; it holds\n"
                  << "       more than this one secret, so check the rest of it too." << std::endl;
        return 1;
    }

    // The credentials this worker signs with, from the same file euclid-cli writes. A worker is a
    // euclid client and is logged in the way any other is: there is no separate worker identity
    // mechanism, which is what keeps its role, its grants and its audit trail ordinary.
    //
    // Crucially *not* the signing secret. A worker holding HttpActionServer::JwtSecret() could mint
    // a token for any principal in the installation, including admin, offline and unloggably - so
    // it asks the master for each instance's credentials instead. worker-nodes.md §3.2.
    const auto credentials = Euclid::CLI::Credentials::Load();
    if (!credentials.has_value() || credentials->token.empty()) {
        std::cerr << "error: not logged in - run 'euclid-cli eam login' as this node's principal first" << std::endl;
        return 1;
    }

    Euclid::Worker::Options workerOptions;
    workerOptions.endpoint = vm.contains("endpoint")
                                     ? vm["endpoint"].as<std::string>()
                                     : Euclid::Core::Configuration::instance().getOr<std::string>("euclid.worker.endpoint", "");
    if (workerOptions.endpoint.empty()) {
        std::cerr << "error: --endpoint is required (or euclid.worker.endpoint in the configuration)" << std::endl;
        return 1;
    }

    workerOptions.nodeName = vm.contains("node") ? vm["node"].as<std::string>() : Euclid::Core::SystemUtils::GetHostName();
    workerOptions.dataDir = vm["data-dir"].as<std::string>();
    workerOptions.tick = std::chrono::seconds{std::max(1L, vm["tick"].as<long>())};
    workerOptions.caCertPath = vm["ca-cert"].as<std::string>();
    if (vm.contains("label")) workerOptions.labels = parseLabels(vm["label"].as<std::vector<std::string> >());

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    Euclid::Worker::WorkerClient worker(workerOptions, *credentials);

    if (!worker.Register().has_value()) {
        std::cerr << "error: could not register with the master" << std::endl;
        return 1;
    }

    log_info << "euclid-worker running, node: " << workerOptions.nodeName
             << ", endpoint: " << workerOptions.endpoint << ", tick: " << workerOptions.tick.count() << "s";

    // What the worker knows between ticks. The lease deadline is the master's figure held locally
    // and compared against a local clock, and it is deliberately not re-asked when a renewal
    // fails: the deadline a worker acts on has to be one it already has, or a worker that cannot
    // reach the master could never learn that it must stop.
    Euclid::Worker::Reconciler::State state;

    while (!g_shutdownRequested) {

        state.renewed = worker.Renew(state.assigned, state.leaseExpiresAt);
        state.running = worker.Running();

        const auto plan = Euclid::Worker::Reconciler::Decide(state, std::chrono::system_clock::now());
        worker.Apply(plan);

        // Credentials are replaced halfway through their life, on every tick, for the reason the
        // manager does the same for the applications it runs: an instance started an hour ago is
        // holding a token that is about to expire, and rewriting the file is the only thing that
        // can replace it without restarting the process.
        //
        // After Apply, so an instance started on this tick is not asked twice; and not gated on
        // the renewal having succeeded, because an expiring token is a problem whether or not the
        // master answered the last poll - and if it cannot be reached, this fails harmlessly and
        // is tried again next tick with half a lifetime still in hand.
        worker.RefreshCredentials();

        if (plan.leaseLost) {
            // Everything has been stopped. The worker stays up and keeps trying to renew rather
            // than exiting: the master will have re-placed this work, and when contact comes back
            // the next renewal says what - if anything - this node should be running now. Exiting
            // would need something outside to restart it to find that out.
            state.assigned.clear();
        }

        // Interruptible, in whole seconds: a worker asked to stop should not take a tick to
        // notice. Nothing here is sensitive to the granularity - the tick is ten seconds against
        // a forty-five second lease.
        for (auto slept = std::chrono::seconds{0}; slept < workerOptions.tick && !g_shutdownRequested; ++slept) {
            std::this_thread::sleep_for(std::chrono::seconds{1});
        }
    }

    // Asked to stop, which is not the same as having lost the lease: the master has not re-placed
    // this work, so the instances are stopped and reported rather than abandoned. An operator
    // draining a node first is what avoids the gap this leaves.
    log_info << "euclid-worker stopping, node: " << workerOptions.nodeName;
    worker.StopAll();
    return 0;
}
