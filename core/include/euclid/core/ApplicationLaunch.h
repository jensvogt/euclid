// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

// Boost includes
#include <boost/json.hpp>
#include <boost/log/trivial.hpp>

namespace Euclid::Core {

    /**
     * @brief What starting an application on a host takes, once: the command line, whether the
     * artifact on disk is the one to run, and the credentials file the process reads.
     *
     * @par Why this is one place
     * Two things start applications - the manager for the ones it runs itself, and euclid-wrk for
     * the ones placed on a node - and until this existed each had its own copy of every rule here.
     * The copies drifted the way copies do: the worker started PYTHON and NODEJS as jars, never made
     * a BINARY executable, kept running the old build after a redeploy, fetched every artifact in
     * one call whatever its size, and named the credentials file differently. An application has to
     * start the same way whichever host it lands on, and the only reliable way to keep that true is
     * for there to be one rule. See docs/worker-nodes.md §13.
     *
     * @par What is deliberately not here
     * Anything that needs the database or the signing secret's holder to decide: which user an
     * application runs as, which namespace it resolves names in, its environment. Those are the
     * master's, and a worker is told them. What is here is what both sides do with the answer -
     * which is also what lets it live in core, which a worker links and the database does not.
     *
     * @author jensvogt47\@gmail.com
     */
    namespace Launch {

        /**
         * @brief Where a host keeps the interpreter for one runtime.
         *
         * @par
         * Called with the runtime's key - "java", "java21", "java25", "python", "nodejs" - and the
         * interpreter to use when the host was told nothing. Each side reads its own configuration
         * through it: the manager `euclid.modules.eap.runtimes.<key>`, a worker
         * `euclid.worker.runtimes.<key>`. Which configuration is not this rule's business; what to
         * do with the answer is.
         */
        using Interpreter = std::function<std::string(const std::string &key, const std::string &fallback)>;

        /**
         * @brief The configuration key a runtime's interpreter is named under: its name in lower
         * case, or empty for BINARY and anything unknown, which have no interpreter.
         */
        [[nodiscard]] std::string RuntimeKey(const std::string &runtime);

        /**
         * @brief Whether an application of this type runs to completion.
         *
         * @par
         * Takes the type as the word it arrives as, for the same reason InterpreterPrefix() takes
         * the runtime that way: a worker links this library and not the database one, so it cannot
         * see Entity::EAP::ApplicationType and has only the string the master sent. One definition
         * of what "JOB" means, rather than a literal compared in two places that could drift.
         *
         * @par
         * Anything else - including the empty string a master too old to send the field leaves -
         * is long-running. That is the safe direction: being wrong this way restarts an
         * application that need not have been, where the other way a service quietly stops being
         * restarted at all.
         *
         * @param type the application's type as EAP spells it, e.g. "JOB" or "PROCESS".
         * @return true only for a job.
         */
        [[nodiscard]] bool IsJob(const std::string &type);

        /**
         * @brief The interpreter a runtime's artifact is handed to, and the arguments before it.
         *
         * @par
         * java -jar for the three Java runtimes, python3 for PYTHON, node for NODEJS, nothing for
         * BINARY. "java21" and "java25" are not names any distribution ships, deliberately: a host
         * asked for a version it was never told the location of fails to exec with the version in
         * the message, rather than starting the jar under some other JDK.
         *
         * @par
         * Only the interpreter is ever configurable, never the arguments - "-jar" is how a jar is
         * started, not a preference, and a host that could change it could produce a command line
         * that cannot work.
         *
         * @param runtime the runtime's name, as EAP stores it ("JAVA21", "BINARY", ...).
         * @return the interpreter and its leading arguments; empty when the artifact is its own
         * command.
         */
        [[nodiscard]] std::vector<std::string> InterpreterPrefix(const std::string &runtime);

        /**
         * @brief The command line an application is started with: executable first.
         *
         *   - a command the application names is the executable, the artifact its first argument,
         *     and overrides the runtime;
         *   - otherwise the runtime's interpreter, from @p interpreter, then its leading arguments,
         *     then the artifact;
         *   - BINARY and anything unknown is the artifact itself.
         *
         * The application's arguments follow in every case.
         *
         * @return the command line; never empty.
         */
        [[nodiscard]] std::vector<std::string> CommandLine(const std::string &runtime, const std::string &command,
                                                           const std::string &artifact,
                                                           const std::vector<std::string> &arguments,
                                                           const Interpreter &interpreter);

        /**
         * @brief One runtime this host is configured for but could not actually start.
         */
        struct UnusableRuntime {
            /** @brief The runtime's key, as it is named in the configuration: "java25". */
            std::string runtime;

            /** @brief What it resolves to here, which is the runtime's own name when nothing named a path. */
            std::string command;

            /** @brief Why it cannot be run, as a phrase to put after @ref command in a message. */
            std::string reason;
        };

