package simpledb;

import simpledb.common.Database;
import simpledb.transaction.Transaction;
import simpledb.transaction.TransactionAbortedException;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.PrintStream;
import java.io.UnsupportedEncodingException;
import java.nio.charset.StandardCharsets;

/**
 * One SQL session: a private {@link Parser} plus the machinery to capture
 * everything a statement prints and classify the outcome.
 *
 * A session is confined to one thread. {@link PipeRunner} owns exactly one;
 * {@link SocketRunner} owns one per connection. Parser is not thread-safe --
 * it keeps a single current transaction in a field -- so two threads sharing
 * a session would interleave each other's transactions.
 *
 * Sessions do share the process-wide {@link Database}: one Catalog, one
 * BufferPool, one LogFile. That is exactly what makes a multi-session process
 * correct, since the BufferPool's LockManager arbitrates between them.
 */
public class StatementSession {

    /** Outcome of a statement, reported to the client as an {@code <<END>>} marker. */
    public enum Status {
        /** Statement succeeded. */
        OK("<<END ok>>"),
        /** Statement failed for a reason resubmitting it will not fix. */
        ERROR("<<END error>>"),
        /**
         * The transaction was aborted to break a deadlock. Nothing is wrong
         * with the statement itself; the client should resubmit it.
         */
        RETRY("<<END retry>>");

        private final String marker;

        Status(String marker) {
            this.marker = marker;
        }

        public String marker() {
            return marker;
        }
    }

    /** A statement's captured output together with its outcome. */
    public static final class Result {
        public final Status status;
        /** Everything the statement printed, stdout followed by stderr. */
        public final String body;

        Result(Status status, String body) {
            this.status = status;
            this.body = body;
        }
    }

    /**
     * Failure messages Parser prints to stdout, anchored to the start of a
     * line so tab-separated result rows containing similar text can't
     * trigger them.
     */
    private static final String[] ERROR_LINE_PREFIXES = {
        "Invalid SQL expression",
        "Can't parse ",
    };

    private static ThreadRoutedPrintStream capturedOut;
    private static ThreadRoutedPrintStream capturedErr;

    /**
     * Replace System.out and System.err with per-thread routing streams, so
     * each session can collect its own statement's output even while other
     * sessions are printing concurrently. Idempotent; call once at startup
     * before any session runs.
     */
    public static synchronized void installCapture() {
        if (capturedOut != null) {
            return;
        }
        capturedOut = new ThreadRoutedPrintStream(System.out);
        capturedErr = new ThreadRoutedPrintStream(System.err);
        System.setOut(capturedOut);
        System.setErr(capturedErr);
    }

    private final Parser parser = new Parser();

    /**
     * Run one statement and collect everything it printed.
     *
     * Note that {@link Parser#processNextStatement} handles most failures
     * internally and only prints a message, so success cannot be told apart
     * by catching an exception. The outcome is decided from three signals:
     * an exception that did escape, any output on stderr (some Parser paths
     * only print a stack trace there), and the failure messages Parser
     * prints to stdout.
     *
     * @param sql one statement, terminated with ';'
     */
    public Result execute(String sql) {
        ByteArrayOutputStream outBuf = new ByteArrayOutputStream();
        ByteArrayOutputStream errBuf = new ByteArrayOutputStream();
        PrintStream out = printStream(outBuf);
        PrintStream err = printStream(errBuf);

        boolean threw = false;
        capturedOut.bind(out);
        capturedErr.bind(err);
        try {
            parser.processNextStatement(sql);
        } catch (Throwable t) {
            out.println("ERROR: " + t);
            threw = true;
        } finally {
            // Flush inside the binding: PrintStream buffers characters that a
            // print() with no newline left behind, and they must land in this
            // statement's buffer rather than the next thread's.
            out.flush();
            err.flush();
            capturedOut.unbind();
            capturedErr.unbind();
        }

        String stdout = decode(outBuf);
        String stderr = decode(errBuf);
        boolean failed = threw || !stderr.isEmpty() || hasErrorLine(stdout);

        Status status;
        if (!failed) {
            status = Status.OK;
        } else if (parser.getLastStatementFailure() instanceof TransactionAbortedException) {
            status = Status.RETRY;
        } else {
            status = Status.ERROR;
        }
        return new Result(status, join(stdout, stderr));
    }

    /**
     * Abandon this session, rolling back any transaction the client left
     * open. Without this a client that disconnects mid-transaction holds its
     * page locks forever and every other session eventually blocks on them.
     */
    public void close() {
        Transaction open = parser.getTransaction();
        if (open == null) {
            return;
        }
        parser.setTransaction(null);
        try {
            open.abort();
        } catch (IOException e) {
            // Nothing useful to do: the session is already going away, and the
            // log's own recovery pass will undo the transaction on restart.
            System.err.println("failed to abort transaction on disconnect: " + e);
        }
    }

    /** True if the client left a transaction open across statements. */
    public boolean hasOpenTransaction() {
        return parser.getTransaction() != null;
    }

    private static PrintStream printStream(ByteArrayOutputStream sink) {
        try {
            return new PrintStream(sink, false, "UTF-8");
        } catch (UnsupportedEncodingException e) {
            throw new AssertionError("UTF-8 is always supported", e);
        }
    }

    private static String decode(ByteArrayOutputStream buf) {
        return new String(buf.toByteArray(), StandardCharsets.UTF_8);
    }

    /** Concatenate the two captured streams, each newline-terminated. */
    private static String join(String stdout, String stderr) {
        StringBuilder sb = new StringBuilder();
        for (String part : new String[] { stdout, stderr }) {
            if (part.isEmpty()) {
                continue;
            }
            sb.append(part);
            if (!part.endsWith("\n")) {
                sb.append('\n');
            }
        }
        return sb.toString();
    }

    private static boolean hasErrorLine(String text) {
        for (String line : text.split("\n", -1)) {
            for (String prefix : ERROR_LINE_PREFIXES) {
                if (line.startsWith(prefix)) {
                    return true;
                }
            }
        }
        return false;
    }
}
