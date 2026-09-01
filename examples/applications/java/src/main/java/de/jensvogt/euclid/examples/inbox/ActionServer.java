package de.jensvogt.euclid.examples.inbox;

import java.io.IOException;
import java.net.StandardProtocolFamily;
import java.net.UnixDomainSocketAddress;
import java.nio.ByteBuffer;
import java.nio.channels.ServerSocketChannel;
import java.nio.channels.SocketChannel;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;

/**
 * The half of the euclid application contract that makes this reachable: an HTTP server on the
 * Unix socket the manager passes in {@code EUCLID_SOCKET}.
 *
 * <p>Creating that socket is the readiness signal - the manager waits for it to appear and kills
 * the process if it does not show up within the application's {@code readyTimeoutMs}, which for a
 * JVM is worth setting generously. Once it is there, the gateway forwards requests addressed to
 * this application ({@code x-euclid-target: <applicationId>}) straight to it, and the autoscaler
 * treats it like any other module in the pool.
 *
 * <p>Deliberately hand-rolled rather than a framework: an application needs to answer
 * {@code x-euclid-action} with JSON over a Unix socket, and that is small enough to read in one
 * sitting. Anything that can serve HTTP on a {@link java.net.UnixDomainSocketAddress} will do.
 */
final class ActionServer {

    private final Path socketPath;
    private final AtomicLong filesSeen;
    private final AtomicLong messagesSeen;
    private final AtomicBoolean connected;

    ActionServer(String socketPath, AtomicLong filesSeen, AtomicLong messagesSeen, AtomicBoolean connected) {
        this.socketPath = Path.of(socketPath);
        this.filesSeen = filesSeen;
        this.messagesSeen = messagesSeen;
        this.connected = connected;
    }

    /**
     * Binds the socket and serves it on a daemon thread.
     *
     * @throws IOException if the socket cannot be created, which the manager will see as a failure
     *                     to start.
     */
    void start() throws IOException {
        Files.deleteIfExists(socketPath);

        final ServerSocketChannel channel = ServerSocketChannel.open(StandardProtocolFamily.UNIX);
        channel.bind(UnixDomainSocketAddress.of(socketPath));

        // Deleted on the way out so a restarted instance does not inherit a stale socket file;
        // the manager's instance sockets are named per pid, but tidiness here costs nothing.
        Runtime.getRuntime().addShutdownHook(new Thread(() -> {
            try {
                channel.close();
                Files.deleteIfExists(socketPath);
            } catch (IOException ignored) {
                // Shutting down anyway.
            }
        }));

        final Thread thread = new Thread(() -> serve(channel), "euclid-action-server");
        thread.setDaemon(true);
        thread.start();

        InboxApplication.log("listening on %s", socketPath);
    }

    private void serve(ServerSocketChannel channel) {
        while (channel.isOpen()) {
            try (SocketChannel connection = channel.accept()) {
                handle(connection);
            } catch (IOException ex) {
                if (!channel.isOpen()) return;
                InboxApplication.log("connection failed: %s", ex.getMessage());
            }
        }
    }

    private void handle(SocketChannel connection) throws IOException {

        final String request = readRequest(connection);
        final String action = headerValue(request, "x-euclid-action");

        final String body = switch (action) {
            case "ping" -> """
                    {"application":"%s","pid":%d}"""
                    .formatted(System.getenv().getOrDefault("EUCLID_APPLICATION_ID", "inbox"), ProcessHandle.current().pid());
            case "status" -> """
                    {"bucket":"%s","queue":"%s","connected":%b,"filesSeen":%d,"messagesSeen":%d}"""
                    .formatted(System.getenv().getOrDefault("INBOX_BUCKET", "inbox"),
                               System.getenv().getOrDefault("INBOX_QUEUE", "inbox-queue"),
                               connected.get(), filesSeen.get(), messagesSeen.get());
            default -> null;
        };

        if (body == null) {
            respond(connection, 404, "{\"error\":\"Action not implemented: %s\"}".formatted(action));
            return;
        }
        respond(connection, 200, body);
    }

    /**
     * Reads one request: everything up to the blank line, then as much body as Content-Length says.
     */
    private static String readRequest(SocketChannel connection) throws IOException {
        final ByteBuffer buffer = ByteBuffer.allocate(8192);
        final StringBuilder request = new StringBuilder();

        while (connection.read(buffer) > 0) {
            buffer.flip();
            request.append(StandardCharsets.UTF_8.decode(buffer));
            buffer.clear();

            final int headerEnd = request.indexOf("\r\n\r\n");
            if (headerEnd < 0) continue;

            final int contentLength = contentLength(request.toString());
            if (request.length() - (headerEnd + 4) >= contentLength) break;
        }
        return request.toString();
    }

    private static int contentLength(String request) {
        final String value = headerValue(request, "content-length");
        try {
            return value.isBlank() ? 0 : Integer.parseInt(value.trim());
        } catch (NumberFormatException ex) {
            return 0;
        }
    }

    /**
     * Case-insensitive header lookup - HTTP header names are not case-sensitive, and a client that
     * spells one differently is not sending a different header.
     */
    private static String headerValue(String request, String name) {
        for (String line : request.split("\r\n")) {
            if (line.isEmpty()) break;
            final int colon = line.indexOf(':');
            if (colon > 0 && line.substring(0, colon).trim().equalsIgnoreCase(name)) {
                return line.substring(colon + 1).trim();
            }
        }
        return "";
    }

    private static void respond(SocketChannel connection, int status, String body) throws IOException {
        final byte[] payload = body.getBytes(StandardCharsets.UTF_8);
        final String response = "HTTP/1.1 " + status + (status == 200 ? " OK" : " Not Found") + "\r\n"
                                + "Content-Type: application/json\r\n"
                                + "Content-Length: " + payload.length + "\r\n"
                                + "Connection: close\r\n\r\n";
        connection.write(ByteBuffer.wrap(response.getBytes(StandardCharsets.UTF_8)));
        connection.write(ByteBuffer.wrap(payload));
    }
}
