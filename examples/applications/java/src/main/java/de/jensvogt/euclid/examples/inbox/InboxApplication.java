package de.jensvogt.euclid.examples.inbox;

import de.jensvogt.euclid.Euclid;
import de.jensvogt.euclid.dto.eqs.model.Message;
import de.jensvogt.euclid.dto.esm.model.EsmObject;
import de.jensvogt.euclid.module.eam.EuclidSession;
import de.jensvogt.euclid.module.eqs.EuclidEqs;
import de.jensvogt.euclid.module.esm.EuclidEsm;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.time.Instant;
import java.util.HashSet;
import java.util.Set;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Example euclid application: an inbox that logs whatever arrives in it.
 *
 * <p>It logs into euclid with the euclid-jdk, makes sure a bucket and a queue exist, and then
 * watches both: the bucket is listed for objects that appeared, and the queue is received from.
 * Anything dropped into the bucket - by {@code esm upload-file}, by an SDK, or by a user on an
 * FTP/SFTP transfer server - is logged as a file, and every message on the queue is logged as it
 * arrives. Subscribe the bucket to the queue ({@code esm subscribe}) and an upload shows up as
 * both: once as the file itself, once as the notification ESM published about it.
 *
 * <p>It runs two ways. From a command line it is an ordinary program. Deployed as a euclid
 * application ({@code eap create-application --runtime JAVA}) it additionally serves the socket in
 * {@code EUCLID_SOCKET}, which is how the manager knows it started and how the gateway reaches it -
 * see {@link ActionServer}.
 *
 * <pre>
 * mvn package
 * euclid-cli esm upload-file --bucket apps --key euclid-inbox-app.jar --file target/euclid-inbox-app.jar
 * euclid-cli eap create-application --application-id inbox --runtime JAVA \
 *     --bucket apps --artifact euclid-inbox-app.jar --ready-timeout 30000
 * euclid-cli eap start-application --application-id inbox
 * </pre>
 */
public final class InboxApplication {

    /** Bucket that is watched; anything uploaded here is logged. */
    private static final String BUCKET = env("INBOX_BUCKET", "inbox");

    /** Queue the bucket's object-created notifications are delivered to. */
    private static final String QUEUE = env("INBOX_QUEUE", "inbox-queue");

    /** How long a receive call waits for a message before returning empty, in seconds. */
    private static final long WAIT_TIME = Long.parseLong(env("INBOX_WAIT_TIME", "5"));

    /** How often the bucket is listed to notice files that arrived, in seconds. */
    private static final long POLL_INTERVAL = Long.parseLong(env("INBOX_POLL_INTERVAL", "10"));

    private static final AtomicLong messagesSeen = new AtomicLong();
    private static final AtomicLong filesSeen = new AtomicLong();
    private static final AtomicBoolean running = new AtomicBoolean(true);
    private static final AtomicBoolean connected = new AtomicBoolean(false);

    /** The session in use, and the token it was built from - see {@link #currentSession()}. */
    private static volatile EuclidSession currentSession;
    private static volatile String currentToken = "";
    private static String endpoint;
    private static String caCert;

    public static void main(String[] args) throws Exception {

        endpoint = env("EUCLID_ENDPOINT", "https://localhost:5566");
        caCert = env("EUCLID_CA_CERT", "/usr/local/euclid/etc/euclid_cert.crt");
        final String user = env("EUCLID_USER", "admin");
        final String password = env("EUCLID_PASSWORD", "");

        // The socket comes first, before anything that can be slow or unavailable. Readiness is
        // this process's own business: an application that logged in first would be killed and
        // restarted by the manager every time EAM happened to be slow, and a restart loop is a
        // worse failure than an application that is up and retrying.
        //
        // Only when the manager started us: from a command line there is no socket to serve, and
        // nobody waiting for one either.
        final String socketPath = System.getenv("EUCLID_SOCKET");
        if (socketPath != null && !socketPath.isBlank()) {
            new ActionServer(socketPath, filesSeen, messagesSeen, connected).start();
        }

        Runtime.getRuntime().addShutdownHook(new Thread(() -> {
            log("shutting down after %d files and %d messages", filesSeen.get(), messagesSeen.get());
            running.set(false);
        }));

        final EuclidSession session = session(endpoint, caCert, user, password);
        if (session == null) return;
        currentSession = session;
        currentToken = readToken();
        connected.set(true);

        final String bucketErn = ensureBucket(session.esm());
        final String queueErn = ensureQueue(session.eqs());

        log("watching bucket '%s' and queue '%s'", BUCKET, QUEUE);
        log("tip: 'euclid-cli esm subscribe --source-ern %s --type SQS --target-ern %s' makes every "
            + "upload arrive as a queue message too", bucketErn, queueErn);

        // Two sources, because a file and a message reach an application by different routes: the
        // bucket is polled for objects that appeared, and the queue is received from. With the
        // subscription above in place the same upload shows up in both - once as a file, once as
        // the notification ESM published about it.
        final Thread files = new Thread(() -> pollBucket(bucketErn), "inbox-bucket-poll");
        files.setDaemon(true);
        files.start();

        receiveLoop(queueErn);
    }

