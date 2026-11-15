// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// file-delivery - a euclid application in C++, written against euclid-cdk.
//
// Picks up what FTP and SFTP clients upload and delivers it to another bucket. The transfer servers
// of examples/docker store every upload in the "transfer-server" bucket, under the uploading user's
// home: ftpuser1/incoming/report.csv. This application subscribes its queue to that bucket's object
// events and, for each file that lands in an "incoming" directory, writes a copy to the
// "file-delivery" bucket under the same user: ftpuser1/report.csv. A ZIP archive is not copied but
// unpacked into a temporary directory, and each file in it delivered on its own:
// ftpuser1/incoming/batch.zip holding a.csv and b/c.csv becomes ftpuser1/a.csv and ftpuser1/b/c.csv.
//
// Still a queue consumer underneath, like every application: take a message, do the work, delete
// it. The bucket subscription is what turns uploads into messages - nothing polls the bucket.
//
// Deployed as BINARY, the runtime for something already executable. See README.md in this
// directory for the commands and the grants it needs.

// C++ includes
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// libarchive includes
#include <archive.h>
#include <archive_entry.h>

// Euclid includes
#include <euclid/cdk/Euclid.h>

using namespace Euclid::CDK;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

    // How long a receive may wait for work before coming back empty. Twenty seconds is EQS's
    // maximum, and a long poll is how a worker stays responsive without spinning.
    constexpr auto kWaitTime = 20s;

    // How many notifications one receive may claim. Small on purpose: each one can mean
    // downloading and unpacking a large archive, and every message claimed is on the clock of its
    // visibility timeout from the moment it is received - not from the moment its turn comes.
    constexpr long kBatchSize = 4;

    // How often a notification is tried before it is given up on. A file that fails every time - a
    // corrupt or encrypted ZIP, say - would otherwise come back after every visibility timeout for
    // the queue's whole retention period, and be retried by every instance in turn.
    constexpr long kMaxAttempts = 5;

    // -- shutdown -------------------------------------------------------------------------------

    // Set by the signal handler and read by the loop. The manager sends SIGTERM and waits; an
    // application that ignores it is SIGKILLed after a few seconds, mid-file, and the notification
    // comes back to whoever polls next. Exiting on purpose means the current file finishes.
    std::atomic<bool> g_running{true};

    // Everything a signal handler is allowed to do: writing a lock-free flag. Not logging, not any
    // CDK call - a handler runs between two arbitrary instructions of whatever it interrupted.
    extern "C" void onSignal(int) { g_running.store(false); }

    // -- configuration --------------------------------------------------------------------------

    std::string env(const char *name, const std::string &fallback = {}) {
        const char *value = std::getenv(name);
        return value != nullptr && *value != '\0' ? std::string(value) : fallback;
    }

    std::uintmax_t envBytes(const char *name, const std::uintmax_t fallback) {
        try {
            const auto value = env(name);
            return value.empty() ? fallback : static_cast<std::uintmax_t>(std::stoull(value));
        } catch (const std::exception &) {
            std::cerr << name << " is not a number, using " << fallback << std::endl;
            return fallback;
        }
    }

    struct Config {
        std::string applicationId;
        std::string instanceId;

        // Where the uploads land, and where they are delivered to.
        std::string sourceBucket;
        std::string targetBucket;

        // The queue the source bucket's notifications are delivered to.
        std::string queueName;

        // The directory, inside each user's home, whose files are delivered. Anything uploaded
        // elsewhere - "outgoing", the home itself - is left alone.
        std::string incomingDir;

        // The most one archive may unpack to. A ZIP of a few kilobytes can expand to terabytes,
        // and every byte of it lands on this host's disk before it is delivered.
        std::uintmax_t maxExtractedBytes;
    };

    Config loadConfig() {
        Config config;
        config.applicationId = env("EUCLID_APPLICATION_ID", "file-delivery");
        config.instanceId = env("EUCLID_INSTANCE_ID", "local");
        config.sourceBucket = env("SOURCE_BUCKET", "transfer-server");
        config.targetBucket = env("TARGET_BUCKET", "file-delivery");
        config.queueName = env("DELIVERY_QUEUE", config.applicationId + "-queue");
        config.incomingDir = env("INCOMING_DIR", "incoming");
        config.maxExtractedBytes = envBytes("MAX_EXTRACTED_BYTES", 4ULL * 1024 * 1024 * 1024);
        return config;
    }

    // -- the session ----------------------------------------------------------------------------

    // Builds a session from the credentials the manager wrote.
    //
    // No login: an application has no password. It is handed a bearer token for its own technical
    // principal in the file EUCLID_CREDENTIALS_FILE names, and Credentials::Load() reads exactly
    // that file. Nothing is cached back (cache=false): ~/.euclid/credentials is the CLI's file.
    std::optional<EAM::Session> openSession() {

        const auto stored = Credentials::Load();
        if (!stored.has_value()) {
            std::cerr << "no credentials: EUCLID_CREDENTIALS_FILE=" << env("EUCLID_CREDENTIALS_FILE", "(unset)") << std::endl;
            return std::nullopt;
        }

        EAM::SessionOptions options;
        options.baseUrl = !stored->baseUrl.empty() ? stored->baseUrl : env("EUCLID_ENDPOINT", env("EUCLID_BASE_URL"));
        options.token = stored->token;
        options.userId = stored->userId;
        options.accountId = stored->accountId;
        options.region = stored->region;
        // What lets this name a bucket or a queue rather than spell out a full ERN.
        options.nameSpace = stored->nameSpace;
        options.signingScheme = &SigningScheme::Rfc9421();
        options.auth = EAM::AuthMode::Bearer;
        options.cache = false;

        // A euclid deployment's gateway has a self-signed certificate, so the system trust store
        // alone fails with "certificate verify failed". EUCLID_CA_CERT overrides the default.
        options.connection.caCertPath = env("EUCLID_CA_CERT", HttpClient::DefaultCaCertPath());
        // Long enough for one part of a transfer; the transfers themselves have their own timeout.
        options.connection.timeout = std::chrono::seconds(60);

        if (options.baseUrl.empty()) {
            std::cerr << "credentials name no server, and neither does the environment" << std::endl;
            return std::nullopt;
        }

        EAM::Session session(std::move(options));

        // The token is replaced while this process runs, so the session is told where to get the
        // current one rather than handed a copy of the first - otherwise it works for an hour and
        // then collects "Bearer token expired". The transfers below call this from several threads,
        // which is fine: it only reads a file the manager replaces by rename.
        session.SetTokenProvider([] {
            const auto current = Credentials::Load();
            return current.has_value() ? current->token : std::string{};
        });

        return session;
    }

    // -- load reporting -------------------------------------------------------------------------

    // Tells the autoscaler how busy this instance is - including when it is idle, since an instance
    // whose last report is older than 45 seconds counts as busy and is never scaled back down.
    void reportLoad(const EAP::Eap &eap, const std::string &applicationId, const long received, const long backlog) {

        EAP::LoadReportOptions report;
        report.utilisation = 100.0 * static_cast<double>(received) / static_cast<double>(kBatchSize);
        report.backlog = backlog;

        try {
            std::ignore = eap.ReportLoad(applicationId, report);
        } catch (const std::exception &ex) {
            // Never fatal: a lost report costs responsiveness, an exit costs the work in hand.
            std::cerr << "load report failed: " << ex.what() << std::endl;
        }
    }

    // -- keys -----------------------------------------------------------------------------------

    std::vector<std::string> segmentsOf(const std::string &path) {
        std::vector<std::string> segments;
        std::string current;
        for (const char c: path) {
            if (c == '/') {
                if (!current.empty()) segments.push_back(std::move(current));
                current.clear();
            } else {
                current += c;
            }
        }
        if (!current.empty()) segments.push_back(std::move(current));
        return segments;
    }

    std::string joined(const std::vector<std::string>::const_iterator begin, const std::vector<std::string>::const_iterator end) {
        std::string result;
        for (auto it = begin; it != end; ++it) {
            if (!result.empty()) result += '/';
            result += *it;
        }
        return result;
    }

    std::string childKey(const std::string &parent, const std::string &child) {
        return parent.empty() ? child : parent + "/" + child;
    }

    // Where an uploaded file came from: the home it was uploaded to ("ftpuser1"), and its path
    // inside that home's incoming directory ("report.csv", or "2026/report.csv" for a client that
    // made a subdirectory). Nothing for a key outside an incoming directory.
    struct Route {
        std::string owner;
        std::string relative;
    };

    std::optional<Route> routeOf(const std::string &key, const std::string &incomingDir) {
        const auto segments = segmentsOf(key);
        const auto incoming = std::ranges::find(segments, incomingDir);
        if (incoming == segments.end() || std::next(incoming) == segments.end()) return std::nullopt;
        return Route{.owner = joined(segments.begin(), incoming), .relative = joined(std::next(incoming), segments.end())};
    }

    bool isZip(const std::string &key) {
        if (key.size() < 4) return false;
        std::string suffix = key.substr(key.size() - 4);
        std::ranges::transform(suffix, suffix.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return suffix == ".zip";
    }

    // An archive entry's name as a relative path that cannot leave the directory it is unpacked
    // into, or nothing if it would. A ZIP names its entries however its maker liked: "../../etc/x"
    // and "/etc/x" are both valid names, and writing either where it points is the "zip slip"
    // vulnerability. ".." is refused rather than resolved, since no honest archive needs it.
    std::optional<std::string> safeEntryName(std::string name) {
        std::ranges::replace(name, '\\', '/');
        std::vector<std::string> kept;
        for (auto &segment: segmentsOf(name)) {
            if (segment == ".") continue;
            if (segment == "..") return std::nullopt;
            kept.push_back(std::move(segment));
        }
        if (kept.empty()) return std::nullopt;
        return joined(kept.begin(), kept.end());
    }

    // -- temporary space ------------------------------------------------------------------------

    // A directory of this instance's own for one notification, removed again however the work on it
    // ends - an exception halfway through an archive included. Named after the instance, so two
    // instances sharing a host never share one.
    class TempDir {
    public:
        explicit TempDir(const std::string &instanceId) {
            static std::atomic<long> counter{0};
            _path = fs::temp_directory_path() / ("file-delivery-" + instanceId + "-" + std::to_string(counter++));
            fs::remove_all(_path);
            fs::create_directories(_path);
        }

        ~TempDir() {
            std::error_code ec;
            fs::remove_all(_path, ec);
        }

        TempDir(const TempDir &) = delete;
        TempDir &operator=(const TempDir &) = delete;

        [[nodiscard]] const fs::path &path() const { return _path; }

    private:
        fs::path _path;
    };

    // -- unzipping ------------------------------------------------------------------------------

    struct ExtractedFile {
        // The entry's name inside the archive, made safe - what it is delivered under.
        std::string name;
        fs::path file;
    };

    std::string archiveError(archive *reader) {
        const char *message = archive_error_string(reader);
        return message != nullptr ? message : "unknown error";
    }

    // Unpacks every regular file of a ZIP archive below a directory. Directories are not entries of
    // their own here - a file's parents are created for it - and links, devices and the like are
    // skipped: none of them is something to deliver, and a link is a way out of the directory.
    std::vector<ExtractedFile> extractZip(const fs::path &zip, const fs::path &into, const std::uintmax_t maxBytes) {

        const std::unique_ptr<archive, decltype(&archive_read_free)> reader(archive_read_new(), &archive_read_free);
        if (!reader) throw std::runtime_error("cannot allocate an archive reader");

        archive_read_support_format_zip(reader.get());
        if (archive_read_open_filename(reader.get(), zip.string().c_str(), 64 * 1024) != ARCHIVE_OK) {
            throw std::runtime_error("not a readable ZIP archive: " + archiveError(reader.get()));
        }

        std::vector<ExtractedFile> files;
        std::uintmax_t total = 0;
        std::array<char, 64 * 1024> buffer{};

        archive_entry *entry = nullptr;
        while (true) {
            const int status = archive_read_next_header(reader.get(), &entry);
            if (status == ARCHIVE_EOF) break;
            // ARCHIVE_WARN is a header libarchive could read with something to say about it, which
            // is still an entry; anything worse is an archive that cannot be read any further.
            if (status < ARCHIVE_WARN) throw std::runtime_error("cannot read the archive: " + archiveError(reader.get()));

            const char *rawName = archive_entry_pathname_utf8(entry);
            if (rawName == nullptr) rawName = archive_entry_pathname(entry);
            const std::string displayName = rawName != nullptr ? rawName : "(unnamed)";

            if (archive_entry_filetype(entry) != AE_IFREG) {
                archive_read_data_skip(reader.get());
                continue;
            }

            const auto name = rawName != nullptr ? safeEntryName(rawName) : std::nullopt;
            if (!name.has_value()) {
                std::cerr << "  skipped entry with an unsafe name: " << displayName << std::endl;
                archive_read_data_skip(reader.get());
                continue;
            }

            const fs::path file = into / fs::path(*name);
            fs::create_directories(file.parent_path());
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            if (!out) throw std::runtime_error("cannot write " + file.string());

            while (true) {
                const la_ssize_t read = archive_read_data(reader.get(), buffer.data(), buffer.size());
                if (read == 0) break;
                // An encrypted entry, or one whose data does not match its checksum, ends up here.
                if (read < 0) throw std::runtime_error("cannot unpack " + displayName + ": " + archiveError(reader.get()));

                // Counted as it is written rather than from the sizes the archive declares: those
                // are the archive's own claims, and a hostile one makes them up.
                total += static_cast<std::uintmax_t>(read);
                if (total > maxBytes) {
                    throw std::runtime_error("unpacks to more than " + std::to_string(maxBytes) + " bytes (MAX_EXTRACTED_BYTES)");
                }
                out.write(buffer.data(), read);
                if (!out) throw std::runtime_error("cannot write " + file.string());
            }

            files.push_back({.name = *name, .file = file});
        }
        return files;
    }

    // -- delivering -----------------------------------------------------------------------------

    struct Buckets {
        std::string sourceErn;
        std::string targetErn;
    };

    void deliver(const ESM::Esm &esm, const std::string &targetErn, const std::string &key, const fs::path &file) {
        // Overwrites whatever is there under that key, which is what makes a notification handled
        // twice - after a crash, or by two instances - harmless: the second delivery writes the same
        // bytes again.
        const auto stored = esm.UploadFile(targetErn, key, file.string());
        std::cout << "  delivered " << key << " (" << stored.size << " bytes)" << std::endl;
    }

    // One notification. Throws if the file could not be delivered, which leaves the notification
    // in the queue to be tried again; returns normally both when it was delivered and when there
    // was nothing to deliver, and either way the notification is then done with.
    void handle(const ESM::Esm &esm, const Buckets &buckets, const Config &config, const ESM::BucketEvent &event) {

        // Deletions, and anything this subscription would not normally deliver.
        if (event.eventType != ESM::ObjectCreated && event.eventType != ESM::ObjectUpdated) return;
        if (event.key.empty() || event.key.ends_with('/')) return;

        const auto route = routeOf(event.key, config.incomingDir);
        if (!route.has_value()) {
            std::cout << "ignored " << event.key << ": not in an '" << config.incomingDir << "' directory" << std::endl;
            return;
        }

        std::cout << "picked up " << event.key << " (" << event.size << " bytes)" << std::endl;

        TempDir temp(config.instanceId);
        const fs::path download = temp.path() / "download";
        esm.DownloadFile(buckets.sourceErn, event.key, download.string());

        if (!isZip(event.key)) {
            deliver(esm, buckets.targetErn, childKey(route->owner, route->relative), download);
            return;
        }

        // The archive's contents go where the archive was: an archive uploaded to
        // incoming/2026/batch.zip unpacks below the owner's 2026/.
        const auto archiveDir = fs::path(route->relative).parent_path().generic_string();
        const auto base = archiveDir.empty() ? route->owner : childKey(route->owner, archiveDir);

        const auto files = extractZip(download, temp.path() / "unpacked", config.maxExtractedBytes);
        if (files.empty()) {
            std::cout << "  " << event.key << " holds no files" << std::endl;
            return;
        }
        for (const auto &file: files) {
            // Stopping halfway is fine: the notification stays in the queue, and whoever takes it
            // next unpacks the archive again and writes the files already delivered once more.
            if (!g_running.load()) throw std::runtime_error("stopping before the archive was fully delivered");
            deliver(esm, buckets.targetErn, childKey(base, file.name), file.file);
        }
        std::cout << "  unpacked " << event.key << ": " << files.size() << " file(s)" << std::endl;
    }

    // -- startup --------------------------------------------------------------------------------

    std::string ensureQueue(const EQS::Eqs &eqs, const std::string &name) {
        // Two calls rather than a create that tolerates failure: "already exists" and "you may not"
        // are different answers, and only one of them is fine to carry on from.
        if (eqs.ExistsQueue(name)) return eqs.GetQueueErn(name);
        std::cout << "creating queue " << name << std::endl;
        return eqs.CreateQueue(name).ern;
    }

    // Subscribes the queue to the source bucket's created and updated events, unless it already is.
    // Subscribe() is not idempotent - a second call is a second subscription, and every upload would
    // then arrive twice - so a restart checks first. Two instances starting at the same moment can
    // still both subscribe; the cost is each file delivered twice, which overwrites itself.
    void ensureSubscription(const ESM::Esm &esm, const std::string &bucketErn, const std::string &queueErn) {
        for (const auto &subscription: esm.ListSubscriptions(bucketErn)) {
            if (subscription.targetErn == queueErn) return;
        }

        ESM::SubscribeOptions options;
        // Updated as well as created: a client that uploads a file under a name it used before
        // replaces the object, and that is an update. Directory markers are left out (the default).
        options.eventTypes = {std::string(ESM::ObjectCreated), std::string(ESM::ObjectUpdated)};
        const auto result = esm.Subscribe(bucketErn, std::string(COM::Queue), queueErn, options);
        std::cout << "subscribed the queue to the bucket's uploads, subscription: " << result.ern << std::endl;
    }

}// namespace

