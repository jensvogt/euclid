// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// echo-worker - a euclid application in C++, written against euclid-cdk.
//
// A queue consumer, which is the shape most applications take: take a message, do something with
// it, delete it, repeat. What it actually does with a message is one line, because that is the part
// you would replace. Everything else is what an application has to get right to be a well-behaved
// one, and each piece says why.
//
// Deployed as BINARY, the runtime for something already executable. See README.md in this
// directory for the commands; the short version is upload the binary, create the application with
// --runtime BINARY, start it, and send it a message.

// C++ includes
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

// Euclid includes
#include <euclid/cdk/Euclid.h>

using namespace Euclid::CDK;
using namespace std::chrono_literals;

namespace {

    // How long a receive may wait for work before coming back empty. Twenty seconds is EQS's
    // maximum, and a long poll is how a worker stays responsive without spinning: an idle instance
    // makes three requests a minute rather than three hundred.
    constexpr auto kWaitTime = 20s;

    // How many messages one receive may claim. Also the denominator for the utilisation this
    // reports - see reportLoad().
    constexpr long kBatchSize = 10;

    // -- shutdown -------------------------------------------------------------------------------

    // Set by the signal handler and read by the loop. The manager sends SIGTERM and waits; an
    // application that ignores it is SIGKILLed after a few seconds, mid-message, and the message
    // comes back to whoever polls next. Exiting on purpose means the current message finishes.
    std::atomic<bool> g_running{true};

    // Everything a signal handler is allowed to do. Not logging, not any CDK call: a handler runs
    // between two arbitrary instructions of whatever it interrupted, and almost nothing is safe
    // there. Writing a lock-free flag is.
    extern "C" void onSignal(int) { g_running.store(false); }

    // -- environment ----------------------------------------------------------------------------

    std::string env(const char *name, const std::string &fallback = {}) {
        const char *value = std::getenv(name);
        return value != nullptr && *value != '\0' ? std::string(value) : fallback;
    }

    // -- the session ----------------------------------------------------------------------------

    // Builds a session from the credentials the manager wrote.
    //
    // No login: an application has no password. It is handed a bearer token for its own technical
    // principal in the file EUCLID_CREDENTIALS_FILE names, and Credentials::Load() reads exactly
    // that file - Credentials::FilePath() prefers the environment variable over ~/.euclid.
    //
    // Nothing is cached back: cache=false, because ~/.euclid/credentials is the CLI's file and an
    // application has no business writing it. AuthMode::Bearer because a technical principal has no
    // access key at all - its long-lived secret never leaves EAM, so the token is the whole of what
    // this process holds and there is nothing here to sign with.
    std::optional<EAM::Session> openSession() {

        const auto stored = Credentials::Load();
        if (!stored.has_value()) {
            std::cerr << "no credentials: EUCLID_CREDENTIALS_FILE=" << env("EUCLID_CREDENTIALS_FILE", "(unset)") << std::endl;
            return std::nullopt;
        }

        EAM::SessionOptions options;
        // The manager writes the server as "endpoint"; EUCLID_ENDPOINT carries the same value, and
        // is the fallback for an older euclid whose file this SDK could not read the server out of.
        options.baseUrl = !stored->baseUrl.empty() ? stored->baseUrl : env("EUCLID_ENDPOINT", env("EUCLID_BASE_URL"));
        options.token = stored->token;
        options.userId = stored->userId;
        options.accountId = stored->accountId;
        options.region = stored->region;
        // What lets this name a queue rather than spell out a full ERN.
        options.nameSpace = stored->nameSpace;
        options.signingScheme = &SigningScheme::Rfc9421();
        options.auth = EAM::AuthMode::Bearer;
        options.cache = false;

        // TLS, and the one an application building its own session has to remember. A euclid
        // deployment installs a self-signed certificate for its gateway, so verifying against the
        // system trust store alone fails with "certificate verify failed" - which is what this
        // example did until it said this. EAM::Eam's constructor fills these in; constructing a
        // Session directly skips it, and there is nothing to warn you.
        //
        // EUCLID_CA_CERT overrides it, for an installation that keeps the certificate somewhere
        // DefaultCaCertPath() does not look.
        options.connection.caCertPath = env("EUCLID_CA_CERT", HttpClient::DefaultCaCertPath());
        options.connection.timeout = std::chrono::seconds(30);

        if (options.baseUrl.empty()) {
            std::cerr << "credentials name no server, and neither does the environment" << std::endl;
            return std::nullopt;
        }

        EAM::Session session(std::move(options));

        // And the part that makes it keep working. The token is replaced while this process runs -
        // once less than half its hour is left - so the session is told where to get the current one
        // rather than handed a copy of the first. Without this an application works for an hour and
        // then collects "Bearer token expired", a long way from the change that caused it.
        //
        // Re-reading the file per request is what this looks like, and it is cheap: the manager
        // writes it beside and renames, so a reader never sees half a file, and the page has been in
        // cache since the last call. A provider that cached with an expiry would be more code for
        // the same behaviour.
        session.SetTokenProvider([] {
            const auto current = Credentials::Load();
            return current.has_value() ? current->token : std::string{};
        });

        return session;
    }

