package ru.yandex.yt.ytclient.proxy;

import java.time.Duration;
import java.util.Objects;

import ru.yandex.yt.ytclient.misc.YtGuid;
import ru.yandex.yt.rpcproxy.ETransactionType;

/**
 * Опции для открытия транзакций
 */
public class ApiServiceTransactionOptions {
    private final ETransactionType type;
    private Duration timeout;
    private YtGuid id;
    private YtGuid parentId;
    private Boolean autoAbort;
    private Boolean ping;
    private Boolean pingAncestors;
    private Boolean sticky;

    public ApiServiceTransactionOptions(ETransactionType type) {
        this.type = Objects.requireNonNull(type);
    }

    public ETransactionType getType() {
        return type;
    }

    public Duration getTimeout() {
        return timeout;
    }

    public YtGuid getId() {
        return id;
    }

    public YtGuid getParentId() {
        return parentId;
    }

    public Boolean getAutoAbort() {
        return autoAbort;
    }

    public Boolean getPing() {
        return ping;
    }

    public Boolean getPingAncestors() {
        return pingAncestors;
    }

    public Boolean getSticky() {
        return sticky;
    }

    public ApiServiceTransactionOptions setTimeout(Duration timeout) {
        this.timeout = timeout;
        return this;
    }

    public ApiServiceTransactionOptions setId(YtGuid id) {
        this.id = id;
        return this;
    }

    public ApiServiceTransactionOptions setParentId(YtGuid parentId) {
        this.parentId = parentId;
        return this;
    }

    public ApiServiceTransactionOptions setAutoAbort(Boolean autoAbort) {
        this.autoAbort = autoAbort;
        return this;
    }

    public ApiServiceTransactionOptions setPing(Boolean ping) {
        this.ping = ping;
        return this;
    }

    public ApiServiceTransactionOptions setPingAncestors(Boolean pingAncestors) {
        this.pingAncestors = pingAncestors;
        return this;
    }

    public ApiServiceTransactionOptions setSticky(Boolean sticky) {
        this.sticky = sticky;
        return this;
    }
}