        /**
         * @brief The runtimes this host cannot start, for saying so before an application needs one.
         *
         * @par
         * An interpreter nobody configured is not found out until an application is started with it,
         * and then only as exit 127 - a message about a runtime arriving minutes or days after the
         * mistake, on whichever host happened to be given the application. Asked at startup instead,
         * the same mistake is a line in the log of the host that has it, next to everything else
         * that host got told.
         *
         * @par
         * An interpreter named rather than located - "python3", "node", and the "java" default - is
         * looked for on the PATH, because that is where a host that configured nothing keeps them and
         * finding one there is not a misconfiguration. Anything holding a separator is a path, and is
         * checked as one.
         *
         * @par
         * Says nothing about BINARY, which is its own command and has no interpreter to name.
         *
         * @param interpreter how this host resolves a runtime's key - see Interpreter.
         * @return one entry per unusable runtime, empty when every one of them can be started.
         */
        [[nodiscard]] std::vector<UnusableRuntime> UnusableRuntimes(const Interpreter &interpreter);

        /**
         * @brief Whether the file on disk is the object, byte for byte, so nothing needs fetching.
         *
         * @par
         * By content rather than size, because a rebuild landing on the same byte count - a changed
         * string literal of equal length is enough - would otherwise go on running the previous
         * build with everything about the deployment looking correct. Size is the fallback for an
         * object stored before its md5 was recorded; neither known means fetch.
         *
         * @param path the local copy.
         * @param md5Sum the object's content hash as ESM recorded it.
         * @param size the object's size as ESM recorded it.
         */
        [[nodiscard]] bool ArtifactIsCurrent(const std::filesystem::path &path, const std::string &md5Sum, long size);

        /**
         * @brief Makes a fetched artifact runnable, if it is what gets exec'd.
         *
         * @par
         * A BINARY artifact arrives as a plain object with no mode bits worth speaking of. Nothing
         * to do for anything else, or for a BINARY that names its own command - the command is
         * what runs, and the artifact is its argument. A no-op on Windows, which has no exec bit.
         */
        void PrepareArtifact(const std::filesystem::path &path, const std::string &runtime, const std::string &command);

        /**
         * @brief The TCP port an instance's own HTTP listener is given, from a range the host set
         * aside for applications; 0 when it set none, or every port in it is taken.
         *
         * @par
         * Only applications need one: several instances of one share a host, so a port named in the
         * application's own configuration is bound by whichever starts first and refused to every
         * other. Handed out from a range rather than left to the kernel because it has to be
         * knowable afterwards - the gateway routes to it, and a port the kernel picked is written
         * down nowhere. The process is told it as EUCLID_HTTP_PORT.
         *
         * @par
         * A slot that already holds one keeps it, so a restart lands on the same port and nothing
         * pointed at the instance has to be told. Nothing outside the range is ever handed out: a
         * port the operator did not set aside may belong to something else on the host.
         *
         * @param held the ports already handed out on this host, by instance id.
         * @param instanceId the slot asking.
         * @param first,last the range, inclusive.
         */
        [[nodiscard]] int PickHttpPort(const std::map<std::string, int> &held, const std::string &instanceId, long first, long last);

        /**
         * @brief The log channel an application's own output is re-emitted on: "app.<runtimeName>".
         *
         * @par
         * What makes its output something an operator can turn down or off by itself - in
         * euclid.logging.channels, or with eap set-log-level - which matters because one talkative
         * application otherwise buries everything euclid has to say. The same channel on every host,
         * so the level set for an application applies wherever it runs.
         */
        [[nodiscard]] std::string OutputChannel(const std::string &runtimeName);

        /**
         * @brief What euclid knows about the process a line came from and the line itself does not
         * say: the service, the namespace, the account, as a JSON object for LogStream::LogVerbatim.
         *
         * @par
         * One index holds every namespace of an installation, so this is what tells development's
         * output from production's - an applicationId does not, being unique only within
         * (account, namespace).
         */
        [[nodiscard]] std::string OutputFields(const std::string &runtimeName, const std::string &nameSpace, const std::string &accountId);

        /**
         * @brief Escapes ASCII control bytes, other than tab, as \\xHH.
         *
         * @par
         * A child's output is not trusted content. It can carry raw bytes from a misbehaving
         * dependency, and journalctl -o cat prints a message unescaped, so a raw ESC byte becomes a
         * terminal escape sequence on whatever terminal later views the log. Every other log line
         * goes through Boost.Log's formatter, which escapes on its way; a child's line is written
         * verbatim by design, so it is the one path that needs its own guard.
         */
        [[nodiscard]] std::string SanitizeOutput(const std::string &raw);

        /**
         * @brief The severity a line of a child's output is recorded at.
         *
         * @par
         * An application that logs JSON has already said what its record means - "level", with
         * Logback's and SLF4J's spellings understood - and taking its word for it is what makes a
         * channel level worth setting. Otherwise the pipe decides: stderr is an error, so a channel
         * left at "error" still shows what went wrong, and stdout is info.
         */
        [[nodiscard]] boost::log::trivial::severity_level OutputSeverity(const std::string &line, bool fromStderr);

