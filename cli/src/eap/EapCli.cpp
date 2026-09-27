// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// C++ includes
#include <filesystem>
#include <map>
#include <sstream>

// Euclid includes
#include <euclid/cli/eap/EapCli.h>
#include <euclid/core/ApplicationManifest.h>
#include <euclid/cli/esm/EsmCli.h>
#include <euclid/core/CryptoUtils.h>
#include <euclid/database/entity/eap/Application.h>

namespace Euclid::CLI {

    namespace po = boost::program_options;

    namespace {

        // Splits "KEY=value,OTHER=value" into a JSON object. Environment variables are a map on
        // the wire but a single argument on the command line, and only the first '=' separates
        // name from value - the value may well contain more of them.
        boost::json::object SplitEnvironment(const std::string &value) {
            boost::json::object environment;
            std::stringstream ss(value);
            for (std::string part; std::getline(ss, part, ',');) {
                const auto first = part.find_first_not_of(" \t");
                if (first == std::string::npos) continue;
                const auto equals = part.find('=', first);
                if (equals == std::string::npos) continue;
                environment[part.substr(first, equals - first)] = part.substr(equals + 1);
            }
            return environment;
        }

        boost::json::array SplitList(const std::string &value) {
            boost::json::array entries;
            std::stringstream ss(value);
            for (std::string part; std::getline(ss, part, ',');) {
                const auto first = part.find_first_not_of(" \t");
                const auto last = part.find_last_not_of(" \t");
                if (first == std::string::npos) continue;
                entries.push_back(boost::json::string(part.substr(first, last - first + 1)));
            }
            return entries;
        }

        // The objects a manifest names, split the way create-application and update-application
        // take them. Both halves count: an application reaches what it owns and what it borrows,
        // and a grant that named only the first would refuse it the second.
        //
        // This is what turns a manifest into a narrower principal. Without it EAP grants the
        // application role with resources = ["*"], because a deployment that says nothing about
        // what it needs can only safely be read as needing everything in its namespace.
        bool addManifestResources(const std::filesystem::path &directory, boost::json::object &request) {

            const auto loaded = Core::LoadApplicationManifest(directory);
            if (!loaded.ok()) {
                std::cerr << "error: " << directory.string() << " cannot be read:\n";
                for (const auto &problem: loaded.errors) std::cerr << "  " << problem << "\n";
                return false;
            }

            boost::json::array buckets;
            boost::json::array queues;
            boost::json::array topics;

            const auto add = [&](const Core::ApplicationManifest::Kind kind, const std::string &name) {
                switch (kind) {
                    case Core::ApplicationManifest::Kind::Bucket: buckets.push_back(boost::json::string(name)); break;
                    case Core::ApplicationManifest::Kind::Queue: queues.push_back(boost::json::string(name)); break;
                    case Core::ApplicationManifest::Kind::Topic: topics.push_back(boost::json::string(name)); break;
                }
            };

            for (const auto &declaration: loaded.manifest.creates) add(declaration.kind, declaration.name);
            for (const auto &declaration: loaded.manifest.uses) add(declaration.kind, declaration.name);

            if (buckets.empty() && queues.empty() && topics.empty()) {
                // A manifest that declares nothing would otherwise send three empty lists, and EAP
                // reads "no resources named" as "every resource in the account" - the opposite of
                // what a manifest is for. Better to leave the deployment's own lists alone.
                std::cerr << "warning: " << directory.string() << " declares no objects; the deployment's resources are unchanged\n";
                return true;
            }

            request["buckets"] = buckets;
            request["queues"] = queues;
            request["topics"] = topics;
            return true;
        }
    }// namespace

    EapCli::EapCli(std::string endpoint, Credentials::Entry authentication, const bool pretty, std::string caCertPath) : _endpoint(std::move(endpoint)), _authentication(std::move(authentication)), _pretty(pretty), _caCertPath(std::move(caCertPath)) {}

    /**
     * @brief Every action this module takes, with the one-line summary each is listed by.
     *
     * @par
     * A table rather than an initialiser inside the help branch, because two things read
     * it now: "help", and tab completion. An action listed in one and not the other is an
     * action somebody cannot find.
     */
    const std::vector<std::pair<std::string, std::string> > &EapCli::Actions() {
        static const std::vector<std::pair<std::string, std::string> > kActions = {
                {"create-application", "Define a new application from an artifact in an ESM bucket"},
                {"delete-application", "Delete an application definition"},
                {"list-applications", "List the defined applications and how many instances are running"},
                {"get-application", "Show one application's definition"},
                {"redeploy-application", "Deploy a new build of an application from a local file"},
                {"restart-application", "Ask the manager to start an application's instances again"},
                {"start-application", "Ask the manager to start an application"},
                {"stop-application", "Ask the manager to stop an application"},
                {"set-log-level", "Turn an application's own logging up, down or off"},
                {"update-application", "Change an existing application's definition"},
                {"copy-application", "Define the same application again in another namespace"},
                {"scale-application", "Change how many instances an application runs, without restarting it"},
                {"apply", "Create and check the objects an application's euclid/ manifest declares"},
        };
        return kActions;
    }
    int EapCli::process(const std::string &action, const std::vector<std::string> &args) const {
        if (action == "help" || action == "--help" || action == "-h") {
            return PrintModuleHelp("eap", Actions());
        }

        // Every eap action decides which code euclid executes and under whose identity, so all of
        // them are administrator-only. Checked here, once, ahead of dispatch - a client-side
        // fail-fast for a better error message only, not the security boundary: the server
        // re-checks every request regardless of what an older CLI binary sends. Help is exempt,
        // since usage text gives nothing away.
        if (!IsHelpRequest(args)) {
            if (_authentication.token.empty()) {
                std::cerr << "error: " << action << " failed: not authenticated; run 'euclid-cli eam login' again\n";
                return 1;
            }
            if (!_authentication.isAdmin) {
                std::cerr << "error: " << action << " failed: administrator privileges required\n";
                return 1;
            }
        }

        if (action == "create-application") return createApplication(args);
        if (action == "update-application") return updateApplication(args);
        if (action == "copy-application") return copyApplication(args);
        if (action == "scale-application") return scaleApplication(args);
        if (action == "redeploy-application") return redeployApplication(args);
        if (action == "list-applications") return listApplications(args);
        if (action == "get-application") return getApplication(args);
        if (action == "delete-application") return deleteApplication(args);
        if (action == "start-application") return setState(args, true);
        if (action == "stop-application") return setState(args, false);
        if (action == "restart-application") return restartApplication(args);
        if (action == "set-log-level") return setLogLevel(args);
        if (action == "apply") return applyManifest(args);

        std::cerr << "error: unknown eap action '" << action << "'\n";
        return 1;
    }

