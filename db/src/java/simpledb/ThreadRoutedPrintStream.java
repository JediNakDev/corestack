package simpledb;

import java.io.OutputStream;
import java.io.PrintStream;
import java.util.Locale;

/**
 * A {@link PrintStream} that delivers everything written on a given thread to
 * a destination that thread has claimed, falling back to a shared stream for
 * threads that have not claimed one.
 *
 * This exists because {@link Parser} and {@link simpledb.execution.Query}
 * print their output to {@code System.out}, a process-wide singleton, while
 * {@link SocketRunner} runs one session per thread and has to attribute every
 * byte to the right connection. Swapping {@code System.out} around each
 * statement only works while a single thread is executing statements at a
 * time, which is what {@link PipeRunner} guarantees and SocketRunner does not.
 *
 * Every printing method is overridden and delegated rather than routing
 * through this stream's own {@code OutputStream}. PrintStream buffers
 * characters internally on the way to its OutputStream and only flushes them
 * on {@code println}, so a {@code print()} with no trailing newline from one
 * thread would otherwise still be sitting in that shared buffer when another
 * thread's flush pushed it out, and would be delivered to the wrong
 * connection.
 */
public class ThreadRoutedPrintStream extends PrintStream {

    /** Discards writes; the superclass stream is never actually used. */
    private static final OutputStream SINK = new OutputStream() {
        @Override
        public void write(int b) {
        }
    };

    private final ThreadLocal<PrintStream> target = new ThreadLocal<>();
    private final PrintStream fallback;

    /**
     * @param fallback where output from threads with no binding goes; this
     *                 must be the real stream captured before this one was
     *                 installed via {@code System.setOut}, or writes recurse.
     */
    public ThreadRoutedPrintStream(PrintStream fallback) {
        super(SINK);
        this.fallback = fallback;
    }

    /** Route this thread's output to {@code s} until {@link #unbind}. */
    public void bind(PrintStream s) {
        target.set(s);
    }

    /** Send this thread's output back to the fallback stream. */
    public void unbind() {
        target.remove();
    }

    private PrintStream out() {
        PrintStream s = target.get();
        return s != null ? s : fallback;
    }

    @Override
    public void write(int b) {
        out().write(b);
    }

    @Override
    public void write(byte[] buf, int off, int len) {
        out().write(buf, off, len);
    }

    @Override
    public void print(boolean b) {
        out().print(b);
    }

    @Override
    public void print(char c) {
        out().print(c);
    }

    @Override
    public void print(int i) {
        out().print(i);
    }

    @Override
    public void print(long l) {
        out().print(l);
    }

    @Override
    public void print(float f) {
        out().print(f);
    }

    @Override
    public void print(double d) {
        out().print(d);
    }

    @Override
    public void print(char[] s) {
        out().print(s);
    }

    @Override
    public void print(String s) {
        out().print(s);
    }

    @Override
    public void print(Object obj) {
        out().print(obj);
    }

    @Override
    public void println() {
        out().println();
    }

    @Override
    public void println(boolean b) {
        out().println(b);
    }

    @Override
    public void println(char c) {
        out().println(c);
    }

    @Override
    public void println(int i) {
        out().println(i);
    }

    @Override
    public void println(long l) {
        out().println(l);
    }

    @Override
    public void println(float f) {
        out().println(f);
    }

    @Override
    public void println(double d) {
        out().println(d);
    }

    @Override
    public void println(char[] s) {
        out().println(s);
    }

    @Override
    public void println(String s) {
        out().println(s);
    }

    @Override
    public void println(Object obj) {
        out().println(obj);
    }

    @Override
    public PrintStream printf(String format, Object... args) {
        out().printf(format, args);
        return this;
    }

    @Override
    public PrintStream printf(Locale l, String format, Object... args) {
        out().printf(l, format, args);
        return this;
    }

    @Override
    public PrintStream format(String format, Object... args) {
        out().format(format, args);
        return this;
    }

    @Override
    public PrintStream format(Locale l, String format, Object... args) {
        out().format(l, format, args);
        return this;
    }

    @Override
    public PrintStream append(CharSequence csq) {
        out().append(csq);
        return this;
    }

    @Override
    public PrintStream append(CharSequence csq, int start, int end) {
        out().append(csq, start, end);
        return this;
    }

    @Override
    public PrintStream append(char c) {
        out().append(c);
        return this;
    }

    @Override
    public void flush() {
        out().flush();
    }

    @Override
    public boolean checkError() {
        return out().checkError();
    }

    /**
     * Deliberately does not close anything: this stream is installed as
     * System.out/System.err for the life of the process, and the per-thread
     * destinations are owned by whoever bound them.
     */
    @Override
    public void close() {
        flush();
    }
}
