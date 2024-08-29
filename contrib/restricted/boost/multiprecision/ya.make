# Generated by devtools/yamaker from nixpkgs 22.11.

LIBRARY()

LICENSE(BSL-1.0)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

VERSION(1.86.0)

ORIGINAL_SOURCE(https://github.com/boostorg/multiprecision/archive/boost-1.86.0.tar.gz)

PEERDIR(
    contrib/restricted/boost/assert
    contrib/restricted/boost/config
    contrib/restricted/boost/core
    contrib/restricted/boost/integer
    contrib/restricted/boost/lexical_cast
    contrib/restricted/boost/math
    contrib/restricted/boost/random
)

ADDINCL(
    GLOBAL contrib/restricted/boost/multiprecision/include
)

NO_COMPILER_WARNINGS()

NO_UTIL()

END()