    /**
     * The session this application acts through.
     *
     * <p>Deployed by euclid there is nothing to log in with, and that is the point: the manager
     * puts an access key in the environment belonging to a technical principal created for this
     * application alone - an EAM identity with no password and no way in through
     * {@code eam login}. A session is built straight from that key, and euclid attributes
     * everything this application does to it rather than to whoever deployed it.
     *
     * <p>Run from a command line there is no such key, so it logs in the ordinary way instead.
     *
     * @return the session, or {@code null} if the process was stopped before one was obtained.
     */
    private static EuclidSession session(String endpoint, String caCert, String user, String password) {

        final String managedToken = readToken();
        if (!managedToken.isBlank()) {
            log("using the managed credentials of %s", env("EUCLID_USER_ID", "this application"));
            // Component order of euclid-jdk 0.1.9's EuclidSession, which the pom pins. No access
            // key: with a token present the client sends it as a bearer, which is the whole point
            // of the arrangement - nothing long-lived is in this process.
            return new EuclidSession(managedToken, env("EUCLID_USER_ID", ""), env("EUCLID_ACCOUNT_ID", ""), env("EUCLID_REGION", ""),
                                     "", "", "", endpoint, caCert, env("EUCLID_NAMESPACE", ""));
        }

        final String accessKeyId = System.getenv("EUCLID_ACCESS_KEY_ID");
        final String secretAccessKey = System.getenv("EUCLID_SECRET_ACCESS_KEY");

        if (accessKeyId != null && !accessKeyId.isBlank() && secretAccessKey != null && !secretAccessKey.isBlank()) {
            log("using the injected access key of %s", env("EUCLID_USER_ID", "this application"));
            return new EuclidSession("", env("EUCLID_USER_ID", ""), env("EUCLID_ACCOUNT_ID", ""), env("EUCLID_REGION", ""),
                                     accessKeyId, secretAccessKey, "", endpoint, caCert, env("EUCLID_NAMESPACE", ""));
        }
        return login(endpoint, caCert, user, password);
    }

    /**
     * The session to use right now, rebuilt whenever the manager has replaced the token on disk.
     *
     * <p>This is what makes short-lived credentials workable: the token in a running process
     * expires, so the process has to notice a new one rather than hold the first it was given.
     * Nothing else about the session changes, so only the token is compared.
     */
    private static EuclidSession currentSession() {
        final String token = readToken();
        if (token.isBlank() || token.equals(currentToken)) return currentSession;

        log("credentials rotated, rebuilding session");
        currentToken = token;
        currentSession = new EuclidSession(token, env("EUCLID_USER_ID", ""), env("EUCLID_ACCOUNT_ID", ""),
                                           env("EUCLID_REGION", ""), "", "", "", endpoint, caCert, env("EUCLID_NAMESPACE", ""));
        return currentSession;
    }

    /**
     * The bearer token the manager currently has on disk for this application, or an empty string
     * when it manages none.
     *
     * <p>Read afresh rather than remembered: the token expires, and the manager replaces the file
     * before it does. An application that held on to the first one it saw would work for an hour
     * and then start failing.
     */
    private static String readToken() {
        final String path = System.getenv("EUCLID_CREDENTIALS_FILE");
        if (path == null || path.isBlank() || !Files.exists(Path.of(path))) return "";
        try {
            final String content = Files.readString(Path.of(path));
            final int start = content.indexOf("\"token\"");
            if (start < 0) return "";
            final int open = content.indexOf('"', content.indexOf(':', start) + 1);
            final int close = content.indexOf('"', open + 1);
            return open < 0 || close < 0 ? "" : content.substring(open + 1, close);
        } catch (IOException ex) {
            log("could not read %s: %s", path, ex.getMessage());
            return "";
        }
    }

