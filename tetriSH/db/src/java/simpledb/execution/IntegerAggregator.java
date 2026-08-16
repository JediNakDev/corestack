package simpledb.execution;

import simpledb.common.Type;
import simpledb.storage.Tuple;
import simpledb.storage.TupleDesc;
import simpledb.storage.TupleIterator;
import simpledb.storage.IntField;
import simpledb.storage.Field;

import java.util.HashMap;
import java.util.Map;
import java.util.ArrayList;
import java.util.List;

/**
 * Knows how to compute some aggregate over a set of IntFields.
 */
public class IntegerAggregator implements Aggregator {

    private static final long serialVersionUID = 1L;

    private int gbfield;
    private Type gbfieldtype;
    private int afield;
    private Op what;

    private boolean grouping;
    private Map<Field, Integer> aggVals;
    private Map<Field, Integer> counts;

    private static final Field NO_GROUP_KEY = new IntField(0);

    /**
     * Aggregate constructor
     * 
     * @param gbfield
     *                    the 0-based index of the group-by field in the tuple, or
     *                    NO_GROUPING if there is no grouping
     * @param gbfieldtype
     *                    the type of the group by field (e.g., Type.INT_TYPE), or
     *                    null
     *                    if there is no grouping
     * @param afield
     *                    the 0-based index of the aggregate field in the tuple
     * @param what
     *                    the aggregation operator
     */

    public IntegerAggregator(int gbfield, Type gbfieldtype, int afield, Op what) {
        this.gbfield = gbfield;
        this.gbfieldtype = gbfieldtype;
        this.afield = afield;
        this.what = what;
        this.grouping = gbfield != NO_GROUPING;
        this.aggVals = new HashMap<>();
        this.counts = new HashMap<>();
    }

    /**
     * Merge a new tuple into the aggregate, grouping as indicated in the
     * constructor
     * 
     * @param tup
     *            the Tuple containing an aggregate field and a group-by field
     */
    public void mergeTupleIntoGroup(Tuple tup) {
        Field key = (!grouping) ? NO_GROUP_KEY : tup.getField(gbfield);
        int val = ((IntField) tup.getField(afield)).getValue();

        counts.merge(key, 1, Integer::sum);

        if (!aggVals.containsKey(key)) {
            aggVals.put(key, val);
        } else {
            int cur = aggVals.get(key);
            switch (what) {
                case MIN:
                    aggVals.put(key, Math.min(cur, val));
                    break;
                case MAX:
                    aggVals.put(key, Math.max(cur, val));
                    break;
                case SUM:
                case AVG:
                    aggVals.put(key, cur + val);
                    break;
                default:
                    break;
            }
        }
    }

    /**
     * Create a OpIterator over group aggregate results.
     * 
     * @return a OpIterator whose tuples are the pair (groupVal, aggregateVal)
     *         if using group, or a single (aggregateVal) if no grouping. The
     *         aggregateVal is determined by the type of aggregate specified in
     *         the constructor.
     */
    public OpIterator iterator() {
        TupleDesc td = grouping
                ? new TupleDesc(new Type[] { gbfieldtype, Type.INT_TYPE })
                : new TupleDesc(new Type[] { Type.INT_TYPE });
        List<Tuple> tuples = new ArrayList<>();
        for (Field key : aggVals.keySet()) {
            int result;
            switch (what) {
                case COUNT:
                    result = counts.get(key);
                    break;
                case AVG:
                    result = aggVals.get(key) / counts.get(key);
                    break;
                default:
                    result = aggVals.get(key);
                    break;
            }

            Tuple t = new Tuple(td);
            if (grouping) {
                t.setField(0, key);
                t.setField(1, new IntField(result));
            } else {
                t.setField(0, new IntField(result));
            }
            tuples.add(t);
        }
        return new TupleIterator(td, tuples);
    }

}
