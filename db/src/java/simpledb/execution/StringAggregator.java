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
 * Knows how to compute some aggregate over a set of StringFields.
 */
public class StringAggregator implements Aggregator {

    private static final long serialVersionUID = 1L;

    private int gbfield;
    private Type gbfieldtype;

    private boolean grouping;
    private Map<Field, Integer> counts;
    private static final Field NO_GROUP_KEY = new IntField(0);

    /**
     * Aggregate constructor
     * 
     * @param gbfield     the 0-based index of the group-by field in the tuple, or
     *                    NO_GROUPING if there is no grouping
     * @param gbfieldtype the type of the group by field (e.g., Type.INT_TYPE), or
     *                    null if there is no grouping
     * @param afield      the 0-based index of the aggregate field in the tuple
     * @param what        aggregation operator to use -- only supports COUNT
     * @throws IllegalArgumentException if what != COUNT
     */

    public StringAggregator(int gbfield, Type gbfieldtype, int afield, Op what) {
        if (what != Op.COUNT) {
            throw new IllegalArgumentException("Operator is not supported.");
        }
        this.gbfield = gbfield;
        this.gbfieldtype = gbfieldtype;
        this.grouping = gbfield != NO_GROUPING;
        this.counts = new HashMap<>();
    }

    /**
     * Merge a new tuple into the aggregate, grouping as indicated in the
     * constructor
     * 
     * @param tup the Tuple containing an aggregate field and a group-by field
     */
    public void mergeTupleIntoGroup(Tuple tup) {
        Field key = (!grouping) ? NO_GROUP_KEY : tup.getField(gbfield);
        counts.merge(key, 1, Integer::sum);
    }

    /**
     * Create a OpIterator over group aggregate results.
     *
     * @return a OpIterator whose tuples are the pair (groupVal,
     *         aggregateVal) if using group, or a single (aggregateVal) if no
     *         grouping. The aggregateVal is determined by the type of
     *         aggregate specified in the constructor.
     */
    public OpIterator iterator() {
        TupleDesc td = grouping
                ? new TupleDesc(new Type[] { gbfieldtype, Type.INT_TYPE })
                : new TupleDesc(new Type[] { Type.INT_TYPE });
        List<Tuple> tuples = new ArrayList<>();
        for (Field key : counts.keySet()) {
            int result = counts.get(key);

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