    /**
     * Logs in, retrying until it works or the process is asked to stop.
     *
     * <p>Credentials are only sent when a password was configured: without one the euclid-jdk
     * falls back to the session cached in {@code ~/.euclid/credentials}, which is how this runs
     * from a command line after {@code euclid-cli eam login}.
     *
     * @return the session, or {@code null} if the process was stopped before one was obtained.
     */
    private static EuclidSession login(String endpoint, String caCert, String user, String password) {
        while (running.get()) {
            try {
                log("logging in to %s%s", endpoint, password.isBlank() ? " with the cached session" : " as " + user);
                var access = Euclid.forServer(endpoint).access().caCertPath(caCert);
                if (!password.isBlank()) {
                    access = access.credentials(user, password);
                }
                final EuclidSession session = access.login();
                log("logged in, account: %s, region: %s", session.getAccountId(), session.getRegion());
                return session;
            } catch (InterruptedException ex) {
                Thread.currentThread().interrupt();
                return null;
            } catch (Exception ex) {
                log("login failed, retrying in 5s: %s", describe(ex));
                sleep(Duration.ofSeconds(5));
            }
        }
        return null;
    }

    /**
     * Creates the bucket unless it is already there.
     *
     * <p>Resolved by name first rather than creating and catching: a bucket that already exists is
     * the normal case on every restart after the first, not an error worth an exception.
     */
    private static String ensureBucket(EuclidEsm esm) throws Exception {
        try {
            final String ern = esm.getBucketErn(BUCKET).ern();
            if (ern != null && !ern.isBlank()) {
                log("bucket exists: %s", ern);
                return ern;
            }
        } catch (Exception ignored) {
            // Not found - falls through to create it.
        }
        final String ern = esm.createBucket(BUCKET).ern();
        log("bucket created: %s", ern);
        return ern;
    }

    /**
     * Creates the queue unless it is already there.
     */
    private static String ensureQueue(EuclidEqs eqs) throws Exception {
        try {
            final String ern = eqs.getQueueErn(QUEUE).ern();
            if (ern != null && !ern.isBlank()) {
                log("queue exists: %s", ern);
                return ern;
            }
        } catch (Exception ignored) {
            // Not found - falls through to create it.
        }
        final String ern = eqs.createQueue(QUEUE).ern();
        log("queue created: %s", ern);
        return ern;
    }

    /**
     * Lists the bucket over and over, logging keys that were not there last time.
     *
     * <p>Whatever put them there - the CLI, an SDK, a user on an FTP or SFTP transfer server - an
     * object in the bucket is the arrival this application cares about. What is already present on
     * the first pass is logged as existing, so a restart says what it inherited rather than
     * announcing the whole bucket as new.
     */
    private static void pollBucket(String bucketErn) {
        final Set<String> seen = new HashSet<>();
        boolean first = true;

        while (running.get()) {
            try {
                for (EsmObject object : currentSession().esm().listObjects(bucketErn).objects()) {
                    if (!seen.add(object.key())) continue;
                    log("%s file: key=%s size=%d contentType=%s", first ? "existing" : "new",
                        object.key(), object.size(), object.contentType());
                    if (!first) filesSeen.incrementAndGet();
                }
                first = false;
            } catch (Exception ex) {
                log("listing bucket failed: %s", describe(ex));
            }
            sleep(Duration.ofSeconds(POLL_INTERVAL));
        }
    }

    /**
     * Receives and logs messages until the process is asked to stop.
     *
     * <p>Each message is deleted once logged - a message that is received but never deleted comes
     * back when its visibility timeout runs out, so an inbox that only logged would log the same
     * arrival over and over.
     */
    private static void receiveLoop(String queueErn) {
        while (running.get()) {
            try {
                final EuclidEqs eqs = currentSession().eqs();
                for (Message message : eqs.receiveMessages(queueErn, 10, WAIT_TIME).messages()) {
                    messagesSeen.incrementAndGet();
                    log("message %s (%s): %s", message.messageId(), message.created(), message.body());
                    eqs.deleteMessage(message.receiptHandle());
                }
            } catch (InterruptedException ex) {
                Thread.currentThread().interrupt();
                return;
            } catch (Exception ex) {
                // A failed receive must not end the application: the gateway restarting, or a
                // module being scaled down mid-call, is a pause rather than a reason to exit and
                // have the manager restart the whole process.
                log("receive failed: %s", describe(ex));
                sleep(Duration.ofSeconds(2));
            }
        }
    }

    /**
     * What went wrong, in a form worth logging: plenty of exceptions carry no message at all, and
     * "failed: null" tells nobody anything.
     */
    private static String describe(Exception ex) {
        final String message = ex.getMessage();
        return message == null || message.isBlank() ? ex.getClass().getSimpleName() : message;
    }

    static void log(String format, Object... args) {
        System.out.printf("%s %s%n", Instant.now(), String.format(format, args));
        System.out.flush();
    }

    private static void sleep(Duration duration) {
        try {
            Thread.sleep(duration.toMillis());
        } catch (InterruptedException ex) {
            Thread.currentThread().interrupt();
        }
    }

    private static String env(String name, String fallback) {
        final String value = System.getenv(name);
        return value == null || value.isBlank() ? fallback : value;
    }

    private InboxApplication() {
    }
}
