package simpledb.optimizer;

import simpledb.common.Database;
import simpledb.common.Type;
import simpledb.execution.Predicate;
import simpledb.execution.SeqScan;
import simpledb.storage.*;
import simpledb.transaction.Transaction;

import java.util.HashMap;
import java.util.Iterator;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ConcurrentMap;

/**
 * TableStats represents statistics (e.g., histograms) about base tables in a
 * query. 
 * 
 * This class is not needed in implementing lab1 and lab2.
 */
public class TableStats {

    private static final ConcurrentMap<String, TableStats> statsMap = new ConcurrentHashMap<>();

    static final int IOCOSTPERPAGE = 1000;

    public static TableStats getTableStats(String tablename) {
        return statsMap.get(tablename);
    }

    public static void setTableStats(String tablename, TableStats stats) {
        statsMap.put(tablename, stats);
    }
    
    public static void setStatsMap(Map<String,TableStats> s)
    {
        try {
            java.lang.reflect.Field statsMapF = TableStats.class.getDeclaredField("statsMap");
            statsMapF.setAccessible(true);
            statsMapF.set(null, s);
        } catch (NoSuchFieldException | IllegalAccessException | IllegalArgumentException | SecurityException e) {
            e.printStackTrace();
        }

    }

    public static Map<String, TableStats> getStatsMap() {
        return statsMap;
    }

    public static void computeStatistics() {
        Iterator<Integer> tableIt = Database.getCatalog().tableIdIterator();

        System.out.println("Computing table stats.");
        while (tableIt.hasNext()) {
            int tableid = tableIt.next();
            TableStats s = new TableStats(tableid, IOCOSTPERPAGE);
            setTableStats(Database.getCatalog().getTableName(tableid), s);
        }
        System.out.println("Done.");
    }

    /**
     * Number of bins for the histogram. Feel free to increase this value over
     * 100, though our tests assume that you have at least 100 bins in your
     * histograms.
     */
    static final int NUM_HIST_BINS = 100;

    private final int ioCostPerPage;
    private final int numPages;
    private final TupleDesc td;
    private int numTuples;
    private final Map<Integer, IntHistogram> intHistograms = new HashMap<>();
    private final Map<Integer, StringHistogram> stringHistograms = new HashMap<>();

    /**
     * Create a new TableStats object, that keeps track of statistics on each
     * column of a table
     *
     * @param tableid
     *            The table over which to compute statistics
     * @param ioCostPerPage
     *            The cost per page of IO. This doesn't differentiate between
     *            sequential-scan IO and disk seeks.
     */
    public TableStats(int tableid, int ioCostPerPage) {
        this.ioCostPerPage = ioCostPerPage;
        DbFile file = Database.getCatalog().getDatabaseFile(tableid);
        this.numPages = ((HeapFile) file).numPages();
        this.td = file.getTupleDesc();

        // An integer histogram needs its range up front, so take one pass to find the
        // per-column extrema and a second to populate the buckets.
        int[] mins = new int[td.numFields()];
        int[] maxs = new int[td.numFields()];
        for (int i = 0; i < td.numFields(); i++) {
            mins[i] = Integer.MAX_VALUE;
            maxs[i] = Integer.MIN_VALUE;
        }

        Transaction t = new Transaction();
        t.start();
        try {
            SeqScan scan = new SeqScan(t.getId(), tableid);
            scan.open();
            while (scan.hasNext()) {
                Tuple tup = scan.next();
                numTuples++;
                for (int i = 0; i < td.numFields(); i++) {
                    if (td.getFieldType(i) == Type.INT_TYPE) {
                        int v = ((IntField) tup.getField(i)).getValue();
                        mins[i] = Math.min(mins[i], v);
                        maxs[i] = Math.max(maxs[i], v);
                    }
                }
            }
            scan.close();

            for (int i = 0; i < td.numFields(); i++) {
                if (td.getFieldType(i) == Type.INT_TYPE) {
                    // an empty table still needs a usable range
                    int lo = mins[i] == Integer.MAX_VALUE ? 0 : mins[i];
                    int hi = maxs[i] == Integer.MIN_VALUE ? 0 : maxs[i];
                    intHistograms.put(i, new IntHistogram(NUM_HIST_BINS, lo, hi));
                } else {
                    stringHistograms.put(i, new StringHistogram(NUM_HIST_BINS));
                }
            }

            scan.rewind();
            while (scan.hasNext()) {
                Tuple tup = scan.next();
                for (int i = 0; i < td.numFields(); i++) {
                    if (td.getFieldType(i) == Type.INT_TYPE) {
                        intHistograms.get(i).addValue(((IntField) tup.getField(i)).getValue());
                    } else {
                        stringHistograms.get(i).addValue(((StringField) tup.getField(i)).getValue());
                    }
                }
            }
            scan.close();
            t.commit();
        } catch (Exception e) {
            throw new RuntimeException("failed to compute stats for table " + tableid, e);
        }
    }

    /**
     * Estimates the cost of sequentially scanning the file, given that the cost
     * to read a page is costPerPageIO. You can assume that there are no seeks
     * and that no pages are in the buffer pool.
     * 
     * Also, assume that your hard drive can only read entire pages at once, so
     * if the last page of the table only has one tuple on it, it's just as
     * expensive to read as a full page. (Most real hard drives can't
     * efficiently address regions smaller than a page at a time.)
     * 
     * @return The estimated cost of scanning the table.
     */
    public double estimateScanCost() {
        return (double) numPages * ioCostPerPage;
    }

    /**
     * This method returns the number of tuples in the relation, given that a
     * predicate with selectivity selectivityFactor is applied.
     * 
     * @param selectivityFactor
     *            The selectivity of any predicates over the table
     * @return The estimated cardinality of the scan with the specified
     *         selectivityFactor
     */
    public int estimateTableCardinality(double selectivityFactor) {
        return (int) (numTuples * selectivityFactor);
    }

    /**
     * The average selectivity of the field under op.
     * @param field
     *        the index of the field
     * @param op
     *        the operator in the predicate
     * The semantic of the method is that, given the table, and then given a
     * tuple, of which we do not know the value of the field, return the
     * expected selectivity. You may estimate this value from the histograms.
     * */
    public double avgSelectivity(int field, Predicate.Op op) {
        if (td.getFieldType(field) == Type.INT_TYPE) {
            return intHistograms.get(field).avgSelectivity();
        }
        return stringHistograms.get(field).avgSelectivity();
    }

    /**
     * Estimate the selectivity of predicate <tt>field op constant</tt> on the
     * table.
     * 
     * @param field
     *            The field over which the predicate ranges
     * @param op
     *            The logical operation in the predicate
     * @param constant
     *            The value against which the field is compared
     * @return The estimated selectivity (fraction of tuples that satisfy) the
     *         predicate
     */
    public double estimateSelectivity(int field, Predicate.Op op, Field constant) {
        if (td.getFieldType(field) == Type.INT_TYPE) {
            return intHistograms.get(field).estimateSelectivity(op, ((IntField) constant).getValue());
        }
        return stringHistograms.get(field).estimateSelectivity(op, ((StringField) constant).getValue());
    }

    /**
     * return the total number of tuples in this table
     * */
    public int totalTuples() {
        return numTuples;
    }

}
