# Zigbee2Supla

**Zigbee2MQTT to Supla bridge** – mostek między
[zigbee2mqtt](https://www.zigbee2mqtt.io) a [Suplą](https://www.supla.org).
Urządzenia ZigBee z zigbee2mqtt trafiają do Supli tak, jakby
każde było osobnym urządzeniem Supli: z własną nazwą, lokalizacją,
statusem online, kanałami i historią. W Supla Cloud i w aplikacji widzisz
więc „Termometr salon” i „Gniazdko pralka”, a nie jedną bramkę ze stoma
kanałami.

**Stan: wersja beta.** Mostek działa na co dzień w instalacji z ponad
100 urządzeniami ZigBee (Home Assistant OS z zigbee2mqtt, Supla Cloud).
Sprawdzone urządzenia i ograniczenia opisuje rozdział
[Co jest sprawdzone](#co-jest-sprawdzone).

## Co potrafi

* przekaźniki (gniazdka, przełączniki, światła), także wielokanałowe,
  z włączaniem na czas, jeśli urządzenie ma timer,
* termometry, higrometry, ciśnienie, czujniki otwarcia, ruchu, obecności,
  zalania, dymu, gazu, CO i drgań, natężenie światła,
* liczniki energii gniazdek (energia, moc, napięcie, prąd) powiązane
  z ich przekaźnikiem,
* przyciski i piloty jako wyzwalacze akcji w Supli,
* termostaty i głowice grzejnikowe (temperatura zadana, tryb, własny
  program urządzenia),
* bateria i siła sygnału ZigBee przy każdym urządzeniu,
* podpisy kanałów nadawane automatycznie („Salon – termometr”).

Szczegóły dla każdego rodzaju urządzeń: [docs/urzadzenia.md](docs/urzadzenia.md).

## Jak to działa

```
 [urządzenia ZigBee] ─► [koncentrator ZigBee] ─► [zigbee2mqtt] ─MQTT─► [zigbee2supla] ─TLS─► [serwer Supli]
                                                                        jedno połączenie
                                                                        na każde urządzenie
```

Mostek czyta urządzenia i ich stany z zigbee2mqtt przez MQTT i dla każdego
urządzenia ZigBee utrzymuje osobne połączenie z serwerem Supli, tak jak
robi to zwykłe urządzenie Supli. Polecenia z Supli (włącz, ustaw
temperaturę) wracają tą samą drogą do zigbee2mqtt.

Tożsamość urządzeń w Supli (GUID i AuthKey) mostek losuje przy pierwszym
połączeniu i zapisuje w pliku `identities.json`. **Ten plik trzeba
chronić i uwzględnić w kopii zapasowej**: bez niego wszystkie urządzenia
pojawią się w Supli jako nowe.

## Wymagania

* zigbee2mqtt z brokerem MQTT (np. dodatki „Zigbee2MQTT” i „Mosquitto
  broker” w Home Assistant; zalecana włączona funkcja *availability*)
  albo koordynator ZigBee (USB lub sieciowy), wtedy zigbee2mqtt instaluje
  się razem z mostkiem,
* konto Supli: Supla Cloud albo własny serwer (supla-docker),
* miejsce do uruchomienia mostka: dodatek Home Assistant albo Docker
  (amd64, arm64, armv7).

## Szybki start

### Dodatek Home Assistant

1. Ustawienia → Dodatki → Sklep z dodatkami → ⋮ → Repozytoria → dodaj
   `https://github.com/Goral64/ZigBee2Supla`.
2. Zainstaluj dodatek „Zigbee2Supla”.
3. W konfiguracji ustaw `supla_server` (np. `svr12.supla.org`, adres
   widoczny w Supla Cloud) i `supla_email`. Broker MQTT jest brany
   automatycznie z dodatku Mosquitto.
4. Przeczytaj [Przed pierwszym uruchomieniem](#przed-pierwszym-uruchomieniem)
   i uruchom dodatek.

`identities.json` jest w katalogu `/addon_configs/<slug dodatku>/`;
uwzględnij go w kopii zapasowej Home Assistant.

### Docker

Na maszynie z Linuksem i Dockerem (np. Raspberry Pi albo ta sama, na której
działa supla-docker):

```sh
curl -L https://github.com/Goral64/ZigBee2Supla/releases/latest/download/zigbee2supla-docker.tar.gz | tar xz
cd zigbee2supla
./z2s.sh
```

Skrypt zapyta o serwer Supli, e-mail konta i o to, skąd brać urządzenia:
z własnego koordynatora, USB albo sieciowego (wtedy sam doda
zigbee2mqtt), albo z zigbee2mqtt, który już masz, np. w Home Assistant.
Potem uruchomi mostek. Do supla-docker dołączy się sam, wystarczy
wskazać jego katalog.

Dalej przydają się `./z2s.sh status`, `logs`, `update` i `backup`.
Ręczna instalacja i szczegóły: [docs/docker.md](docs/docker.md).

## Przed pierwszym uruchomieniem

1. **Włącz rejestrację nowych urządzeń** w Supla Cloud (Moje urządzenia →
   „Rejestracja urządzeń”). Dopóki jest wyłączona, mostek ponawia próbę co
   minutę.
2. **Sprawdź limit urządzeń na koncie.** Każde urządzenie ZigBee liczy się
   jako osobne urządzenie Supli.
3. **Zacznij od kilku urządzeń** (opcja `include`), obejrzyj je w Supli,
   potem dodaj resztę. Kanały raz utworzonego urządzenia zostają w Supli
   na stałe ([docs/urzadzenia.md](docs/urzadzenia.md#spis-kanałów)).
4. **Nie uruchamiaj dwóch mostków z tym samym `identities.json`**:
   wyrzucałyby się nawzajem z serwera. Przenosząc mostek (np. z Dockera do
   dodatku), zatrzymaj stary i skopiuj jego `identities.json` przed
   pierwszym startem nowego.

Jeśli w Supla Cloud jest włączona integracja MQTT z Home Assistant,
urządzenia ZigBee trafią do HA drugi raz, jako urządzenia Supli. Mostek
może je tam automatycznie wyłączać: [docs/home-assistant.md](docs/home-assistant.md).

## Konfiguracja

Opcje dodatku HA, zmienne `Z2S_<OPCJA>` (Docker, np. `Z2S_SUPLA_SERVER`)
i plik YAML ([`config.example.yaml`](config.example.yaml)) mają te same
nazwy. Zmienne mają pierwszeństwo przed plikiem; `Z2S_<OPCJA>_FILE` czyta
wartość z pliku (np. hasło z sekretu Dockera).

| Opcja | Domyślnie | Opis |
|---|---|---|
| `supla_server` | – | adres serwera Supli, np. `svr12.supla.org` |
| `supla_email` | – | e-mail konta Supla |
| `supla_security_level` | `0` | `0` – certyfikat CA Supli (Supla Cloud), `3` – przypięty certyfikat z `supla_ca_file` (własny serwer), `1` – inny CA z `supla_ca_file`, `2` – bez weryfikacji (tylko zaufana sieć lokalna); zob. [docs/docker.md](docs/docker.md#5-certyfikat-serwera) |
| `supla_ca_file` | | plik PEM (dla poziomów `1` i `3`) |
| `supla_port` | `2016` | port serwera Supli |
| `supla_proto_version` | `27` | wersja protokołu Supli (27–29) |
| `mqtt_host`, `mqtt_port` | `localhost`, `1883` | broker MQTT |
| `mqtt_username`, `mqtt_password` | | dane logowania MQTT |
| `mqtt_client_id` | `zigbee2supla-<nazwa hosta>` | identyfikator klienta MQTT; dwie instancje mostka przy jednym brokerze muszą mieć różne (broker rozłącza klienta, gdy połączy się inny z tym samym identyfikatorem) |
| `z2m_base_topic` | `zigbee2mqtt` | temat bazowy zigbee2mqtt |
| `state_dir` | `.` | katalog na `identities.json` (Docker: `/data`) |
| `include` | `[]` | tylko te urządzenia (nazwa z zigbee2mqtt albo adres IEEE); pusta lista = wszystkie |
| `exclude` | `[]` | te urządzenia są pomijane |
| `max_parallel_connects` | `4` | ile urządzeń łączy się z Suplą jednocześnie |
| `log_level` | `info` | `error`, `warning`, `info`, `debug`, `verbose` |
| `ha_disable_supla_duplicates` | `false` | wyłączaj w HA duplikaty z discovery Supli ([docs/home-assistant.md](docs/home-assistant.md)); w dodatku HA domyślnie włączone |
| `ha_websocket_url` | | API WebSocket HA, np. `ws://192.168.1.10:8123/api/websocket` (nie dotyczy dodatku) |
| `ha_token` | | token dostępu HA, długoterminowy (nie dotyczy dodatku) |

Co minutę (i przy każdej zmianie) mostek wypisuje stan, np.:

```
Status: 101 ZigBee device(s), 101 bridged: 66 connected to Supla, 35 offline in ZigBee, 0 connecting or failed
```

czyli: urządzenia w zigbee2mqtt, z nich mostkowane do Supli, a mostkowane
dzielą się na połączone z Suplą, niedostępne w sieci ZigBee (celowo
rozłączone) i łączące się albo odrzucone przez serwer.

## Co jest sprawdzone

Na Supla Cloud (svr28.supla.org) z zigbee2mqtt w Home Assistant OS,
w sieci ponad 100 urządzeń:

| Rodzaj | Urządzenia |
|---|---|
| przekaźniki, gniazdka | SONOFF ZBMINI, Tuya TS0001/TS0002, gniazdka Nous A1Z, Girier JR-ZPM01, Tuya TS011F/TS0121 (z licznikiem energii; timer na Nous A1Z) |
| czujniki | SONOFF SNZB-04 (otwarcie), SNZB-03 (ruch), SNZB-05P (zalanie), Xiaomi GZCGQ01LM, Aqara GZCGQ11LM i Tuya TS0222 (światło) |
| przyciski | Tuya TS0041 |
| termostaty | głowice Tuya TS0601, termostat ścienny Moes BHT-002 |

Czujniki temperatury, wilgotności i ciśnienia, piloty wieloprzyciskowe
i czujniki dymu, gazu i CO są sprawdzone tylko testami, bez prawdziwych
urządzeń.

Sprawdzone sposoby uruchomienia: dodatek Home Assistant (z repozytorium),
kontener Docker na osobnej maszynie, usługa w supla-docker oraz zestaw
z własnym koordynatorem instalowany przez `z2s.sh`, z ConBee II (USB)
i SLZB-06U (sieciowym), zarówno z Supla Cloud, jak i z supla-docker.

**Jeszcze nie ma:** ściemniania i koloru świateł, rolet, zamków,
wentylatorów, harmonogramu tygodniowego Supli dla termostatów (działa
program samego urządzenia), zerowania licznika energii.

## Problemy i rozwiązania

| Objaw (log mostka) | Przyczyna i rozwiązanie |
|---|---|
| `registration of new devices is disabled` | Włącz rejestrację urządzeń w Supla Cloud; mostek ponawia próbę co minutę. |
| `device limit exceeded` | Limit urządzeń na koncie Supli; zwiększ go albo ogranicz urządzenia opcją `include`/`exclude`. |
| `channel conflict` | Urządzenie w Supli ma inne kanały niż teraz (np. po skasowaniu `identities.json`). Przywróć `identities.json` z kopii; w ostateczności usuń urządzenie w Supla Cloud. |
| urządzenia co chwilę rozłączają się i łączą | Dwa mostki z tym samym `identities.json`. Zostaw jeden. |
| `Cannot resolve …` w Dockerze | Kontener nie ma DNS; zob. [docs/docker.md, rozdział 7](docs/docker.md#7-rozwiązywanie-problemów). |
| urządzenie nie pojawia się w Supli | Sprawdź w logu listę pominiętych urządzeń i [docs/urzadzenia.md](docs/urzadzenia.md#pomijane-urządzenia); urządzenie niedostępne w ZigBee łączy się z Suplą dopiero po powrocie do sieci. |
| zmiana w Supli widoczna z opóźnieniem | Strona Supla Cloud odświeża stany co kilka sekund; termostaty Tuya potwierdzają zmiany po 10–25 s. Mostek przekazuje polecenia w milisekundach. |

Więcej informacji daje `log_level: debug`.

## Dla programistów

Mostek jest napisany w C++17 (CMake). Rdzeń (`core/`) nie zależy od
platformy, z myślą o przyszłej wersji na ESP32.

```sh
tools/dev/dev.sh cmake --preset debug          # kontener deweloperski,
tools/dev/dev.sh cmake --build --preset debug  # wystarczy Docker
tools/dev/dev.sh ctest --preset debug
```

Albo z zależnościami w systemie (Debian/Ubuntu):

```sh
sudo apt install build-essential cmake ninja-build libssl-dev \
    libmosquitto-dev nlohmann-json3-dev libyaml-cpp-dev libgtest-dev
cmake --preset debug && cmake --build --preset debug   # presety: debug, asan, release
ctest --preset debug
./build/debug/zigbee2supla -c config.local.yaml
```

| Katalog | Zawartość |
|---|---|
| `core/` | sesje Supli (jedno urządzenie = jedna sesja), kanały, tożsamości |
| `backends/z2m/` | źródło urządzeń: zigbee2mqtt przez MQTT, mapowanie na kanały |
| `platform/linux/` | transport TLS (OpenSSL), klient WebSocket |
| `apps/linux/` | program `zigbee2supla`, konfiguracja |
| `integrations/ha/` | wyłączanie duplikatów w Home Assistant |
| `third_party/supla-common/` | protokół Supli, kopia z [supla-device](https://github.com/SUPLA/supla-device) |
| `tests/` | testy (GoogleTest) z udawanym serwerem Supli |
| `docker/`, `Dockerfile`, `zigbee2supla/` | obrazy, compose, dodatek Home Assistant |

Bez konta Supli można testować z udawanym serwerem
`tools/fake_supla_server.py`.

## Licencja

GPL-2.0-or-later, jak supla-device, z którego pochodzi kod protokołu.