    int EapCli::createApplication(const std::vector<std::string> &args) const {
        po::options_description desc("create a new application");
        desc.add_options()
                ("application-id,n", po::value<std::string>()->required(), "name identifying the application, unique across the installation")
                ("runtime,r", po::value<std::string>()->required(), "runtime the artifact is started with: JAVA, JAVA21, JAVA25, PYTHON, NODEJS or BINARY")
                ("bucket,b", po::value<std::string>()->required(), "name of the ESM bucket holding the artifact")
                ("artifact,a", po::value<std::string>()->required(), "object key of the artifact within that bucket")
                ("version", po::value<std::string>(), "version of this build; read out of the artifact name (x.y.z) when not given")
                ("user,u", po::value<std::string>(), "EAM user the application runs as; defaults to a technical principal created for this application alone")
                ("buckets", po::value<std::string>(), "comma-separated names of the ESM buckets the application may use; empty means every bucket in its account")
                ("manifest", po::value<std::string>(), "an application euclid/ directory; the objects it declares become the resources this application is granted, instead of every resource in its namespace")
                ("queues", po::value<std::string>(), "comma-separated names of the EQS queues the application may use; empty means every queue in its account")
                ("command,c", po::value<std::string>(), "command to run instead of the runtime's default")
                ("arguments", po::value<std::string>(), "comma-separated arguments passed after the artifact")
                ("environment,e", po::value<std::string>(), "comma-separated KEY=value environment variables")
                ("min-instances", po::value<long>(), "smallest number of instances the autoscaler keeps running (default 1)")
                ("max-instances", po::value<long>(), "largest number of instances the autoscaler may scale out to (default 1)")
                ("ready-timeout", po::value<long>(), "kept on the definition but no longer decides readiness: an application counts as started by surviving its own startup, not by creating a socket");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eap", "create-application",
                                   "--application-id <name> --runtime <runtime> --bucket <bucket> --artifact <key> [--version <x.y.z>] "
                                   "[--user <user>] [--buckets <list>] [--queues <list>] [--command <cmd>] "
                                   "[--arguments <list>] [--environment <list>] [--min-instances <n>] "
                                   "[--max-instances <n>] [--ready-timeout <ms>]",
                                   "Defines a new application from an artifact already stored in an ESM bucket - upload it first with "
                                   "\"esm upload-file\", or through a transfer server. The manager copies the artifact to the host, "
                                   "starts it with the runtime's interpreter (java -jar, python3, node) or directly for BINARY, and "
                                   "scales it between --min-instances and --max-instances like any other module. An instance counts as "
                                   "started once it is still running a moment after it was spawned, so an application that only does "
                                   "work of its own is never killed for failing to answer a convention it was not written for; a socket "
                                   "path is still handed to it in EUCLID_SOCKET for one that does want to serve x-euclid-action requests. "
                                   "Its endpoint and credentials arrive the same way - EUCLID_BASE_URL, and a rotating token in the file "
                                   "named by EUCLID_CREDENTIALS_FILE - which it uses to sign its own calls back into euclid (RFC 9421). "
                                   "Without --user the application gets "
                                   "a technical principal of its own (\"app-<application-id>\"): an EAM identity that cannot log in and "
                                   "has nothing but that key, created here and deleted with the application, so no application ever runs "
                                   "on a person's credentials. Name --user only when an application really should act as an existing user. "
                                   "Naming --buckets and --queues narrows that principal to exactly those resources: the storage and "
                                   "queueing modules refuse anything else it asks for, so a compromised application reaches what it was "
                                   "deployed with and nothing more. Naming neither leaves it able to use everything in its account. "
                                   "The application is created stopped - use \"eap start-application\" to run it.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << std::endl << std::endl << desc << std::endl;
            return 1;
        }

        boost::json::object request{
                {"applicationId", vm["application-id"].as<std::string>()},
                {"runtime", vm["runtime"].as<std::string>()},
                {"bucket", vm["bucket"].as<std::string>()},
                {"artifact", vm["artifact"].as<std::string>()}
        };
        if (vm.contains("version")) request["version"] = vm["version"].as<std::string>();
        if (vm.contains("user")) request["user"] = vm["user"].as<std::string>();
        if (vm.contains("command")) request["command"] = vm["command"].as<std::string>();
        if (vm.contains("namespace")) request["namespace"] = vm["namespace"].as<std::string>();
        if (vm.contains("arguments")) request["arguments"] = SplitList(vm["arguments"].as<std::string>());
        if (vm.contains("environment")) request["environment"] = SplitEnvironment(vm["environment"].as<std::string>());
        if (vm.contains("buckets")) request["buckets"] = SplitList(vm["buckets"].as<std::string>());
        if (vm.contains("queues")) request["queues"] = SplitList(vm["queues"].as<std::string>());
        if (vm.contains("manifest") && !addManifestResources(vm["manifest"].as<std::string>(), request)) return 1;
        if (vm.contains("min-instances")) request["minInstances"] = vm["min-instances"].as<long>();
        if (vm.contains("max-instances")) request["maxInstances"] = vm["max-instances"].as<long>();
        if (vm.contains("ready-timeout")) request["readyTimeoutMs"] = vm["ready-timeout"].as<long>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("eap", "create-application", request);
            if (!response.IsSuccess()) {
                std::cerr << "error: create-application failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EapCli::updateApplication(const std::vector<std::string> &args) const {
        po::options_description desc("update an existing application");
        desc.add_options()
                ("application-id,n", po::value<std::string>()->required(), "name of the application to change")
                ("runtime,r", po::value<std::string>(), "runtime the artifact is started with")
                ("artifact,a", po::value<std::string>(), "object key of the artifact within the application's bucket")
                ("command,c", po::value<std::string>(), "command to run instead of the runtime's default")
                ("user,u", po::value<std::string>(), "EAM user the application runs as; it has to exist and have an access key")
                ("namespace", po::value<std::string>(), "namespace the application's own requests run in, which is what lets it name a queue or topic rather than spell out a full ERN; pass an empty string to move it back to the account root")
                ("arguments", po::value<std::string>(), "comma-separated arguments; replaces the current list")
                ("environment,e", po::value<std::string>(), "comma-separated KEY=value environment variables; replaces the current set")
                ("buckets", po::value<std::string>(), "comma-separated bucket names the application may use; replaces the current list")
                ("manifest", po::value<std::string>(), "an application euclid/ directory; the objects it declares become the resources this application is granted, instead of every resource in its namespace")
                ("queues", po::value<std::string>(), "comma-separated queue names the application may use; replaces the current list")
                ("min-instances", po::value<long>(), "smallest number of instances the autoscaler keeps running")
                ("max-instances", po::value<long>(), "largest number of instances the autoscaler may scale out to")
                ("ready-timeout", po::value<long>(), "kept on the definition but no longer decides readiness: an application counts as started by surviving its own startup, not by creating a socket");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eap", "update-application",
                                   "--application-id <name> [--runtime <runtime>] [--artifact <key>] [--command <cmd>] "
                                   "[--arguments <list>] [--environment <list>] [--buckets <list>] [--queues <list>] "
                                   "[--min-instances <n>] [--max-instances <n>] [--ready-timeout <ms>] [--user <userId>]",
                                   "Changes an existing application's definition. Only the options actually given are altered, so one "
                                   "setting can be changed without resending the whole definition; --arguments and --environment "
                                   "replace the current values rather than adding to them. A running application is restarted onto the "
                                   "new definition by the manager's reconciler within a few seconds - an artifact, a command, an "
                                   "environment and the credentials a process was handed are all decided when it starts, so there is no "
                                   "other way to apply them. That also makes this the way to deploy a new build: upload it over the same "
                                   "key with \"esm upload-file\" and run this command, which re-materialises the artifact even when "
                                   "nothing about the definition changed. "
                                   "Changing --buckets or --queues re-grants the application's technical principal, so the "
                                   "new list takes effect immediately, for running instances too. "
                                   "--user points the application at a different EAM identity, which has to exist and hold an access "
                                   "key - the repair for a definition naming a principal that was deleted or renamed, which otherwise "
                                   "runs and is refused everything it calls. If the application was running as a technical principal EAP "
                                   "issued it, that principal and its grants are deleted with the change: it was made for this "
                                   "application and nothing else can use it.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << std::endl << std::endl << desc << std::endl;
            return 1;
        }

