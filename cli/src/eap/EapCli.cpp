// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// C++ includes
#include <filesystem>
#include <sstream>

// Euclid includes
#include <euclid/cli/eap/EapCli.h>
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
                ("namespace", po::value<std::string>(), "namespace the application's own requests run in, which is what lets it name a queue or topic rather than spell out a full ERN; pass an empty string to move it back to the account root")
                ("arguments", po::value<std::string>(), "comma-separated arguments; replaces the current list")
                ("environment,e", po::value<std::string>(), "comma-separated KEY=value environment variables; replaces the current set")
                ("buckets", po::value<std::string>(), "comma-separated bucket names the application may use; replaces the current list")
                ("queues", po::value<std::string>(), "comma-separated queue names the application may use; replaces the current list")
                ("min-instances", po::value<long>(), "smallest number of instances the autoscaler keeps running")
                ("max-instances", po::value<long>(), "largest number of instances the autoscaler may scale out to")
                ("ready-timeout", po::value<long>(), "kept on the definition but no longer decides readiness: an application counts as started by surviving its own startup, not by creating a socket");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eap", "update-application",
                                   "--application-id <name> [--runtime <runtime>] [--artifact <key>] [--command <cmd>] "
                                   "[--arguments <list>] [--environment <list>] [--buckets <list>] [--queues <list>] "
                                   "[--min-instances <n>] [--max-instances <n>] [--ready-timeout <ms>]",
                                   "Changes an existing application's definition. Only the options actually given are altered, so one "
                                   "setting can be changed without resending the whole definition; --arguments and --environment "
                                   "replace the current values rather than adding to them. A running application is restarted onto the "
                                   "new definition by the manager's reconciler within a few seconds - an artifact, a command, an "
                                   "environment and the credentials a process was handed are all decided when it starts, so there is no "
                                   "other way to apply them. That also makes this the way to deploy a new build: upload it over the same "
                                   "key with \"esm upload-file\" and run this command, which re-materialises the artifact even when "
                                   "nothing about the definition changed. "
                                   "Changing --buckets or --queues re-grants the application's technical principal, so the "
                                   "new list takes effect immediately, for running instances too. The identity the application runs as "
                                   "cannot be changed here - delete it and create it again instead.",
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
        if (vm.contains("arguments")) request["arguments"] = SplitList(vm["arguments"].as<std::string>());
        if (vm.contains("environment")) request["environment"] = SplitEnvironment(vm["environment"].as<std::string>());
        if (vm.contains("buckets")) request["buckets"] = SplitList(vm["buckets"].as<std::string>());
        if (vm.contains("queues")) request["queues"] = SplitList(vm["queues"].as<std::string>());
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
                ("version", po::value<std::string>(), "version this build is; read out of the file name (x.y.z) when not given");

        if (IsHelpRequest(args)) {
            return PrintActionHelp("eap", "redeploy-application", "--application-id <name> --file <path> [--artifact <key>] [--version <x.y.z>]",
                                   "Deploys a new build: uploads the local file into the application's own bucket, under the artifact "
                                   "key the application already uses, and stamps the definition so the manager restarts the running "
                                   "instances onto it - within a few seconds, since an artifact is decided when a process starts. "
                                   "The version is read out of the file name (\"orders-1.4.0.jar\" is 1.4.0) unless --version says "
                                   "otherwise; the same version can be deployed again, since a rebuilt snapshot keeps its number. "
                                   "What is refused is a build that is byte for byte the one already deployed: there would be nothing "
                                   "to deploy, and the restart would buy nothing. Checked here before the upload, so a refused redeploy "
                                   "leaves the bucket as it was, and again by the server. "
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

            if (const auto refused = Database::Entity::EAP::RedeployRefusal(deployedVersion, deployedMd5, version,
                                                                            Core::CryptoUtils::md5SumFile(filePath));
                !refused.empty()) {
                std::cerr << "error: refusing to redeploy '" << applicationId << "': " << refused
                        << "\nuse 'eap update-application --application-id " << applicationId << " --artifact " << artifactKey
                        << "' to deploy it anyway\n";
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
            const HttpResponse response = client.Post("eap", "redeploy-application",
                                                      boost::json::object{{"applicationId", applicationId}, {"artifact", artifactKey}, {"version", version}});
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

}// namespace Euclid::CLI
