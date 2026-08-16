package simpledb;

import simpledb.common.Database;
import simpledb.optimizer.TableStats;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.PrintStream;
import java.io.UnsupportedEncodingException;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.StandardProtocolFamily;
import java.net.UnixDomainSocketAddress;
import java.nio.channels.Channels;
import java.nio.channels.ServerSocketChannel;
import java.nio.channels.SocketChannel;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Multi-client driver for the SimpleDB SQL parser: one JVM, one Catalog, one
 * BufferPool, and one session per connected client. Daemons that need to share
 * tables connect here instead of each forking a private {@link PipeRunner},
 * which cannot share anything because a BufferPool and its LockManager live
 * inside a single JVM.
 *
 * Sessions run concurrently and are isolated from each other by the
 * BufferPool's page-level locking, which aborts one side of a deadlock rather
 * than hanging. A session is one thread with one {@link StatementSession};
 * nothing is shared at the Parser level.
 *
 * Usage:
 * <pre>
 *   java -cp dist/simpledb.jar simpledb.SocketRunner &lt;catalogFile&gt; &lt;address&gt; [options]
 * </pre>
 * where {@code address} is either a filesystem path (a Unix domain socket, for
 * clients on the same host) or a plain port number (TCP on the loopback
 * interface). Options:
 * <ul>
 *   <li>{@code --sessions=N} concurrent sessions to serve, default 8. Further
 *       connections are accepted but wait for a free slot, so a client that
 *       has not yet seen {@code <<READY>>} is queued, not rejected.</li>
 *   <li>{@code --pages=N} BufferPool size, default 16 pages per session. Under
 *       NO STEAL a dirty page cannot be evicted, so a pool too small for the
 *       concurrent working set fails statements outright rather than slowing
 *       down.</li>
 *   <li>{@code --no-recover} skip the recovery pass at startup.</li>
 * </ul>
 *
 * The per-connection wire protocol is identical to {@link PipeRunner}'s, so a
 * client can move between the two by swapping how it obtains its two file
 * descriptors.
 */
public class SocketRunner {

    private static final int DEFAULT_SESSIONS = 8;
    private static final int PAGES_PER_SESSION = 16;

    public static void main(String[] args) throws IOException {
        if (args.length < 2) {
            usage();
            return;
        }
        String catalogFile = args[0];
        String address = args[1];

        int sessions = DEFAULT_SESSIONS;
        int pages = -1;
        boolean recover = true;
        for (int i = 2; i < args.length; i++) {
            String a = args[i];
            if (a.startsWith("--sessions=")) {
                sessions = parsePositive(a, a.substring("--sessions=".length()));
            } else if (a.startsWith("--pages=")) {
                pages = parsePositive(a, a.substring("--pages=".length()));
            } else if (a.equals("--no-recover")) {
                recover = false;
            } else {
                System.err.println("Unknown option: " + a);
                usage();
                return;
            }
        }
        if (pages < 0) {
            pages = sessions * PAGES_PER_SESSION;
        }

        Database.getCatalog().loadSchema(catalogFile);
        TableStats.computeStatistics();
        Database.resetBufferPool(pages);

        if (recover) {
            // Pages are flushed before the commit record is written, so a crash
            // in that window can leave a transaction's pages on disk with no
            // commit record. The log's recovery pass undoes exactly those. It is
            // a no-op on a log with no records in it.
            Database.getLogFile().recover();
        }

        ServerSocketChannel server = bind(address);
        Path socketPath = isPath(address) ? Paths.get(address) : null;

        // From here on the parser's output belongs to whichever session
        // produced it, so System.out must stop being a shared destination.
        // Anything this class prints for the operator goes to stderr.
        StatementSession.installCapture();

        System.err.println("SocketRunner listening on " + address
                + " (sessions=" + sessions + ", pages=" + pages + ")");

        ExecutorService pool = Executors.newFixedThreadPool(sessions);
        AtomicInteger nextId = new AtomicInteger(1);
        installShutdownHook(server, pool, socketPath);

        try {
            while (true) {
                SocketChannel client = server.accept();
                int id = nextId.getAndIncrement();
                pool.submit(() -> serve(client, id));
            }
        } catch (IOException e) {
            // accept() fails once the shutdown hook closes the channel; that is
            // the normal exit path, not a fault worth reporting.
        }
    }