        boost::json::object request{{"applicationId", vm["application-id"].as<std::string>()}};
        if (vm.contains("runtime")) request["runtime"] = vm["runtime"].as<std::string>();
        if (vm.contains("artifact")) request["artifact"] = vm["artifact"].as<std::string>();
        if (vm.contains("command")) request["command"] = vm["command"].as<std::string>();
        if (vm.contains("user")) request["user"] = vm["user"].as<std::string>();
        if (vm.contains("arguments")) request["arguments"] = SplitList(vm["arguments"].as<std::string>());
        if (vm.contains("environment")) request["environment"] = SplitEnvironment(vm["environment"].as<std::string>());
        if (vm.contains("buckets")) request["buckets"] = SplitList(vm["buckets"].as<std::string>());
        if (vm.contains("queues")) request["queues"] = SplitList(vm["queues"].as<std::string>());
        if (vm.contains("manifest") && !addManifestResources(vm["manifest"].as<std::string>(), request)) return 1;
        if (vm.contains("min-instances")) request["minInstances"] = vm["min-instances"].as<long>();
        if (vm.contains("max-instances")) request["maxInstances"] = vm["max-instances"].as<long>();
        if (vm.contains("ready-timeout")) request["readyTimeoutMs"] = vm["ready-timeout"].as<long>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("eap", "update-application", request);
            if (!response.IsSuccess()) {
                std::cerr << "error: update-application failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EapCli::copyApplication(const std::vector<std::string> &args) const {
        po::options_description desc("copy an application into another namespace");
        desc.add_options()
                ("application-id,n", po::value<std::string>()->required(), "name of the application to copy")
                ("to-namespace,t", po::value<std::string>()->required(), "namespace to copy it into")
                ("as", po::value<std::string>(), "name the copy is defined under; the original's name unless given");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eap", "copy-application",
                                   "--application-id <name> --to-namespace <namespace> [--as <name>]",
                                   "Defines the same application again in another namespace, leaving the original alone and running - "
                                   "which is how a build is promoted from development to integration to production without taking the "
                                   "namespace it came from out of service. Use \"eap update-application --namespace\" instead to move an "
                                   "application rather than copy it. "
                                   "The copy runs the same artifact: bucket, object key and checksum are taken as they stand, so it is "
                                   "the same bytes and not a rebuild that happens to share a version. "
                                   "It is given its own runtime name and its own technical principal with its own access key, because "
                                   "both are installation-wide and cannot be shared - so revoking the copy's credentials leaves the "
                                   "original running. An application told to run as a named user keeps that user. "
                                   "What it may reach is re-resolved rather than copied: a bucket or queue ERN carries the namespace it "
                                   "was resolved in, so copying the list would point the new application at the old namespace's data. "
                                   "The same names are looked up in the target namespace instead, and a name that does not exist there "
                                   "fails the copy rather than quietly leaving the application with less access than the original. "
                                   "The copy is created stopped, whatever the original is doing - start it with \"eap start-application\" "
                                   "once you have looked at it.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << std::endl << std::endl << desc << std::endl;
            return 1;
        }

