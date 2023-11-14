# Generated by devtools/yamaker (pypi).

PY3_LIBRARY()

VERSION(10.0.0)

LICENSE(BSD-3-Clause)

PEERDIR(
    contrib/python/jaraco.functools
    contrib/python/more-itertools
)

NO_LINT()

NO_CHECK_IMPORTS(
    cheroot.ssl.pyopenssl
    cheroot.testing
)

PY_SRCS(
    TOP_LEVEL
    cheroot/__init__.py
    cheroot/__init__.pyi
    cheroot/__main__.py
    cheroot/_compat.py
    cheroot/_compat.pyi
    cheroot/cli.py
    cheroot/cli.pyi
    cheroot/connections.py
    cheroot/connections.pyi
    cheroot/errors.py
    cheroot/errors.pyi
    cheroot/makefile.py
    cheroot/makefile.pyi
    cheroot/server.py
    cheroot/server.pyi
    cheroot/ssl/__init__.py
    cheroot/ssl/__init__.pyi
    cheroot/ssl/builtin.py
    cheroot/ssl/builtin.pyi
    cheroot/ssl/pyopenssl.py
    cheroot/ssl/pyopenssl.pyi
    cheroot/testing.py
    cheroot/testing.pyi
    cheroot/workers/__init__.py
    cheroot/workers/__init__.pyi
    cheroot/workers/threadpool.py
    cheroot/workers/threadpool.pyi
    cheroot/wsgi.py
    cheroot/wsgi.pyi
)

RESOURCE_FILES(
    PREFIX contrib/python/cheroot/py3/
    .dist-info/METADATA
    .dist-info/entry_points.txt
    .dist-info/top_level.txt
    cheroot/py.typed
)

END()