    /**
     * Run one client's session to completion: greet, then answer statements
     * one at a time until the client hangs up.
     */
    private static void serve(SocketChannel client, int id) {
        StatementSession session = new StatementSession();
        try (SocketChannel c = client) {
            BufferedReader in = new BufferedReader(new InputStreamReader(
                    Channels.newInputStream(c), StandardCharsets.UTF_8));
            PrintStream out = utf8(Channels.newOutputStream(c));

            out.println("<<READY>>");
            out.flush();

            String line;
            while ((line = in.readLine()) != null) {
                line = line.trim();
                if (line.isEmpty()) {
                    continue;
                }
                StatementSession.Result r = session.execute(line);
                out.print(r.body);
                out.println(r.status.marker());
                out.flush();
            }
        } catch (IOException e) {
            System.err.println("session " + id + " ended: " + e);
        } finally {
            // Runs whether the client hung up cleanly or the socket broke: an
            // open transaction holds page locks that would otherwise block
            // every other session until this process restarts.
            if (session.hasOpenTransaction()) {
                System.err.println("session " + id
                        + " disconnected mid-transaction; rolling back");
            }
            session.close();
        }
    }

    private static ServerSocketChannel bind(String address) throws IOException {
        if (!isPath(address)) {
            int port = parsePositive("port", address);
            ServerSocketChannel server = ServerSocketChannel.open();
            server.bind(new InetSocketAddress(InetAddress.getLoopbackAddress(), port));
            return server;
        }

        Path path = Paths.get(address);
        if (Files.isRegularFile(path) || Files.isDirectory(path)) {
            throw new IOException(path + " exists and is not a socket; "
                    + "refusing to remove it");
        }
        // A socket file left behind by a previous run would make bind() fail.
        Files.deleteIfExists(path);
        ServerSocketChannel server = ServerSocketChannel.open(StandardProtocolFamily.UNIX);
        server.bind(UnixDomainSocketAddress.of(path));
        return server;
    }

    /** Treat anything that looks like a filesystem path as a Unix socket. */
    private static boolean isPath(String address) {
        return address.indexOf('/') >= 0;
    }

    /**
     * Stop accepting, let in-flight statements finish, then flush. Dirty pages
     * only reach disk at commit, so an interrupted statement loses nothing,
     * but a session sitting between statements may hold pages flushed by an
     * earlier mid-transaction eviction.
     */
    private static void installShutdownHook(ServerSocketChannel server,
                                            ExecutorService pool,
                                            Path socketPath) {
        Runtime.getRuntime().addShutdownHook(new Thread(() -> {
            try {
                server.close();
            } catch (IOException ignored) {
                // Closing is best-effort; the process is going away regardless.
            }
            pool.shutdownNow();
            try {
                pool.awaitTermination(5, TimeUnit.SECONDS);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
            try {
                Database.getBufferPool().flushAllPages();
            } catch (IOException e) {
                System.err.println("failed to flush pages on shutdown: " + e);
            }
            if (socketPath != null) {
                try {
                    Files.deleteIfExists(socketPath);
                } catch (IOException ignored) {
                    // A stale socket file is removed by the next bind() anyway.
                }
            }
        }, "socketrunner-shutdown"));
    }

    private static PrintStream utf8(java.io.OutputStream os) {
        try {
            return new PrintStream(os, false, "UTF-8");
        } catch (UnsupportedEncodingException e) {
            throw new AssertionError("UTF-8 is always supported", e);
        }
    }

    private static int parsePositive(String what, String value) {
        int n;
        try {
            n = Integer.parseInt(value);
        } catch (NumberFormatException e) {
            throw new IllegalArgumentException(what + ": not a number: " + value);
        }
        if (n <= 0) {
            throw new IllegalArgumentException(what + ": must be positive: " + value);
        }
        return n;
    }

    private static void usage() {
        System.err.println("Usage: SocketRunner <catalogFile> <socketPath|port>"
                + " [--sessions=N] [--pages=N] [--no-recover]");
        System.exit(1);
    }
}