        boost::json::object request{
                {"applicationId", vm["application-id"].as<std::string>()},
                {"targetNamespace", vm["to-namespace"].as<std::string>()},
        };
        if (vm.contains("as")) request["targetApplicationId"] = vm["as"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("eap", "copy-application", request);
            if (!response.IsSuccess()) {
                std::cerr << "error: copy-application failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EapCli::scaleApplication(const std::vector<std::string> &args) const {
        po::options_description desc("change how many instances an application runs");
        desc.add_options()
                ("application-id,n", po::value<std::string>()->required(), "name of the application to scale")
                ("min-instances", po::value<long>(), "smallest number of instances to keep running; left alone if not given")
                ("max-instances", po::value<long>(), "largest number the autoscaler may run; left alone if not given")
                ("instances,i", po::value<long>(), "run exactly this many: sets both bounds, which pins the pool and leaves the autoscaler no room");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eap", "scale-application",
                                   "--application-id <name> [--instances <n>] [--min-instances <n>] [--max-instances <n>]",
                                   "Changes how many instances an application runs, without restarting the ones it already has. "
                                   "\"eap update-application\" can set the same two fields, but it writes the whole definition and "
                                   "stamps the modification date - and the manager restarts a pool whose application changed since it "
                                   "started it. Scaling that way therefore stops every running instance and starts it again, which is "
                                   "the opposite of what asking for more capacity means and worst at the moment somebody asks for it. "
                                   "What is set is the range the autoscaler works within, not a count: the manager scales toward it "
                                   "within a few seconds, adding instances one at a time and stopping idle ones as the load allows. "
                                   "--instances is shorthand for setting both bounds to the same number, which pins the pool at that "
                                   "size and leaves the autoscaler nothing to decide - useful to hold an application steady, and worth "
                                   "undoing afterwards. A bound not named is left as it stands, so a ceiling can be raised without "
                                   "touching the floor; the two are checked against each other, so a floor cannot be left above a "
                                   "ceiling. A floor of zero is refused: an application desired RUNNING with no instances reads "
                                   "everywhere as a pool that has failed - use \"eap stop-application\" to take one out of service.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << std::endl << std::endl << desc << std::endl;
            return 1;
        }

        if (vm.contains("instances") && (vm.contains("min-instances") || vm.contains("max-instances"))) {
            std::cerr << "error: --instances sets both bounds, so it cannot be combined with --min-instances or --max-instances" << std::endl;
            return 1;
        }
        if (!vm.contains("instances") && !vm.contains("min-instances") && !vm.contains("max-instances")) {
            std::cerr << "error: nothing to change - give --instances, --min-instances or --max-instances" << std::endl;
            return 1;
        }

        boost::json::object request{{"applicationId", vm["application-id"].as<std::string>()}};
        if (vm.contains("instances")) {
            const auto instances = vm["instances"].as<long>();
            request["minInstances"] = instances;
            request["maxInstances"] = instances;
        } else {
            if (vm.contains("min-instances")) request["minInstances"] = vm["min-instances"].as<long>();
            if (vm.contains("max-instances")) request["maxInstances"] = vm["max-instances"].as<long>();
        }

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("eap", "scale-application", request);
            if (!response.IsSuccess()) {
                std::cerr << "error: scale-application failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EapCli::redeployApplication(const std::vector<std::string> &args) const {
        po::options_description desc("deploy a new build of an application");
        desc.add_options()
                ("application-id,n", po::value<std::string>()->required(), "name of the application to deploy")
                ("file,f", po::value<std::string>()->required(), "the new build: the jar for JAVA, the script for PYTHON/NODEJS, the executable for BINARY")
                ("artifact,a", po::value<std::string>(), "object key to store it under; defaults to the key the application already uses")
                ("version", po::value<std::string>(), "version this build is; read out of the file name (x.y.z) when not given")
                ("force", po::bool_switch(), "deploy even when the build is byte for byte the one already deployed");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eap", "redeploy-application", "--application-id <name> --file <path> [--artifact <key>] [--version <x.y.z>] [--force]",
                                   "Deploys a new build: uploads the local file into the application's own bucket, under the artifact "
                                   "key the application already uses, and stamps the definition so the manager restarts the running "
                                   "instances onto it - within a few seconds, since an artifact is decided when a process starts. "
                                   "The version is read out of the file name (\"orders-1.4.0.jar\" is 1.4.0) unless --version says "
                                   "otherwise; the same version can be deployed again, since a rebuilt snapshot keeps its number. "
                                   "What is refused is a build that is byte for byte the one already deployed: there would be nothing "
                                   "to deploy, and the restart would buy nothing. Checked here before the upload, so a refused redeploy "
                                   "leaves the bucket as it was, and again by the server. "
                                   "--force deploys it anyway, for when the same bytes are the point: an artifact that was deleted and "
                                   "is being put back, or a host that lost its copy of the build. The upload happens and the definition "
                                   "is stamped, so the manager restarts the pool onto it - which is the part that was wanted. "
                                   "Give --artifact to deploy under a different key, for a versioned name (\"orders-1.4.0.jar\"); the "
                                   "application is repointed at it. An application that is stopped stays stopped and comes up on the "
                                   "new build when it is started.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << std::endl << std::endl << desc << std::endl;
            return 1;
        }

        const auto applicationId = vm["application-id"].as<std::string>();
        const auto filePath = vm["file"].as<std::string>();

        // Checked before anything is asked of the server: a mistyped path is the likeliest thing
        // to be wrong here, and finding out after the definition was stamped would mean a restart
        // onto the build that is still there.
        if (std::error_code ec; !std::filesystem::is_regular_file(filePath, ec)) {
            std::cerr << "error: redeploy-application failed: '" << filePath << "' is not a file\n";
            return 1;
        }

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);

            // Where the build has to go is the application's own business, not something the
            // caller should have to look up and pass in correctly.
            const HttpResponse current = client.Post("eap", "get-application", boost::json::object{{"applicationId", applicationId}});
            if (!current.IsSuccess()) {
                std::cerr << "error: redeploy-application failed (HTTP " << current.statusCode << "): " << boost::json::serialize(current.body) << std::endl;
                return 1;
            }

            const auto &definition = current.body.as_object();
            const auto bucketErn = std::string(definition.at("bucketErn").as_string());
            const auto artifactKey = vm.count("artifact")
                                         ? vm["artifact"].as<std::string>()
                                         : std::string(definition.at("artifactKey").as_string());

            // What this build calls itself: what was asked for, else what the file's own name
            // says, else what the key says - "orders-1.4.0.jar" carries its version either way.
            auto version = vm.count("version")
                               ? vm["version"].as<std::string>()
                               : Database::Entity::EAP::VersionFromArtifactName(std::filesystem::path(filePath).filename().string());
            if (version.empty()) version = Database::Entity::EAP::VersionFromArtifactName(artifactKey);
            if (version.empty()) {
                std::cerr << "error: redeploy-application failed: no version in '" << std::filesystem::path(filePath).filename().string()
                        << "' (expected something like 1.4.0) - pass --version\n";
                return 1;
            }

            // The server refuses a redeploy that is not a new build, and would refuse this one
            // after the upload had already overwritten the artifact - leaving a build in the
            // bucket that was rejected but that the next restart would pick up anyway. So the same
            // two questions are asked here first, against the definition just read.
            const auto deployedVersion = definition.contains("version") ? std::string(definition.at("version").as_string()) : std::string();
            const auto deployedMd5 = definition.contains("md5Sum") ? std::string(definition.at("md5Sum").as_string()) : std::string();