int main() {

    std::signal(SIGTERM, onSignal);
    std::signal(SIGINT, onSignal);

    const auto config = loadConfig();

    // Logged first: stdout and stderr land in euclid's log under this application's channel, so
    // the version printed here answers "what is actually running?" from the log alone.
    std::cout << "file-delivery starting"
              << ", application: " << config.applicationId
              << ", version: " << env("EUCLID_APPLICATION_VERSION", "(unset)")
              << ", instance: " << config.instanceId
              << ", from: " << config.sourceBucket << "/*/" << config.incomingDir
              << ", to: " << config.targetBucket
              << ", queue: " << config.queueName << std::endl;

    auto session = openSession();
    if (!session.has_value()) return 1;

    // All three hold a reference to the session, which has to outlive them - the reason token
    // rotation goes through the session's provider rather than by rebuilding anything.
    const EQS::Eqs eqs(*session);
    const ESM::Esm esm(*session);
    const EAP::Eap eap(*session);

    // Both buckets have to exist already: creating buckets is the installation's business
    // (examples/docker/setup-infrastructure.sh), not something an application should be able to do.
    Buckets buckets;
    std::string queueErn;
    try {
        buckets.sourceErn = esm.GetBucketErn(config.sourceBucket);
        buckets.targetErn = esm.GetBucketErn(config.targetBucket);
        queueErn = ensureQueue(eqs, config.queueName);
        ensureSubscription(esm, buckets.sourceErn, queueErn);
    } catch (const std::exception &ex) {
        std::cerr << "cannot set up: " << ex.what() << std::endl;
        return 1;
    }

    long delivered = 0;
    // Whether this principal may count its own queue - asked once, and left alone if refused.
    bool backlogVisible = true;

    while (g_running.load()) {

        long received = 0;
        long backlog = 0;
        try {
            const auto batch = eqs.ReceiveMessages(queueErn, {.maxMessages = kBatchSize, .waitTime = kWaitTime});
            received = static_cast<long>(batch.items.size());

            for (const auto &message: batch.items) {
                try {
                    handle(esm, buckets, config, ESM::ParseBucketEvent(message.body));
                    ++delivered;
                } catch (const std::exception &ex) {
                    if (message.receivedCount < kMaxAttempts) {
                        // Not deleted, so it comes back once its visibility timeout runs out.
                        std::cerr << "delivery failed, attempt " << message.receivedCount << " of " << kMaxAttempts
                                  << ", will retry: " << ex.what() << std::endl;
                        if (!g_running.load()) break;
                        continue;
                    }
                    std::cerr << "delivery failed " << message.receivedCount << " times, giving up on message "
                              << message.messageId << ": " << ex.what() << std::endl;
                }

                // Deleted only now: after the work succeeded, or after giving up on it. A
                // notification deleted before the work is a file never delivered.
                eqs.DeleteMessage(message.receiptHandle);

                if (!g_running.load()) break;
            }

        } catch (const std::exception &ex) {
            // A queue that cannot be reached is worth a line and another try, not an exit.
            std::cerr << "receive cycle failed: " << ex.what() << std::endl;
            std::this_thread::sleep_for(1s);
        }

        // Needs eqs:get-message-count, which the built-in "application" role grants. Asked once
        // and then left alone if refused - a 403 every twenty seconds is noise.
        if (backlogVisible) {
            try {
                backlog = eqs.GetMessageCount(queueErn).available;
            } catch (const std::exception &ex) {
                backlogVisible = false;
                std::cerr << "backlog not visible, reporting utilisation only: " << ex.what() << std::endl;
            }
        }

        reportLoad(eap, config.applicationId, received, backlog);
    }

    std::cout << "file-delivery stopping, notifications handled: " << delivered << std::endl;
    return 0;
}
