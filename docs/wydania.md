# Wydania

Obrazy budują się w GitHub Actions (`.github/workflows/docker.yml`):

* każdy commit na `main` → `ghcr.io/goral64/zigbee2supla:main`,
* każdy tag `vX.Y.Z` (także `vX.Y.Z-beta.N`) → obraz samodzielny
  `:X.Y.Z` (i `:X.Y` dla wydań bez przyrostka) oraz `:latest`, a także
  obrazy dodatku Home Assistant `zigbee2supla-addon-{amd64,aarch64,armv7}:X.Y.Z`
  i wydanie na GitHubie z paczką `zigbee2supla-docker.tar.gz` (`z2s.sh`
  i pliki compose, `tools/package_docker.sh`).

Dodatek HA nie buduje się na maszynie z Home Assistant: Supervisor pobiera
obraz o wersji z `zigbee2supla/config.yaml`. Wersja w tym pliku musi więc
być wersją istniejącego wydania.

## Procedura

1. W PR podbij wersję:
   * `CMakeLists.txt`: `project(zigbee2supla VERSION X.Y.Z …)` (bez
     przyrostka; trafia do wersji oprogramowania urządzeń w Supli, `z2s X.Y.Z`),
   * `zigbee2supla/config.yaml`: `version: "X.Y.Z"` albo
     `"X.Y.Z-beta.N"`, dokładnie jak tag bez `v`.
   * `zigbee2supla/CHANGELOG.md`: krótki wpis o tym, co się zmieniło
     (Home Assistant pokazuje go przy aktualizacji dodatku).
2. Scal PR i poczekaj, aż obraz `main` zbuduje się na wszystkich
   architekturach (wraz z testami, także na arm/v7 w emulacji, około
   40 min).
3. Wypchnij tag na ten commit:
   ```sh
   git tag -a vX.Y.Z -m vX.Y.Z <commit> && git push origin vX.Y.Z
   ```
4. Poczekaj na obrazy wydania i dodatku. Dopiero wtedy użytkownicy dodatku
   zobaczą aktualizację (Supervisor porównuje wersję z `config.yaml`
   w gałęzi `main` z zainstalowaną).

Między krokiem 1 a 4 dodatek z repozytorium wskazuje wersję, której obrazu
jeszcze nie ma; instalacja w tym czasie się nie uda. Dlatego tag wypycha
się zaraz po zbudowaniu `main`.