    // -- load reporting -------------------------------------------------------------------------

    // Tells the autoscaler how busy this instance is.
    //
    // Without this the pool never grows past its minimum, whatever the backlog: nothing asks an
    // application for anything over a socket, so the manager cannot see load the way it sees a
    // module's - the application is the only thing that knows. An application that reports nothing
    // is an application that runs one instance forever.
    //
    // **Report when idle too.** An instance whose last report is older than the freshness window
    // (45 seconds) is counted as *busy*, not idle - deliberately, so that an instance the manager
    // has lost sight of is never stopped out from under work it might be doing. The consequence for
    // a worker is the surprising half: one that only reported while it had messages would never be
    // scaled back down. So this is called on every cycle, including the empty ones.
    //
    // Needs eap:report-load, which the built-in "application" role already grants - so it works on
    // a freshly created application with no extra grant. (emo:push-metrics, the older and slower
    // road, does not, which is why this one is worth using.)
    void reportLoad(const EAP::Eap &eap, const std::string &applicationId, const long received, const long backlog) {

        // Two questions of one figure, and the autoscaler reads them against different bars: below
        // 5% it lets the idle timer run, above 75% it asks for another instance. A batch that came
        // back full means there was more work than one cycle could take.
        EAP::LoadReportOptions report;
        report.utilisation = 100.0 * static_cast<double>(received) / static_cast<double>(kBatchSize);
        // Work waiting that nobody has started. This is what lets the pool grow ahead of
        // saturation: one instance at 100% and 4,000 messages waiting needs more than one instance,
        // and utilisation alone cannot say that.
        report.backlog = backlog;
        // instanceId is left empty on purpose: the CDK reads EUCLID_INSTANCE_ID, which is what the
        // manager set, and that is the identifier the pool slot is keyed by.

        try {
            std::ignore = eap.ReportLoad(applicationId, report);
        } catch (const std::exception &ex) {
            // Never fatal. A load report that does not arrive costs responsiveness; an application
            // that exits because one failed costs the work it was doing.
            std::cerr << "load report failed: " << ex.what() << std::endl;
        }
    }

}// namespace

