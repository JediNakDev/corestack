package simpledb;

import simpledb.common.Database;
import simpledb.optimizer.TableStats;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.PrintStream;
import java.io.UnsupportedEncodingException;
import java.nio.charset.StandardCharsets;

/**
 * Single-client driver for the SimpleDB SQL parser, meant to be run as a child
 * process communicating over stdin/stdout pipes (e.g. from a C daemon). Unlike
 * {@link Parser#start}, this never touches jline/ConsoleReader, so it behaves
 * the same whether stdin is a TTY or a pipe.
 *
 * One process serves exactly one client, so two daemons that both need to
 * reach the same table cannot use this: each process has its own BufferPool
 * and its own LockManager, and neither can see the other's cached pages. Use
 * {@link SocketRunner} for that. PipeRunner remains the simpler option for a
 * daemon that owns its tables outright.
 *
 * Protocol: one SQL statement per line on stdin (terminated with ';'). After
 * each statement, all output produced by the parser is flushed to stdout
 * followed by a single marker line -- {@code <<END ok>>}, {@code <<END
 * error>>}, or {@code <<END retry>>}. On startup, {@code <<READY>>} is printed
 * once the catalog is loaded. On EOF, all dirty pages are flushed and the
 * process exits cleanly.
 *
 * @see StatementSession for how a statement's output is captured and its
 *      outcome classified
 */
public class PipeRunner {

    public static void main(String[] args) throws IOException {
        if (args.length < 1 || args.length > 2) {
            System.err.println("Usage: PipeRunner <catalogFile> [--no-recover]");
            System.exit(1);
        }
        boolean recover = true;
        if (args.length == 2) {
            if (!args[1].equals("--no-recover")) {
                System.err.println("Unknown option: " + args[1]);
                System.exit(1);
            }
            recover = false;
        }

        Database.getCatalog().loadSchema(args[0]);
        TableStats.computeStatistics();

        if (recover) {
            // Pages are flushed before the commit record is written, so a crash
            // in that window can leave a transaction's pages on disk with no
            // commit record. The log's recovery pass undoes exactly those. It is
            // a no-op on a log with no records in it.
            Database.getLogFile().recover();
        }

        PrintStream out = utf8(System.out);
        out.println("<<READY>>");
        out.flush();

        // Must happen after the greeting: this replaces System.out, and from
        // here on anything the parser prints belongs to a statement's response.
        StatementSession.installCapture();

        StatementSession session = new StatementSession();
        BufferedReader in = new BufferedReader(
                new InputStreamReader(System.in, StandardCharsets.UTF_8));

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

        session.close();
        Database.getBufferPool().flushAllPages();
        System.exit(0);
    }

    private static PrintStream utf8(java.io.OutputStream os) {
        try {
            return new PrintStream(os, false, "UTF-8");
        } catch (UnsupportedEncodingException e) {
            throw new AssertionError("UTF-8 is always supported", e);
        }
    }
}
