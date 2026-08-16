package simpledb.storage;

import simpledb.common.Permissions;
import simpledb.transaction.TransactionAbortedException;
import simpledb.transaction.TransactionId;

import java.util.ArrayDeque;
import java.util.Collections;
import java.util.Deque;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Iterator;
import java.util.Map;
import java.util.Set;

public class LockManager {

    private static class LockState {
        final Set<TransactionId> holders = new HashSet<>();
        boolean exclusive = false;
    }

    private static final long RETRY_INTERVAL_MS = 10;

    private final Map<PageId, LockState> locks = new HashMap<>();
    private final Map<TransactionId, PageId> waitingFor = new HashMap<>();

    /**
     * Acquire the requested lock on {@code pid} for {@code tid}, blocking until
     * it can be granted.
     *
     * @param tid  the transaction requesting the lock
     * @param pid  the page to lock
     * @param perm READ_ONLY for a shared lock, READ_WRITE for an exclusive lock
     * @throws TransactionAbortedException if the waiting thread is interrupted
     */
    public void acquire(TransactionId tid, PageId pid, Permissions perm)
            throws TransactionAbortedException {
        boolean exclusive = perm == Permissions.READ_WRITE;
        while (true) {
            synchronized (this) {
                if (tryAcquire(tid, pid, exclusive)) {
                    waitingFor.remove(tid);
                    return;
                }
                waitingFor.put(tid, pid);
                if (hasDeadlock(tid)) {
                    waitingFor.remove(tid);
                    throw new TransactionAbortedException();
                }
            }
            try {
                Thread.sleep(RETRY_INTERVAL_MS);
            } catch (InterruptedException e) {
                synchronized (this) {
                    waitingFor.remove(tid);
                }
                throw new TransactionAbortedException();
            }
        }
    }

    /**
     * @return true if {@code start} is part of a cycle in the wait-for graph,
     *         i.e. following "waits-for" edges from it leads back to itself.
     */
    private boolean hasDeadlock(TransactionId start) {
        Set<TransactionId> visited = new HashSet<>();
        Deque<TransactionId> frontier = new ArrayDeque<>(waitEdges(start));
        while (!frontier.isEmpty()) {
            TransactionId t = frontier.poll();
            if (t.equals(start)) {
                return true;
            }
            if (visited.add(t)) {
                frontier.addAll(waitEdges(t));
            }
        }
        return false;
    }

    /**
     * The transactions {@code t} is currently waiting for (holders of its page).
     */
    private Set<TransactionId> waitEdges(TransactionId t) {
        PageId pid = waitingFor.get(t);
        if (pid == null) {
            return Collections.emptySet();
        }
        LockState state = locks.get(pid);
        if (state == null) {
            return Collections.emptySet();
        }
        Set<TransactionId> holders = new HashSet<>(state.holders);
        holders.remove(t);
        return holders;
    }

    /**
     * Attempt to grant the lock without blocking.
     *
     * @return true if the lock is now held by {@code tid}; false if it conflicts
     *         with locks held by other transactions.
     */
    private synchronized boolean tryAcquire(TransactionId tid, PageId pid,
            boolean exclusive) {
        LockState state = locks.get(pid);

        if (state == null || state.holders.isEmpty()) {
            if (state == null) {
                state = new LockState();
                locks.put(pid, state);
            }
            state.holders.add(tid);
            state.exclusive = exclusive;
            return true;
        }

        if (state.holders.contains(tid)) {
            if (state.exclusive || !exclusive) {
                return true;
            }
            if (state.holders.size() == 1) {
                state.exclusive = true;
                return true;
            }
            return false;
        }

        if (!exclusive && !state.exclusive) {
            state.holders.add(tid);
            return true;
        }
        return false;
    }

    /**
     * Release {@code tid}'s lock on {@code pid}, if it holds one.
     */
    public synchronized void release(TransactionId tid, PageId pid) {
        LockState state = locks.get(pid);
        if (state == null) {
            return;
        }
        state.holders.remove(tid);
        if (state.holders.isEmpty()) {
            state.exclusive = false;
            locks.remove(pid);
        }
    }

    /**
     * @return true if {@code tid} holds any lock (shared or exclusive) on
     *         {@code pid}.
     */
    public synchronized boolean holdsLock(TransactionId tid, PageId pid) {
        LockState state = locks.get(pid);
        return state != null && state.holders.contains(tid);
    }

    /**
     * @return every page {@code tid} currently holds a lock on.
     */
    public synchronized Set<PageId> pagesHeldBy(TransactionId tid) {
        Set<PageId> held = new HashSet<>();
        for (Map.Entry<PageId, LockState> e : locks.entrySet()) {
            if (e.getValue().holders.contains(tid)) {
                held.add(e.getKey());
            }
        }
        return held;
    }

    /**
     * Release every lock held by {@code tid}.
     */
    public synchronized void releaseAll(TransactionId tid) {
        Iterator<Map.Entry<PageId, LockState>> it = locks.entrySet().iterator();
        while (it.hasNext()) {
            LockState state = it.next().getValue();
            if (state.holders.remove(tid) && state.holders.isEmpty()) {
                state.exclusive = false;
                it.remove();
            }
        }
    }
}
