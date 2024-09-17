# Generated by devtools/yamaker.

PY3_LIBRARY()

VERSION(3.12.6)

ORIGINAL_SOURCE(https://github.com/python/cpython/archive/v3.12.6.tar.gz)

LICENSE(Python-2.0)

PEERDIR(
    contrib/libs/sqlite3
)

ADDINCL(
    contrib/libs/sqlite3
    contrib/tools/python3/Include
    contrib/tools/python3/Include/internal
)

NO_COMPILER_WARNINGS()

NO_RUNTIME()

SRCS(
    blob.c
    connection.c
    cursor.c
    microprotocols.c
    module.c
    prepare_protocol.c
    row.c
    statement.c
    util.c
)

PY_REGISTER(
    _sqlite3
)

END()
