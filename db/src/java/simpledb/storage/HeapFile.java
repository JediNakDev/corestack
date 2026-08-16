package simpledb.storage;

import simpledb.common.Database;
import simpledb.common.DbException;
import simpledb.common.Debug;
import simpledb.common.Permissions;
import simpledb.transaction.TransactionAbortedException;
import simpledb.transaction.TransactionId;

import java.io.*;
import java.util.*;

/**
 * HeapFile is an implementation of a DbFile that stores a collection of tuples
 * in no particular order. Tuples are stored on pages, each of which is a fixed
 * size, and the file is simply a collection of those pages. HeapFile works
 * closely with HeapPage. The format of HeapPages is described in the HeapPage
 * constructor.
 * 
 * @see HeapPage#HeapPage
 * @author Sam Madden
 */
public class HeapFile implements DbFile {

    private final File f;
    private final TupleDesc td;

    /**
     * Constructs a heap file backed by the specified file.
     * 
     * @param f
     *          the file that stores the on-disk backing store for this heap
     *          file.
     */
    public HeapFile(File f, TupleDesc td) {
        this.f = f;
        this.td = td;
    }

    /**
     * Returns the File backing this HeapFile on disk.
     * 
     * @return the File backing this HeapFile on disk.
     */
    public File getFile() {
        return f;
    }

    /**
     * Returns an ID uniquely identifying this HeapFile. Implementation note:
     * you will need to generate this tableid somewhere to ensure that each
     * HeapFile has a "unique id," and that you always return the same value for
     * a particular HeapFile. We suggest hashing the absolute file name of the
     * file underlying the heapfile, i.e. f.getAbsoluteFile().hashCode().
     * 
     * @return an ID uniquely identifying this HeapFile.
     */
    public int getId() {
        return f.getAbsoluteFile().hashCode();
    }

    /**
     * Returns the TupleDesc of the table stored in this DbFile.
     * 
     * @return TupleDesc of this DbFile.
     */
    public TupleDesc getTupleDesc() {
        return td;
    }

    // see DbFile.java for javadocs
    public Page readPage(PageId pid) {
        int pageSize = BufferPool.getPageSize();
        long offset = (long) pid.getPageNumber() * pageSize;
        byte[] data = new byte[pageSize];

        try (RandomAccessFile raf = new RandomAccessFile(f, "r")) {
            raf.seek(offset);
            raf.readFully(data);
            return new HeapPage((HeapPageId) pid, data);
        } catch (IOException e) {
            throw new RuntimeException("Failed to read page " + pid.getPageNumber(), e);
        }
    }

    // see DbFile.java for javadocs
    public void writePage(Page page) throws IOException {
        int pageSize = BufferPool.getPageSize();
        long offset = (long) page.getId().getPageNumber() * pageSize;
        try (RandomAccessFile raf = new RandomAccessFile(f, "rw")) {
            raf.seek(offset);
            raf.write(page.getPageData());
        }
    }

    /**
     * Returns the number of pages in this HeapFile.
     */
    public int numPages() {
        return (int) Math.ceil((double) f.length() / BufferPool.getPageSize());
    }

    // see DbFile.java for javadocs
    public List<Page> insertTuple(TransactionId tid, Tuple t)
            throws DbException, IOException, TransactionAbortedException {
        ArrayList<Page> modified = new ArrayList<>();
        BufferPool bp = Database.getBufferPool();

        for (int i = 0; i < numPages(); i++) {
            HeapPageId pid = new HeapPageId(getId(), i);
            boolean heldBefore = bp.holdsLock(tid, pid);
            HeapPage page = (HeapPage) bp.getPage(tid, pid, Permissions.READ_ONLY);
            if (page.getNumEmptySlots() > 0) {
                page = (HeapPage) bp.getPage(tid, pid, Permissions.READ_WRITE);
                page.insertTuple(t);
                modified.add(page);
                return modified;
            } else if (!heldBefore) {
                bp.unsafeReleasePage(tid, pid);
            }
        }

        HeapPageId newPid;
        synchronized (this) {
            newPid = new HeapPageId(getId(), numPages());
            HeapPage newPage = new HeapPage(newPid, HeapPage.createEmptyPageData());
            writePage(newPage);
        }
        HeapPage page = (HeapPage) bp.getPage(tid, newPid, Permissions.READ_WRITE);
        page.insertTuple(t);
        modified.add(page);
        return modified;
    }

    // see DbFile.java for javadocs
    public ArrayList<Page> deleteTuple(TransactionId tid, Tuple t) throws DbException,
            TransactionAbortedException {
        ArrayList<Page> modified = new ArrayList<>();
        RecordId rid = t.getRecordId();
        if (rid == null || rid.getPageId().getTableId() != getId()) {
            throw new DbException("tuple is not a member of this file");
        }
        HeapPage page = (HeapPage) Database.getBufferPool()
                .getPage(tid, rid.getPageId(), Permissions.READ_WRITE);
        page.deleteTuple(t);
        modified.add(page);
        return modified;
    }

    private class HeapFileIterator implements DbFileIterator {

        private TransactionId tid;
        private int pageNo;
        private Iterator<Tuple> tupleIt;

        public HeapFileIterator(TransactionId tid) {
            this.tid = tid;
        }

        /**
         * Opens the iterator
         * 
         * @throws DbException when there are problems opening/accessing the database.
         */
        public void open() throws DbException, TransactionAbortedException {
            pageNo = 0;
            tupleIt = getPageTuples(pageNo);
        }

        private Iterator<Tuple> getPageTuples(int pgNo) throws DbException, TransactionAbortedException {
            HeapPageId pid = new HeapPageId(getId(), pgNo);
            HeapPage page = (HeapPage) Database.getBufferPool()
                    .getPage(tid, pid, Permissions.READ_ONLY);
            return page.iterator();
        }

        /**
         * @return true if there are more tuples available, false if no more tuples or
         *         iterator isn't open.
         */
        public boolean hasNext() throws DbException, TransactionAbortedException {
            if (tupleIt == null)
                return false;
            while (!tupleIt.hasNext()) {
                if (pageNo >= numPages() - 1)
                    return false;
                pageNo++;
                tupleIt = getPageTuples(pageNo);
            }
            return true;
        }

        /**
         * Gets the next tuple from the operator (typically implementing by reading
         * from a child operator or an access method).
         *
         * @return The next tuple in the iterator.
         * @throws NoSuchElementException if there are no more tuples
         */
        public Tuple next() throws DbException, TransactionAbortedException, NoSuchElementException {
            if (!hasNext())
                throw new NoSuchElementException();
            return tupleIt.next();
        }

        /**
         * Resets the iterator to the start.
         * 
         * @throws DbException When rewind is unsupported.
         */
        public void rewind() throws DbException, TransactionAbortedException {
            open();
        }

        /**
         * Closes the iterator.
         */
        public void close() {
            tupleIt = null;
        }
    }

    // see DbFile.java for javadocs
    public DbFileIterator iterator(TransactionId tid) {
        return new HeapFileIterator(tid);
    }

}
