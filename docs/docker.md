# Kontener Docker

zigbee2supla jest dostępny jako obraz Dockera. Jeden obraz obsługuje dwa
scenariusze:

| Scenariusz | Pliki | Rozdział |
|---|---|---|
| **obok supla-docker**: mostek jako usługa lokalnego serwera Supli na tej samej maszynie | `docker/supla-docker/` | 2 |
| **samodzielny**: mostek na osobnej maszynie; serwer Supli (lokalny lub Supla Cloud) i HA z zigbee2mqtt na innych | `docker/standalone/` | 3 |

Trzecią formą dystrybucji jest dodatek Home Assistant (katalog
`zigbee2supla/`, opis w README).

1. [Obraz](#1-obraz)
2. [Dołączenie do supla-docker](#2-dołączenie-do-supla-docker)
3. [Samodzielny kontener (Supla i HA na innych maszynach)](#3-samodzielny-kontener)
4. [Konfiguracja przez zmienne środowiskowe](#4-konfiguracja-przez-zmienne-środowiskowe)
5. [Certyfikat serwera (`Z2S_SUPLA_SECURITY_LEVEL`)](#5-certyfikat-serwera)
6. [Dane i kopia zapasowa](#6-dane-i-kopia-zapasowa)
7. [Rozwiązywanie problemów](#7-rozwiązywanie-problemów)

## 1. Obraz

| Tag | Opis |
|---|---|
| `ghcr.io/goral64/zigbee2supla:latest` | ostatnie wydanie |
| `ghcr.io/goral64/zigbee2supla:X.Y.Z`, `:X.Y` | konkretne wydanie |
| `ghcr.io/goral64/zigbee2supla:main` | bieżąca wersja rozwojowa |

Architektury: `amd64`, `arm64`, `arm/v7`. Obrazy publikuje GitHub Actions
(`.github/workflows/docker.yml`).

Obraz można też zbudować samemu (budowa uruchamia wszystkie testy;
obraz zbudowany pod tą nazwą compose użyje bez pobierania):

```sh
git clone https://github.com/Goral64/ZigBee2Supla.git
cd ZigBee2Supla
docker build -t ghcr.io/goral64/zigbee2supla:latest .
```

Obraz (`Dockerfile`, cel `runtime`) jest oparty na Alpine. Program działa
jako nieuprzywilejowany użytkownik `z2s`, a stan trzyma w `/data`.

## 2. Dołączenie do supla-docker

Kontener staje się jedną z usług supla-docker: startuje i zatrzymuje się
razem z nim (`./supla.sh start|stop`), działa w tej samej sieci Dockera
i łączy się z serwerem po nazwie `supla-server`.

1. Skopiuj do katalogu supla-docker (tam, gdzie jest `supla.sh`):
   * [`docker/supla-docker/docker-compose.zigbee2supla.yml`](../docker/supla-docker/docker-compose.zigbee2supla.yml),
   * [`docker/supla-docker/zigbee2supla.env.example`](../docker/supla-docker/zigbee2supla.env.example)
     jako `zigbee2supla.env`.
2. Uzupełnij `zigbee2supla.env`: co najmniej `Z2S_SUPLA_EMAIL` (konto
   w Twojej lokalnej Supla Cloud) i dane brokera MQTT, z którego korzysta
   zigbee2mqtt (`Z2S_MQTT_HOST`, `Z2S_MQTT_USERNAME`, `Z2S_MQTT_PASSWORD`).
3. W pliku `.env` supla-docker dopisz plik compose do `COMPOSE_FILE`:
   ```
   COMPOSE_FILE=docker-compose.yml:docker-compose.standalone.yml:docker-compose.zigbee2supla.yml
   ```
   (zachowaj pliki, które już tam są, np. `docker-compose.proxy.yml`).
4. W lokalnej Supla Cloud włącz **rejestrację nowych urządzeń**.
5. `./supla.sh start` i podgląd logów:
   ```sh
   docker logs -f supla-zigbee2supla
   ```

Co ustawia plik compose (nie trzeba tego powtarzać w `zigbee2supla.env`):

| Ustawienie | Wartość | Dlaczego |
|---|---|---|
| `Z2S_SUPLA_SERVER` | `supla-server` | nazwa usługi w sieci supla-docker |
| `Z2S_SUPLA_SECURITY_LEVEL` | `3` | przypięcie certyfikatu serwera (rozdział 5) |
| `Z2S_SUPLA_CA_FILE` | `/etc/supla-server-ssl/cert.crt` | certyfikat z `./ssl/server` supla-docker (tylko do odczytu) |
| `/data` | `${VOLUME_DATA}/zigbee2supla` | domyślnie `./var/zigbee2supla`, obok innych danych supla-docker |

Opcjonalnie w `.env` supla-docker: `ZIGBEE2SUPLA_IMAGE=...` zmienia obraz
(np. na `ghcr.io/goral64/zigbee2supla:main`).

## 3. Samodzielny kontener

Scenariusz: mostek działa na **osobnej maszynie** z Dockerem, serwer Supli
(Supla Cloud albo supla-docker) jest na innym serwerze, a Home Assistant
z zigbee2mqtt i brokerem MQTT na jeszcze innym. To ten sam obraz co
w rozdziale 2, tylko inaczej skonfigurowany: wszystko łączy się przez sieć
LAN lub internet.

```
 [HA + zigbee2mqtt + Mosquitto] ◄──MQTT 1883── [zigbee2supla] ──TLS 2016──► [serwer Supli]
         maszyna A                               maszyna B                  maszyna C / Cloud
```

1. Skopiuj na maszynę z Dockerem katalog
   [`docker/standalone/`](../docker/standalone/)
   (`docker-compose.yml`, `zigbee2supla.env.example`).
2. `cp zigbee2supla.env.example zigbee2supla.env` i uzupełnij:
   * `Z2S_SUPLA_SERVER`, `Z2S_SUPLA_EMAIL`,
   * `Z2S_MQTT_HOST` = adres maszyny z HA, `Z2S_MQTT_USERNAME`,
     `Z2S_MQTT_PASSWORD` (użytkownik dodany w dodatku Mosquitto).
3. Certyfikat serwera:
   * **Supla Cloud**: nic nie rób (`Z2S_SUPLA_SECURITY_LEVEL=0`).
   * **Lokalny serwer z supla-docker na innej maszynie**: ustaw
     `Z2S_SUPLA_SECURITY_LEVEL=3` i odkomentuj
     `Z2S_SUPLA_CA_FILE=/certs/supla-server.crt`. Następnie umieść
     certyfikat serwera w `./certs/supla-server.crt`, najlepiej kopiując
     plik `ssl/server/cert.crt` z katalogu supla-docker tamtej maszyny:
     ```sh
     mkdir -p certs
     scp uzytkownik@serwer-supli:/sciezka/supla-docker/ssl/server/cert.crt certs/supla-server.crt
     ```
     Jeśli nie masz dostępu do plików serwera, pobierz certyfikat przez
     sieć:
     ```sh
     openssl s_client -connect serwer-supli:2016 -showcerts </dev/null 2>/dev/null \
       | openssl x509 -out certs/supla-server.crt
     openssl x509 -in certs/supla-server.crt -noout -subject -enddate   # CN = SUPLA
     ```
     Rób to w zaufanej sieci: pobrany w ten sposób certyfikat jest
     przypinany bez żadnej weryfikacji („zaufanie przy pierwszym użyciu”).
4. `docker compose up -d`, logi: `docker logs -f zigbee2supla`.
5. W Supla Cloud (tej na serwerze docelowym) włącz rejestrację nowych
   urządzeń.

Wymagania sieciowe: maszyna z mostkiem musi mieć połączenie wychodzące do
brokera MQTT (domyślnie port 1883) i do serwera Supli (port 2016). Nie są
potrzebne żadne porty przychodzące.

Bez compose, samym `docker run`:

```sh
docker run -d --name zigbee2supla --restart unless-stopped \
  --env-file zigbee2supla.env \
  -v "$PWD/data:/data" -v "$PWD/certs:/certs:ro" \
  ghcr.io/goral64/zigbee2supla:latest
```

Zamiast zmiennych możesz zamontować plik YAML (format jak
`config.example.yaml`) i wskazać go przez `-e Z2S_CONFIG=/config/config.yaml`.
Zmienne środowiskowe mają pierwszeństwo przed plikiem.

## 4. Konfiguracja przez zmienne środowiskowe

Każda opcja z pliku konfiguracyjnego ma zmienną `Z2S_` + nazwa opcji
wielkimi literami. Pełna lista: `docker run --rm <obraz> -h`.

| Zmienna | Domyślnie | Opis |
|---|---|---|
| `Z2S_SUPLA_SERVER` | – | adres serwera Supli |
| `Z2S_SUPLA_EMAIL` | – | e-mail konta Supla |
| `Z2S_SUPLA_PORT` | `2016` | port serwera |
| `Z2S_SUPLA_SECURITY_LEVEL` | `0` | weryfikacja certyfikatu, rozdział 5 |
| `Z2S_SUPLA_CA_FILE` | | plik PEM dla poziomu 1 lub 3 |
| `Z2S_SUPLA_PROTO_VERSION` | `27` | wersja protokołu Supli |
| `Z2S_MQTT_HOST`, `Z2S_MQTT_PORT` | `localhost`, `1883` | broker MQTT |
| `Z2S_MQTT_USERNAME`, `Z2S_MQTT_PASSWORD` | | logowanie do MQTT |
| `Z2S_MQTT_CLIENT_ID` | `zigbee2supla-<nazwa hosta>` | identyfikator klienta MQTT; dwie instancje przy jednym brokerze muszą mieć różne |
| `Z2S_Z2M_BASE_TOPIC` | `zigbee2mqtt` | temat bazowy zigbee2mqtt |
| `Z2S_INCLUDE`, `Z2S_EXCLUDE` | | listy urządzeń (IEEE lub nazwa) **oddzielone przecinkami** |
| `Z2S_MAX_PARALLEL_CONNECTS` | `4` | ile urządzeń łączy się jednocześnie |
| `Z2S_LOG_LEVEL` | `info` | `error`, `warning`, `info`, `debug`, `verbose` |
| `Z2S_HA_DISABLE_SUPLA_DUPLICATES` | `false` | wyłączaj w HA duplikaty z discovery Supli ([home-assistant.md](home-assistant.md)) |
| `Z2S_HA_WEBSOCKET_URL` | | np. `ws://192.168.1.10:8123/api/websocket` |
| `Z2S_HA_TOKEN` | | token dostępu HA |
| `Z2S_STATE_DIR` | `/data` (w obrazie) | katalog na `identities.json` |
| `TZ` | `Europe/Warsaw` (w obrazie) | strefa czasowa znaczników czasu w logu, nazwa z bazy tz, np. `Europe/London` albo `UTC` |
| `Z2S_CONFIG` | | ścieżka do opcjonalnego pliku konfiguracji (`.yaml`/`.yml`; inne rozszerzenie = JSON) |

Formaty wartości: liczby dziesiętne, wartości logiczne `true/false`,
`1/0`, `yes/no`, `on/off`, listy oddzielone przecinkami.

**Sekrety:** dla każdej zmiennej działa wariant `_FILE`, który czyta
wartość z pliku, np. `Z2S_MQTT_PASSWORD_FILE=/run/secrets/mqtt_password`
(konwencja sekretów Dockera). Końcowy znak nowej linii jest usuwany.

## 5. Certyfikat serwera

| Poziom | Działanie | Kiedy |
|---|---|---|
| `0` | weryfikacja certyfikatem CA Supli (publicznym dla `*.supla.org`, prywatnym dla pozostałych) i nazwą hosta | Supla Cloud |
| `1` | weryfikacja CA z `Z2S_SUPLA_CA_FILE` i nazwą hosta | serwer z własnym CA i poprawną nazwą w certyfikacie |
| `2` | brak weryfikacji | tylko testy w zaufanej sieci |
| `3` | **przypięcie**: serwer musi przedstawić dokładnie certyfikat z `Z2S_SUPLA_CA_FILE`; nazwa hosta i daty ważności nie są sprawdzane | **supla-docker** |

Dlaczego supla-docker używa poziomu 3: serwer z supla-docker przy pierwszym
starcie generuje samopodpisany certyfikat `ssl/server/cert.crt` z nazwą
`CN=SUPLA`, ważny 365 dni. Poziom 1 odrzuciłby go z powodu nazwy hosta, a po
roku również z powodu daty. Przypięcie chroni przed podstawieniem innego
serwera, a przy tym nie przestaje działać po roku.

Jeśli wygenerujesz w supla-docker nowy certyfikat serwera, zrestartuj
kontener zigbee2supla, żeby wczytał nowy plik:
`docker restart supla-zigbee2supla`.

## 6. Dane i kopia zapasowa

W `/data` (przy supla-docker: `./var/zigbee2supla`) jest `identities.json`
z GUID-ami i kluczami AuthKey wszystkich urządzeń. **Obejmij go kopią
zapasową.** Bez niego każde urządzenie zarejestruje się w Supli jako nowe.
Przy przenoszeniu mostka (np. z CLion do kontenera albo z dodatku HA do
kontenera) skopiuj ten plik, zanim nowa instancja uruchomi się pierwszy raz.

## 7. Rozwiązywanie problemów

| Objaw w logach | Przyczyna i rozwiązanie |
|---|---|
| `Cannot load pinned certificate /etc/supla-server-ssl/cert.crt` | supla-server jeszcze nie wygenerował certyfikatu albo w compose jest zła ścieżka do `ssl/server`. Kontener uruchomi się ponownie sam (`restart: unless-stopped`). |
| `TLS certificate verification failed ... certificate rejected` | certyfikat serwera różni się od przypiętego; zrestartuj kontener po zmianie certyfikatu |
| `Registration failed: registration of new devices is disabled` | włącz rejestrację urządzeń w Supla Cloud; mostek ponawia próbę co minutę |
| `Registration failed: device limit exceeded` | zwiększ limit urządzeń na koncie |
| `MQTT: connection refused` | sprawdź `Z2S_MQTT_*`; broker w HA musi być osiągalny z hosta Dockera |
| `zigbee2mqtt: 0 of N device(s) can be bridged` | urządzenia nie mają obsługiwanych funkcji (lista w README) albo zły `Z2S_Z2M_BASE_TOPIC` |
| `Cannot resolve svr….supla.org: Try again` | kontener nie rozwiązuje nazw, choć host je rozwiązuje; patrz niżej |

### Kontener nie rozwiązuje nazwy serwera Supli

Objaw: w logach co kilka sekund `Cannot resolve <serwer>: Try again`
i `Cannot start connection`, żadne urządzenie nie jest zarejestrowane,
a z samego hosta nazwa serwera rozwiązuje się poprawnie. MQTT może przy tym
działać normalnie, jeśli broker jest podany adresem IP.

Kontener pyta o nazwy wewnętrzny DNS Dockera, który przekazuje zapytania do
serwerów DNS hosta. Część konfiguracji hosta takich zapytań nie obsługuje.
Zaobserwowano to w WSL2 w trybie sieci *mirrored* z tunelowaniem DNS
(serwer `10.255.255.254` odpowiada kontenerom błędem SERVFAIL); w trybie
NAT ten sam host działał poprawnie. Sprawdzenie:

```sh
docker exec zigbee2supla nslookup svr12.supla.org                # przez DNS Dockera
docker exec zigbee2supla nslookup svr12.supla.org 192.168.1.1    # wprost przez router
```

Jeśli drugie polecenie działa, a pierwsze nie, wskaż kontenerowi serwer DNS
wprost, np. w pliku `docker-compose.override.yml` obok `docker-compose.yml`
(compose wczytuje go automatycznie):

```yaml
services:
  zigbee2supla:
    dns:
      - 192.168.1.1     # router albo inny serwer DNS w sieci
```

Potem `docker compose up -d`. Dla wszystkich kontenerów naraz to samo
ustawia wpis `"dns": ["192.168.1.1"]` w `/etc/docker/daemon.json`
i restart Dockera.