int main() {

    std::signal(SIGTERM, onSignal);
    std::signal(SIGINT, onSignal);

    const auto applicationId = env("EUCLID_APPLICATION_ID", "echo-worker");
    const auto instanceId = env("EUCLID_INSTANCE_ID", "local");
    const auto queueName = env("ECHO_QUEUE", applicationId + "-queue");

    // Logged first, and the reason is operational rather than tidy: stdout and stderr are drained
    // into euclid's log under this application's own channel, so a build printing its version on
    // start-up makes "what is actually running?" answerable from the log alone - no comparing
    // checksums, no asking the manager.
    std::cout << "echo-worker starting"
              << ", application: " << applicationId
              << ", version: " << env("EUCLID_APPLICATION_VERSION", "(unset)")
              << ", instance: " << instanceId
              << ", queue: " << queueName << std::endl;

    auto session = openSession();
    if (!session.has_value()) return 1;

    // Both hold a reference to the session rather than a copy of it - ModuleClient is neither
    // copyable nor assignable, and the header says the session has to outlive the client. That is
    // the reason the token provider above is the right place for rotation: the session is mutable
    // and these follow it, where rebuilding them would mean rebuilding the session they point at.
    const EQS::Eqs eqs(*session);
    const EAP::Eap eap(*session);

    // Create it if it is not there. Two calls rather than a create that tolerates failure, because
    // "already exists" and "you may not do that" are different answers and only one of them is
    // fine to carry on from - see ExistsQueue(), which throws rather than returning false when it
    // cannot tell.
    std::string queueErn;
    try {
        if (!eqs.ExistsQueue(queueName)) {
            std::cout << "creating queue " << queueName << std::endl;
            queueErn = eqs.CreateQueue(queueName).ern;
        } else {
            queueErn = eqs.GetQueueErn(queueName);
        }
    } catch (const std::exception &ex) {
        std::cerr << "cannot reach the queue: " << ex.what() << std::endl;
        return 1;
    }

    long handled = 0;
    // Whether this principal may count its own queue - see where it is used.
    bool backlogVisible = true;

    while (g_running.load()) {

        long received = 0;
        long backlog = 0;
        try {
            const auto batch = eqs.ReceiveMessages(queueErn, {.maxMessages = kBatchSize, .waitTime = kWaitTime});
            received = static_cast<long>(batch.items.size());

            for (const auto &message: batch.items) {

                // The work. Everything above and below is the application; this line is the part
                // that would be yours.
                std::cout << "handled message " << message.messageId << ": " << message.body << std::endl;

                // Acknowledging it. By receipt handle, which is what a consumer holds - and only
                // after the work succeeded: a message deleted before it is processed is a message
                // lost, while one processed and not deleted comes back and is processed twice.
                // Twice is recoverable and gone is not, which is why the delete is last.
                eqs.DeleteMessage(message.receiptHandle);
                ++handled;

                // Checked inside the batch as well: a SIGTERM that arrives with nine messages left
                // should not wait for all nine.
                if (!g_running.load()) break;
            }

        } catch (const std::exception &ex) {
            // A queue that cannot be reached is worth a line and another try, not an exit. The
            // manager would restart this process, which reaches the same unreachable queue with a
            // backoff on top - and an application that exits on the first 500 turns a momentary
            // fault into a pool that is down.
            std::cerr << "receive cycle failed: " << ex.what() << std::endl;
            std::this_thread::sleep_for(1s);
        }

        // What is still waiting, outside the block above so that being refused it does not abandon
        // a cycle that had already done its work.
        //
        // This needs eqs:get-message-count, and the built-in "application" role does not grant it -
        // deliberately, since counting a queue is not something every application should be able to
        // do to every queue. So a worker deployed with nothing else asked for cannot see its own
        // backlog, and only the utilisation half of the autoscaler signal is live: the pool still
        // grows when every instance is saturated, but not ahead of that because 4,000 messages are
        // waiting.
        //
        // Asked once and then left alone if it is refused. A 403 every twenty seconds for the life
        // of the process is noise, and the answer never changes without a new grant.
        if (backlogVisible) {
            try {
                backlog = eqs.GetMessageCount(queueErn).available;
            } catch (const std::exception &ex) {
                backlogVisible = false;
                std::cerr << "backlog not visible, reporting utilisation only - grant "
                             "eqs:get-message-count to see it: " << ex.what() << std::endl;
            }
        }

        reportLoad(eap, applicationId, received, backlog);
    }

    std::cout << "echo-worker stopping, handled: " << handled << std::endl;
    return 0;
}
