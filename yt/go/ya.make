IF (
    NOT
    OPENSOURCE
)
    RECURSE(
        bench
        discovery
        docker_registry
        pproflog
        tar2squash
        ytprof
        ytrecipe
    )
ENDIF()

RECURSE(
    blobtable
    bus
    compression
    crc64
    deps
    examples
    # genproto
    guid
    mapreduce
    migrate
    proto
    ratelimit
    row
    schema
    skiff
    wire
    ypath
    yson
    yt
    yterrors
    ytlock
    ytlog
    yttest
    ytwalk
)