            // Present, and not merely assumed to be: the checksum on this side was just taken over
            // the file about to be uploaded, so the bytes are in front of us. The server asks its
            // storage the same question, where the answer can be no.
            //
            // Not asked at all under --force: the answer is known and has been overridden, and
            // hashing the file only to ignore what it says costs a read of something that is
            // routinely hundreds of megabytes.
            const auto force = vm["force"].as<bool>();
            if (const auto refused = force ? std::string()
                                           : Database::Entity::EAP::RedeployRefusal(deployedVersion, deployedMd5, version,
                                                                                    Core::CryptoUtils::md5SumFile(filePath), true);
                !refused.empty()) {
                std::cerr << "error: refusing to redeploy '" << applicationId << "': " << refused
                        << "\npass --force to deploy it anyway, or use 'eap update-application --application-id " << applicationId
                        << " --artifact " << artifactKey << "'\n";
                return 1;
            }

            const EsmCli esm(_endpoint, _authentication, _pretty, _caCertPath);
            boost::json::value uploaded;
            if (esm.uploadOneFile(bucketErn, artifactKey, filePath, 0, 0, uploaded) != 0) {
                // uploadOneFile already said what went wrong, and the definition is untouched -
                // the application keeps running on the build it has.
                return 1;
            }

            // Recording the new version is what makes this a deployment rather than an upload: the
            // definition is stamped, and the manager reads that as a new revision and restarts the
            // pool onto the new artifact.
            //
            // The flag travels with it: the server asks the same question against what is in its
            // own storage, and the upload that just happened is exactly what it would refuse.
            const HttpResponse response = client.Post("eap", "redeploy-application",
                                                      boost::json::object{{"applicationId", applicationId}, {"artifact", artifactKey},
                                                                          {"version", version}, {"force", force}});
            if (!response.IsSuccess()) {
                std::cerr << "error: redeploy-application failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body)
                        << "\nthe new build was uploaded to " << artifactKey << "; run 'eap redeploy-application --application-id " << applicationId
                        << " --file " << filePath << "' again once that is resolved" << std::endl;
                return 1;
            }

            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;

        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EapCli::listApplications(const std::vector<std::string> &args) const {
        po::options_description desc("list applications");
        desc.add_options()
                ("prefix,p", po::value<std::string>(), "only list applications whose ID starts with this");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eap", "list-applications", "[--prefix <prefix>]",
                                   "Lists the defined applications: their runtime, artifact, autoscaler bounds, the state they should "
                                   "be in, and how many instances the manager currently has running.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << std::endl << std::endl << desc << std::endl;
            return 1;
        }

        boost::json::object request;
        if (vm.contains("prefix")) request["prefix"] = vm["prefix"].as<std::string>();

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("eap", "list-applications", request);
            if (!response.IsSuccess()) {
                std::cerr << "error: list-applications failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EapCli::getApplication(const std::vector<std::string> &args) const {
        po::options_description desc("show an application");
        desc.add_options()
                ("application-id,n", po::value<std::string>()->required(), "name of the application");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eap", "get-application", "--application-id <name>",
                                   "Shows one application's definition and how many instances of it are running.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << std::endl << std::endl << desc << std::endl;
            return 1;
        }

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("eap", "get-application", boost::json::object{{"applicationId", vm["application-id"].as<std::string>()}});
            if (!response.IsSuccess()) {
                std::cerr << "error: get-application failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EapCli::deleteApplication(const std::vector<std::string> &args) const {
        po::options_description desc("delete an application");
        desc.add_options()
                ("application-id,n", po::value<std::string>()->required(), "name of the application to delete");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eap", "delete-application", "--application-id <name>",
                                   "Deletes an application definition. Any instances the manager is running are stopped on its next "
                                   "reconcile - an application that no longer exists is not something it keeps running. The artifact "
                                   "in the bucket is left alone, but the technical principal created for the application - if it "
                                   "has one - is deleted with it, so no credential outlives what it was issued to.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << std::endl << std::endl << desc << std::endl;
            return 1;
        }

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("eap", "delete-application", boost::json::object{{"applicationId", vm["application-id"].as<std::string>()}});
            if (!response.IsSuccess()) {
                std::cerr << "error: delete-application failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EapCli::setState(const std::vector<std::string> &args, const bool start) const {
        const std::string action = start ? "start-application" : "stop-application";

        po::options_description desc(start ? "start an application" : "stop an application");
        desc.add_options()
                ("application-id,n", po::value<std::string>()->required(), "name of the application");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eap", action, "--application-id <name>",
                                   start
                                       ? "Records that this application should be running. The manager's reconciler picks that up "
                                       "within a few seconds, materialises the artifact from its bucket and starts it; from then "
                                       "on the autoscaler runs between the application's minimum and maximum instance counts."
                                       : "Records that this application should be stopped. The manager's reconciler stops every "
                                       "instance on its next pass. The definition and the artifact are left in place, so "
                                       "\"start-application\" brings it back as it was.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << std::endl << std::endl << desc << std::endl;
            return 1;
        }

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("eap", action, boost::json::object{{"applicationId", vm["application-id"].as<std::string>()}});
            if (!response.IsSuccess()) {
                std::cerr << "error: " << action << " failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EapCli::restartApplication(const std::vector<std::string> &args) const {

        po::options_description desc("restart an application");
        desc.add_options()
                ("application-id,n", po::value<std::string>()->required(), "name of the application");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eap", "restart-application", "--application-id <name>",
                                   "Records that this application's instances should be started again. The manager's reconciler "
                                   "stops the whole pool on its next pass and starts it straight back up from the current "
                                   "definition - the same thing it does after a redeploy, with nothing new to pick up.\n\n"
                                   "The application keeps running as far as its desired state is concerned, so nothing is left "
                                   "stopped if this command, or the connection carrying it, does not survive the restart. An "
                                   "application that is stopped is refused rather than started: use \"eap start-application\".\n\n"
                                   "Reach for this when an instance has to re-do what it does at startup - a listener that has to "
                                   "subscribe again, a cache read once - rather than to deploy anything: the artifact, the "
                                   "environment and the credentials all come back as they were. Use \"eap redeploy-application\" "
                                   "for a new build.\n\n"
                                   "The whole pool goes down and comes back, which is what makes it a restart rather than "
                                   "\"emm restart-module\" - that cycles a module's instances one per reconcile tick, so it keeps "
                                   "serving throughout and no two instances are down together.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << std::endl << std::endl << desc << std::endl;
            return 1;
        }

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("eap", "restart-application",
                                                      boost::json::object{{"applicationId", vm["application-id"].as<std::string>()}});
            if (!response.IsSuccess()) {
                std::cerr << "error: restart-application failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    int EapCli::setLogLevel(const std::vector<std::string> &args) const {
        po::options_description desc("set an application's log level");
        desc.add_options()
                ("application-id,n", po::value<std::string>()->required(), "name of the application")
                ("level,l", po::value<std::string>()->required(), "trace, debug, info, warning, error, fatal, off, or \"default\" to go back to the configured level");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eap", "set-log-level", "--application-id <name> --level <level>",
                                   "Sets the level an application's own output is logged at, and takes effect on the manager's "
                                   "next reconcile - within a few seconds, without restarting the application or interrupting "
                                   "anything it is doing. "
                                   "Whatever an application writes to standard output or standard error is read back by the "
                                   "manager and logged on a channel of its own, \"app.<application-id>\"; this is that channel's "
                                   "level. Standard error is recorded as an error and standard output as information, so "
                                   "--level error leaves what went wrong visible while silencing the rest, and --level off "
                                   "silences the application entirely. "
                                   "--level default takes the setting back, leaving the application under euclid.logging.channels. "
                                   "The level is stored with the application, so it survives a restart of the installation.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << std::endl << std::endl << desc << std::endl;
            return 1;
        }

