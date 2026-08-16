package simpledb.optimizer;

import simpledb.execution.Predicate;

/** A class to represent a fixed-width histogram over a single integer-based field.
 */
public class IntHistogram {

    private final int min;
    private final int max;
    private final int width;
    private final int[] counts;
    private int ntups;

    /**
     * Create a new IntHistogram.
     *
     * This IntHistogram should maintain a histogram of integer values that it receives.
     * It should split the histogram into "buckets" buckets.
     *
     * The values that are being histogrammed will be provided one-at-a-time through the "addValue()" function.
     *
     * Your implementation should use space and have execution time that are both
     * constant with respect to the number of values being histogrammed.  For example, you shouldn't
     * simply store every value that you see in a sorted list.
     *
     * @param buckets The number of buckets to split the input value into.
     * @param min The minimum integer value that will ever be passed to this class for histogramming
     * @param max The maximum integer value that will ever be passed to this class for histogramming
     */
    public IntHistogram(int buckets, int min, int max) {
        this.min = min;
        this.max = max;
        // The range can approach 2^32, so it is computed in long arithmetic; doing it
        // in int overflows to a negative width and bucket count.
        long range = (long) max - (long) min + 1;
        // A bucket never spans less than one value, so asking for more buckets than
        // there are distinct values just gives one bucket per value.
        this.width = (int) Math.max(1L, (long) Math.ceil(range / (double) buckets));
        this.counts = new int[(int) Math.max(1L, (range + width - 1) / width)];
    }

    /**
     * @return the index of the bucket holding v, which must lie within [min, max]
     */
    private int bucketOf(int v) {
        long index = ((long) v - min) / width;
        return (int) Math.min(index, counts.length - 1);
    }

    /**
     * Add a value to the set of values that you are keeping a histogram of.
     * @param v Value to add to the histogram
     */
    public void addValue(int v) {
        if (v < min || v > max) {
            return;
        }
        counts[bucketOf(v)]++;
        ntups++;
    }

    /**
     * @return the estimated fraction of values equal to v
     */
    private double equalsSelectivity(int v) {
        if (ntups == 0 || v < min || v > max) {
            return 0.0;
        }
        // spread the bucket's count evenly over the values it covers
        return counts[bucketOf(v)] / (double) width / ntups;
    }

    /**
     * @return the estimated fraction of values strictly greater than v
     */
    private double greaterThanSelectivity(int v) {
        if (ntups == 0 || v >= max) {
            return 0.0;
        }
        if (v < min) {
            return 1.0;
        }
        int b = bucketOf(v);
        long bucketRight = (long) min + (long) (b + 1) * width - 1;
        // the part of v's own bucket lying above v, plus every bucket beyond it
        double matched = (bucketRight - v) / (double) width * counts[b];
        for (int i = b + 1; i < counts.length; i++) {
            matched += counts[i];
        }
        return matched / ntups;
    }

    /**
     * Estimate the selectivity of a particular predicate and operand on this table.
     *
     * For example, if "op" is "GREATER_THAN" and "v" is 5,
     * return your estimate of the fraction of elements that are greater than 5.
     *
     * @param op Operator
     * @param v Value
     * @return Predicted selectivity of this particular operator and value
     */
    public double estimateSelectivity(Predicate.Op op, int v) {
        switch (op) {
            case EQUALS:
            case LIKE:
                return equalsSelectivity(v);
            case NOT_EQUALS:
                return 1.0 - equalsSelectivity(v);
            case GREATER_THAN:
                return greaterThanSelectivity(v);
            case GREATER_THAN_OR_EQ:
                return greaterThanSelectivity(v) + equalsSelectivity(v);
            case LESS_THAN:
                return 1.0 - greaterThanSelectivity(v) - equalsSelectivity(v);
            case LESS_THAN_OR_EQ:
                return 1.0 - greaterThanSelectivity(v);
            default:
                throw new IllegalArgumentException("unsupported operator: " + op);
        }
    }

    /**
     * @return
     *     the average selectivity of this histogram.
     *
     *     This is not an indispensable method to implement the basic
     *     join optimization. It may be needed if you want to
     *     implement a more efficient optimization
     * */
    public double avgSelectivity()
    {
        if (ntups == 0) {
            return 0.0;
        }
        double sum = 0.0;
        for (int count : counts) {
            double fraction = count / (double) ntups;
            sum += fraction * fraction;
        }
        return sum;
    }

    /**
     * @return A string describing this histogram, for debugging purposes
     */
    public String toString() {
        StringBuilder sb = new StringBuilder("IntHistogram(min=" + min + ", max=" + max
                + ", width=" + width + ", ntups=" + ntups + ")");
        for (int i = 0; i < counts.length; i++) {
            sb.append("\n  [").append((long) min + (long) i * width).append(", ")
              .append(Math.min(max, (long) min + (long) (i + 1) * width - 1))
              .append("] -> ").append(counts[i]);
        }
        return sb.toString();
    }
}