        /**
         * @brief Records one line of a child's output on @p channel: sanitised, at its own severity,
         * verbatim, with @p fields.
         *
         * @par
         * What both the manager and a worker do with every line they drain from a process's pipes.
         * Reading the pipe is theirs - it is a different call on Windows - and what a line becomes
         * is this.
         */
        void EmitOutput(const std::string &channel, const std::string &line, bool fromStderr, const std::string &fields);

        /**
         * @brief What the credentials file is called in an application's directory.
         *
         * @par
         * One name on every host. An application finds it through EUCLID_CREDENTIALS_FILE rather
         * than by name, but an operator looking for it on a node should not need to know which
         * kind of host they are on.
         */
        inline constexpr const char *CredentialsFileName = "credentials";

        /**
         * @brief How long issued credentials are good for: `euclid.modules.eap.credentials-ttl-seconds`,
         * an hour by default, never under a minute.
         *
         * @par
         * Short enough that a token lifted out of a process is worth little, long enough that the
         * rewrite is a rare event. The floor is there because the refresh happens at half-life: a
         * TTL of a few seconds would have every holder rewriting on every tick.
         */
        [[nodiscard]] std::chrono::seconds CredentialsTtl();

        /**
         * @brief The gateway endpoint an application calls back in on, from the master's
         * `euclid.gateway.*` configuration.
         */
        [[nodiscard]] std::string GatewayEndpoint();

        /**
         * @brief What only the host starting a process can say about it, added to the environment
         * its definition describes.
         *
         * @par
         * The other half of Database::Entity::EAP::ApplicationEnvironment(). These are the host's
         * because they differ by host: the gateway is "localhost" from the manager and an address
         * from a node, the certificate to trust is a path on this disk, and so is the credentials
         * file. Each host passes its own; nothing here is read from configuration, so a worker and
         * the manager cannot accidentally hand an application the other's view.
         *
         * @param environment the definition's half; these overwrite anything of the same name.
         * @param endpoint the gateway, as this host reaches it. Exported twice - EUCLID_ENDPOINT,
         * and EUCLID_BASE_URL, the name the SDKs bind (euclid-spring reads euclid.base-url).
         * @param caCertPath the certificate to trust for that endpoint; left out when empty, since a
         * variable naming no usable file is worse than none - an SDK finding nothing can still fall
         * back to the system trust store.
         * @param credentialsFile where this host writes the application's credentials.
         */
        void AddHostEnvironment(std::map<std::string, std::string> &environment, const std::string &endpoint,
                                const std::string &caCertPath, const std::string &credentialsFile);

        /**
         * @brief Who credentials are being issued for.
         */
        struct Principal {
            std::string userId;
            std::string accountId;
            std::string region;
            /**
             * @brief Sent by the application's client as x-euclid-namespace, which is what lets it
             * name a queue, topic or bucket rather than spell out a full ERN.
             */
            std::string nameSpace;
        };

        /**
         * @brief Mints the credentials blob an application reads: a bearer token for the principal,
         * when it stops being valid, and where to present it.
         *
         * @par
         * The master's alone - it signs with the installation's secret, which a worker never holds.
         * The manager writes the result for its own applications; EAP hands it to a worker for
         * one placed on a node. The same blob either way, so the same file and the same refresh rule.
         */
        [[nodiscard]] boost::json::object IssueCredentials(const Principal &principal);

        /**
         * @brief When a credentials blob stops being valid; nothing if it does not say.
         */
        [[nodiscard]] std::optional<std::chrono::system_clock::time_point> CredentialsExpiry(const boost::json::value &credentials);

        /**
         * @brief The expiry of the credentials file at @p path; nothing if it is missing,
         * unreadable or says no expiry - each of which means "write it again".
         */
        [[nodiscard]] std::optional<std::chrono::system_clock::time_point> ReadCredentialsExpiry(const std::filesystem::path &path);

        /**
         * @brief When credentials should next be replaced: halfway through what is left of their
         * life, so a holder always has at least that long in hand however unluckily a tick lands.
         *
         * @param expiresAt when they stop being valid.
         * @param issuedAt when they were written.
         * @return issuedAt itself for credentials already expired - replace them now.
         */
        [[nodiscard]] std::chrono::system_clock::time_point RefreshAt(std::chrono::system_clock::time_point expiresAt,
                                                                      std::chrono::system_clock::time_point issuedAt);

        /**
         * @brief Writes a credentials blob to @p path, readable by its owner only.
         *
         * @par
         * Written beside the target and renamed into place, so a process reading the file never
         * sees half of one: rename() within a directory is atomic, a rewrite in place is not. The
         * mode is set before the rename, so there is no moment at which the token is readable by
         * anybody else.
         *
         * @return false when it could not be written; whatever was there before is left alone.
         */
        [[nodiscard]] bool WriteCredentials(const std::filesystem::path &path, const boost::json::value &credentials);

    }// namespace Launch

}// namespace Euclid::Core