        // "default" is the word for taking the setting back; the server reads an empty level that
        // way. Asking somebody to pass an empty string for it would be a worse interface.
        auto level = vm["level"].as<std::string>();
        if (level == "default") level.clear();

        // Checked here as well as on the server, for the error message rather than for the rule:
        // a typo is worth answering now instead of after a round trip that ends in a 400.
        if (!level.empty() && !Core::LogStream::CanonicalLevel(level).has_value()) {
            std::cerr << "error: not a log level: " << level << std::endl
                      << "       expected trace, debug, info, warning, error, fatal, off, or default" << std::endl;
            return 1;
        }

        try {
            const HttpClient client(_endpoint, _authentication, _caCertPath);
            const HttpResponse response = client.Post("eap", "set-log-level",
                                                      boost::json::object{{"applicationId", vm["application-id"].as<std::string>()}, {"level", level}});
            if (!response.IsSuccess()) {
                std::cerr << "error: set-log-level failed (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << std::endl;
                return 1;
            }
            Core::WriteJson(std::cout, response.body, _pretty);
            return 0;
        } catch (const std::exception &ex) {
            std::cerr << "error: " << ex.what() << std::endl;
            return 1;
        }
    }

    namespace {

        // The tag that says whose object this is. Written when a manifest is applied rather than
        // when the object is created, which is the difference that keeps euclid-spring's per-run
        // delivery queues - created by the listener container, declared by nobody - out of range of
        // a prune that is about declarations.
        constexpr auto kOwnerTag = "euclid:application";

        struct ModuleActions {
            std::string module;
            std::string lookupErn;
            std::string get;
            std::string create;
            std::string addTag;
        };

        ModuleActions actionsFor(const Core::ApplicationManifest::Kind kind) {
            switch (kind) {
                case Core::ApplicationManifest::Kind::Queue:
                    return {"eqs", "get-queue-ern", "get-queue", "create-queue", "add-queue-tag"};
                case Core::ApplicationManifest::Kind::Topic:
                    return {"ens", "get-topic-ern", "get-topic", "create-topic", "add-topic-tag"};
                case Core::ApplicationManifest::Kind::Bucket:
                default:
                    return {"esm", "get-bucket-ern", "get-bucket", "create-bucket", "add-bucket-tag"};
            }
        }

        // Three answers rather than two, for the reason Euclid::CLI::Exists has three: "the gateway
        // did not answer" is not "the object is not there", and applying a manifest on that reading
        // creates a second one.
        struct Lookup {
            bool known{};
            std::string ern;
            std::string problem;

            [[nodiscard]] bool found() const { return known && !ern.empty(); }
            [[nodiscard]] bool absent() const { return known && ern.empty(); }
        };

        Lookup lookupErn(const HttpClient &client, const Core::ApplicationManifest::Kind kind, const std::string &name) {

            const auto actions = actionsFor(kind);
            try {
                const HttpResponse response = client.Post(actions.module, actions.lookupErn, boost::json::object{{"name", name}});
                if (response.IsSuccess()) {
                    const auto *ern = response.body.is_object() ? response.body.as_object().if_contains("ern") : nullptr;
                    if (ern != nullptr && ern->is_string()) return {.known = true, .ern = std::string(ern->as_string())};
                    return {.known = true, .ern = {}};
                }
                if (response.statusCode == 404) return {.known = true, .ern = {}};

                return {.known = false, .problem = "HTTP " + std::to_string(response.statusCode) + ": " + boost::json::serialize(response.body)};

            } catch (const std::exception &ex) {
                return {.known = false, .problem = ex.what()};
            }
        }

        // The owner tag, out of whatever shape the module answers get-<thing> with. Read from the
        // JSON rather than through each module's response DTO: three modules, three wrappers, one
        // field, and a tool that had to know all three would need changing whenever any of them was
        // reshaped.
        const boost::json::object *findTags(const boost::json::value &value, const int depth = 0) {

            if (depth > 3 || !value.is_object()) return nullptr;
            const auto &object = value.as_object();

            if (const auto *tags = object.if_contains("tags"); tags != nullptr && tags->is_object()) return &tags->as_object();
            for (const auto &field: object) {
                if (const auto *found = findTags(field.value(), depth + 1)) return found;
            }
            return nullptr;
        }

        std::string ownerOf(const HttpClient &client, const Core::ApplicationManifest::Kind kind, const std::string &ern) {

            const auto actions = actionsFor(kind);
            try {
                const HttpResponse response = client.Post(actions.module, actions.get, boost::json::object{{"ern", ern}});
                if (!response.IsSuccess()) return {};

                const auto *tags = findTags(response.body);
                if (tags == nullptr) return {};

                const auto *owner = tags->if_contains(kOwnerTag);
                return owner != nullptr && owner->is_string() ? std::string(owner->as_string()) : std::string{};

            } catch (const std::exception &) {
                return {};
            }
        }

        // Every object of one kind this application owns, by the tag apply wrote. The list is how
        // pruning knows what it used to have: the manifest says what should exist now, and the
        // difference is what a previous version of the manifest created and this one does not
        // mention. Nothing untagged is ever in this list, so nothing an application did not declare
        // can be removed by it.
        std::vector<std::pair<std::string, std::string> > ownedObjects(const HttpClient &client,
                                                                      const Core::ApplicationManifest::Kind kind,
                                                                      const std::string &application, bool &known) {

            static const std::map<std::string, std::string> kListActions{
                    {"esm", "list-buckets"},
                    {"eqs", "list-queues"},
                    {"ens", "list-topics"},
            };
            static const std::map<std::string, std::string> kCollections{
                    {"esm", "buckets"},
                    {"eqs", "queues"},
                    {"ens", "topics"},
            };

            known = false;
            std::vector<std::pair<std::string, std::string> > owned;

            const auto actions = actionsFor(kind);
            try {
                const HttpResponse response = client.Post(actions.module, kListActions.at(actions.module),
                                                          boost::json::object{{"pageSize", 10000}, {"pageIndex", 0}});
                if (!response.IsSuccess() || !response.body.is_object()) return owned;

                const auto *collection = response.body.as_object().if_contains(kCollections.at(actions.module));
                if (collection == nullptr || !collection->is_array()) return owned;

                known = true;
                for (const auto &entry: collection->as_array()) {
                    if (!entry.is_object()) continue;
                    const auto &object = entry.as_object();

                    const auto *tags = object.if_contains("tags");
                    if (tags == nullptr || !tags->is_object()) continue;
                    const auto *owner = tags->as_object().if_contains(kOwnerTag);
                    if (owner == nullptr || !owner->is_string() || std::string(owner->as_string()) != application) continue;

                    const auto *name = object.if_contains("name");
                    const auto *ern = object.if_contains("ern");
                    if (name == nullptr || !name->is_string() || ern == nullptr || !ern->is_string()) continue;

                    owned.emplace_back(std::string(name->as_string()), std::string(ern->as_string()));
                }
            } catch (const std::exception &) {
                known = false;
            }
            return owned;
        }


    }// namespace

    int EapCli::applyManifest(const std::vector<std::string> &args) const {

        po::options_description desc("apply an application manifest");
        desc.add_options()
                ("application-id,n", po::value<std::string>()->required(), "the application these objects belong to; recorded as their owner")
                ("directory,d", po::value<std::string>()->default_value("euclid"), "the application's euclid/ directory")
                ("dry-run", po::bool_switch()->default_value(false), "say what would be done, and do none of it")
                ("no-prune", po::bool_switch()->default_value(false), "leave objects this application owns that the manifest no longer lists");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eap", "apply", "--application-id <name> [--directory <path>] [--dry-run]",
                                   "Reads an application's euclid/ directory and makes the installation match it: creates the "
                                   "queues, topics and buckets the application owns, and checks that the ones it says it uses "
                                   "are there. What it owns is tagged with its name, which is what lets a later run remove what "
                                   "the manifest no longer lists - and what stops it removing anything it does not own. "
                                   "Objects are created in the namespace of the current session. "
                                   "Everything is printed before anything is done, and --dry-run stops there. "
                                   "Exits 0 when the installation matches the manifest, 1 when it does not and could not be "
                                   "made to, and 2 when the question could not be asked at all - an expired session, an "
                                   "unreachable gateway. See docs/application-manifest.md.",
                                   desc);
        }

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(args).options(desc).run(), vm);
            po::notify(vm);
        } catch (const po::error &ex) {
            std::cerr << "error: " << ex.what() << "\n\n"
                      << desc << std::endl;
            return 1;
        }

        const auto application = vm["application-id"].as<std::string>();
        const auto directory = std::filesystem::path(vm["directory"].as<std::string>());
        const auto dryRun = vm["dry-run"].as<bool>();
        const auto prune = !vm["no-prune"].as<bool>();

        const auto loaded = Core::LoadApplicationManifest(directory);
        if (!loaded.ok()) {
            std::cerr << "error: " << directory.string() << " cannot be applied:\n";
            for (const auto &problem: loaded.errors) std::cerr << "  " << problem << "\n";
            return 1;
        }
        if (loaded.manifest.empty()) {
            std::cout << "Nothing declared in " << directory.string() << "; nothing to do.\n";
            return 0;
        }

        const auto nameSpace = _authentication.nameSpace.empty() ? std::string("(the session default)") : _authentication.nameSpace;
        std::cout << "Application '" << application << "' in namespace " << nameSpace << ":\n\n";

        const HttpClient client(_endpoint, _authentication, _caCertPath);

        struct Work {
            const Core::ApplicationManifest::Creates *declaration{};
            bool create{};
            std::string ern;
        };

        std::vector<Work> work;
        bool blocked = false;
        bool unanswerable = false;
        int adoptions = 0;

        // ── What this application owns ──────────────────────────────────────────────────────
        for (const auto &declaration: loaded.manifest.creates) {

            const auto kind = Core::ToString(declaration.kind);
            const auto found = lookupErn(client, declaration.kind, declaration.name);

            if (!found.known) {
                std::cout << "  ?       " << kind << " " << declaration.name << " - could not be looked up: " << found.problem << "\n";
                unanswerable = true;
                continue;
            }
            if (found.absent()) {
                std::cout << "  create  " << kind << " " << declaration.name << "  (" << declaration.source << ")\n";
                work.push_back({.declaration = &declaration, .create = true});
                continue;
            }

            if (const auto owner = ownerOf(client, declaration.kind, found.ern); owner == application) {
                std::cout << "  ok      " << kind << " " << declaration.name << " - already there\n";
            } else if (owner.empty()) {
                // Untagged, so nobody has claimed it: it predates the manifest, or was made by
                // hand. Claimed here rather than left alone, because an object an application owns
                // and cannot manage is a manifest that lies. Printed as its own verb precisely
                // because it is the one step that takes something over.
                std::cout << "  adopt   " << kind << " " << declaration.name << " - exists untagged, to be recorded as owned by " << application << "\n";
                work.push_back({.declaration = &declaration, .create = false, .ern = found.ern});
                adoptions++;
            } else {
                std::cout << "  CLASH   " << kind << " " << declaration.name << " - already owned by '" << owner << "'\n";
                blocked = true;
            }
        }

        // ── What it borrows ─────────────────────────────────────────────────────────────────
        for (const auto &declaration: loaded.manifest.uses) {

            const auto kind = Core::ToString(declaration.kind);
            const auto found = lookupErn(client, declaration.kind, declaration.name);

            if (!found.known) {
                std::cout << "  ?       " << kind << " " << declaration.name << " - could not be looked up: " << found.problem << "\n";
                unanswerable = true;
                continue;
            }
            if (found.found()) {
                std::cout << "  use     " << kind << " " << declaration.name << " as " << Core::ToString(declaration.access) << "\n";
                continue;
            }

            // Not created here: the producer owns it, so a missing one is a deployment-order
            // problem. Naming the application that should have made it is the difference between a
            // fix and a support call.
            std::cout << "  MISSING " << kind << " " << declaration.name << " - used but does not exist"
                      << (declaration.owner.empty() ? "" : "; created by '" + declaration.owner + "'") << "\n";
            blocked = true;
        }

        // ── What it used to own and no longer declares ──────────────────────────────────────
        //
        // The manifest is the desired state, so an object this application owns and no longer
        // mentions is one a previous version of the manifest created. Only ever objects carrying
        // this application's owner tag: nothing untagged, and nothing another application's, can be
        // reached from here however the manifest is edited.
        struct Removal {
            Core::ApplicationManifest::Kind kind{};
            std::string name;
            std::string ern;
        };
        std::vector<Removal> removals;

        if (prune && !blocked && !unanswerable) {
            for (const auto kind: {Core::ApplicationManifest::Kind::Bucket,
                                   Core::ApplicationManifest::Kind::Queue,
                                   Core::ApplicationManifest::Kind::Topic}) {

                bool listed = false;
                const auto owned = ownedObjects(client, kind, application, listed);
                if (!listed) {
                    // Refusing to prune on a list that may be short is the only safe reading: what
                    // is missing from it looks exactly like an object the manifest still declares.
                    std::cout << "  ?       " << Core::ToString(kind) << "s could not be listed; nothing of this kind will be removed\n";
                    continue;
                }

                for (const auto &[name, ern]: owned) {
                    const auto declared = std::ranges::any_of(loaded.manifest.creates, [&](const auto &declaration) {
                        return declaration.kind == kind && declaration.name == name;
                    });
                    if (declared) continue;

                    std::cout << "  remove  " << Core::ToString(kind) << " " << name << " - owned here, no longer declared\n";
                    removals.push_back({.kind = kind, .name = name, .ern = ern});
                }
            }
        }

        std::cout << "\n";

        if (unanswerable) {
            std::cerr << "error: some objects could not be looked up; nothing was changed\n";
            return 2;
        }
        if (blocked) {
            std::cerr << "error: the manifest cannot be applied as it stands; nothing was changed\n";
            return 1;
        }
        if (dryRun) {
            std::cout << "--dry-run: nothing was changed.\n";
            return 0;
        }
        if (work.empty() && removals.empty()) {
            std::cout << "Already matches the manifest.\n";
            return 0;
        }

        // ── Do it ───────────────────────────────────────────────────────────────────────────
        int created = 0;

        for (auto &item: work) {

            const auto kind = Core::ToString(item.declaration->kind);
            const auto actions = actionsFor(item.declaration->kind);

            if (item.create) {
                // The name, plus whatever else the declaration said, passed through as written. The
                // rule about a visibility timeout lives in the module, so the module is what
                // rejects a value it does not like.
                boost::json::object body = item.declaration->settings;
                body["name"] = item.declaration->name;

                try {
                    const HttpResponse response = client.Post(actions.module, actions.create, body);
                    if (!response.IsSuccess()) {
                        std::cerr << "error: could not create " << kind << " " << item.declaration->name
                                  << " (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << "\n";
                        return 1;
                    }
                } catch (const std::exception &ex) {
                    std::cerr << "error: could not create " << kind << " " << item.declaration->name << ": " << ex.what() << "\n";
                    return 1;
                }

                // Asked for rather than read out of the answer: what a create returns differs by
                // module, and the tag below needs an ERN from all three.
                const auto found = lookupErn(client, item.declaration->kind, item.declaration->name);
                if (!found.found()) {
                    std::cerr << "error: created " << kind << " " << item.declaration->name
                              << " but could not resolve its ERN to record ownership\n";
                    return 1;
                }
                item.ern = found.ern;
                created++;
            }

            try {
                const HttpResponse response = client.Post(actions.module, actions.addTag,
                                                          boost::json::object{{"ern", item.ern}, {"key", kOwnerTag}, {"value", application}});
                if (!response.IsSuccess()) {
                    // The object is there either way; what is missing is the record of whose it is.
                    // Said plainly, because an untagged object is one the next run offers to adopt
                    // rather than one it can prune - which is the safe way round, but only if
                    // somebody knows.
                    std::cerr << "warning: " << kind << " " << item.declaration->name
                              << " exists but could not be tagged as owned by " << application
                              << " (HTTP " << response.statusCode << "): " << boost::json::serialize(response.body) << "\n";
                }
            } catch (const std::exception &ex) {
                std::cerr << "warning: " << kind << " " << item.declaration->name << " exists but could not be tagged: " << ex.what() << "\n";
            }
        }

        // ── Remove what is no longer declared ───────────────────────────────────────────────
        int removed = 0;
        std::vector<std::string> orphans;

        for (const auto &removal: removals) {

            const auto kind = Core::ToString(removal.kind);
            const auto actions = actionsFor(removal.kind);
            const auto deleteAction = removal.kind == Core::ApplicationManifest::Kind::Bucket ? "delete-bucket"
                                      : removal.kind == Core::ApplicationManifest::Kind::Queue ? "delete-queue"
                                                                                               : "delete-topic";

            try {
                // ifEmpty, always. An object this application owns may still hold a delivery
                // somebody is waiting for, and a line removed from a file is not a decision to
                // destroy it - see docs/application-manifest.md. A topic has no ifEmpty because it
                // holds nothing: what it has is subscriptions, and ENS refuses a topic that still
                // has them on its own.
                boost::json::object body{{"ern", removal.ern}};
                if (removal.kind != Core::ApplicationManifest::Kind::Topic) body["ifEmpty"] = true;

                const HttpResponse response = client.Post(actions.module, deleteAction, body);
                if (response.IsSuccess()) {
                    std::cout << "  removed " << kind << " " << removal.name << "\n";
                    removed++;
                    continue;
                }
                if (response.statusCode == 409) {
                    // Not a failure. It is the answer the rule asks for: still in use, so still
                    // here, and now somebody's to decide about.
                    orphans.push_back(kind + " " + removal.name + " - " + boost::json::serialize(response.body));
                    continue;
                }
                orphans.push_back(kind + " " + removal.name + " - HTTP " + std::to_string(response.statusCode) + ": " + boost::json::serialize(response.body));

            } catch (const std::exception &ex) {
                orphans.push_back(kind + " " + removal.name + " - " + ex.what());
            }
        }

        std::cout << "Created " << created << ", adopted " << adoptions << ", removed " << removed << ".\n";

        if (!orphans.empty()) {
            // Reported rather than counted away: these are objects nothing declares any more, which
            // nobody will look for again unless they are named here. They keep their owner tag, so
            // the next run offers them again.
            std::cout << "\nLeft in place, no longer declared by " << application << ":\n";
            for (const auto &orphan: orphans) std::cout << "  " << orphan << "\n";
        }
        return 0;
    }

}// namespace Euclid::CLI
