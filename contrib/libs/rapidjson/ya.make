LIBRARY()

NO_UTIL()

LICENSE(
    BSD-3-Clause AND
    JSON AND
    MIT
)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

VERSION(1.1.0)

ADDINCL(
    contrib/libs/rapidjson/include
)

END()
