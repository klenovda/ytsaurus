package ru.yandex.yt.ytclient.wire;

import ru.yandex.yt.ytclient.tables.ColumnValueType;
import ru.yandex.yt.ytclient.ytree.YTreeConsumer;

public class VersionedValue extends UnversionedValue {
    private final long timestamp;

    public VersionedValue(int id, ColumnValueType type, boolean aggregate, Object value, long timestamp) {
        super(id, type, aggregate, value);
        this.timestamp = timestamp;
    }

    public long getTimestamp() {
        return timestamp;
    }

    @Override
    public boolean equals(Object o) {
        if (this == o) {
            return true;
        }
        if (!(o instanceof VersionedValue)) {
            return false;
        }
        if (!super.equals(o)) {
            return false;
        }

        VersionedValue that = (VersionedValue) o;

        return timestamp == that.timestamp;
    }

    @Override
    public int hashCode() {
        int result = super.hashCode();
        result = 31 * result + (int) (timestamp ^ (timestamp >>> 32));
        return result;
    }

    @Override
    public String toString() {
        return "VersionedValue{" + super.toString() +
                ", timestamp=" + timestamp +
                '}';
    }

    @Override
    public void writeTo(YTreeConsumer consumer) {
        consumer.onBeginAttributes();
        consumer.onKeyedItem("timestamp");
        consumer.onUint64Scalar(timestamp);
        consumer.onKeyedItem("aggregate");
        consumer.onBooleanScalar(isAggregate());
        consumer.onEndAttributes();
        super.writeTo(consumer);
    }
}
