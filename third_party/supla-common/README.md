# supla-common

Pliki protokołu SRPC skopiowane **bez zmian** z
[SUPLA/supla-device](https://github.com/SUPLA/supla-device), katalog
`src/supla-common`. Wersja źródłowa jest zapisana w pliku [`UPSTREAM`](UPSTREAM).

Licencja: GPL-2.0-or-later (AC SOFTWARE SP. Z O.O.).

`log.c` nie jest kopiowany – implementację `supla_log()` dostarcza
`core/src/log.cpp`.

**Nie edytuj tych plików ręcznie.** Aktualizacja:
`tools/update_supla_common.sh <tag>`, a potem `ctest` (w tym
`proto_contract_test`).
