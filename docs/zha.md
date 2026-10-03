# ZHA zamiast zigbee2mqtt

Jeśli urządzenia ZigBee są w Home Assistant podłączone przez ZHA
(wbudowaną integrację ZigBee), mostek może brać je stamtąd. Nie trzeba
przechodzić na zigbee2mqtt ani parować urządzeń od nowa, a MQTT nie jest
potrzebne. Mostek łączy się wtedy z API WebSocket Home Assistanta: czyta
urządzenia, ich stany i naciśnięcia przycisków, a polecenia z Supli
wykonuje przez usługi HA (np. `switch.turn_on`).

## Włączenie

W dodatku Home Assistant wystarczy ustawić opcję `source` na `zha`. Adres
HA i token dodatek dostaje sam.

W Dockerze albo przy uruchomieniu ręcznym:

```yaml
source: zha
ha_websocket_url: ws://192.168.1.10:8123/api/websocket
ha_token: <token>
```

Token tworzy się w HA w profilu użytkownika (Bezpieczeństwo → Tokeny
dostępu o długim terminie ważności), na koncie administratora, bo mostek
czyta rejestr urządzeń.

Instalując mostek w Dockerze skryptem `z2s.sh` (README, „Szybki start”),
wybierz na pytanie „Skąd brać urządzenia ZigBee?” odpowiedź „ZHA w Home
Assistant”. Skrypt zapyta o adres HA i token. Przy ręcznej instalacji te
same ustawienia wpisuje się do `zigbee2supla.env`:

```
Z2S_SOURCE=zha
Z2S_HA_WEBSOCKET_URL=ws://192.168.1.10:8123/api/websocket
Z2S_HA_TOKEN=<token>
```

Aktualizacja mostka nie zależy od źródła: dodatek aktualizuje się w HA,
Docker przez `./z2s.sh update` albo `docker compose pull && docker compose
up -d`.

## Co trafia do Supli

To samo co przy zigbee2mqtt ([urzadzenia.md](urzadzenia.md)), tylko
rozpoznawane po encjach HA:

* `switch` i `light` jako przekaźniki (światła tylko włącz/wyłącz),
* `binary_sensor` i `sensor` według klasy urządzenia (otwarcie, ruch,
  zalanie, temperatura, wilgotność, ciśnienie, światło, energia, moc,
  napięcie, prąd),
* `climate` z trybem `heat` jako termometr i termostat,
* przyciski i piloty z wyzwalaczy urządzenia, tych samych, które HA
  pokazuje w automatyzacjach („Naciśnięto przycisk…”),
* bateria z encji baterii.

Pomijane są encje konfiguracyjne i diagnostyczne (blokada rodzicielska,
podświetlenie, firmware) oraz encje wyłączone w HA. Urządzenie z roletą,
zamkiem albo wentylatorem jest pomijane w całości, jak przy zigbee2mqtt.

Różnice względem zigbee2mqtt:

* nie ma włączania na czas, bo ZHA nie udostępnia timera urządzenia,
* siła sygnału nie jest przekazywana (encje LQI są w ZHA domyślnie
  wyłączone),
* termostatu, który w ZHA nie ma trybu `off`, nie da się wyłączyć z Supli,
* czujnik binarny bez klasy urządzenia jest pomijany, bo nie wiadomo, czym
  jest. Zdarza się to u niektórych usypiających czujników; czasem pomaga
  ponowne sparowanie z wybudzaniem czujnika,
* przycisk bez wyzwalaczy w ZHA (brak opisu urządzenia w ZHA) nie dostaje
  kanału,
* nazwy urządzeń są takie jak w HA.

## Zmiana źródła

W dodatku zmienia się opcję `source` i uruchamia dodatek ponownie.
W Dockerze wystarczy jeszcze raz `./z2s.sh setup` z inną odpowiedzią albo
zmiana `Z2S_SOURCE` (i adresu HA z tokenem) w `zigbee2supla.env`
i `docker compose up -d`. `identities.json` zostaje ten sam.

Mostek nie przenosi urządzeń między ZHA a zigbee2mqtt. Koordynator może
obsługiwać tylko jeden z nich, więc przy przesiadce urządzenia paruje się
od nowa w nowym programie. Co wtedy dzieje się z nimi w Supli, opisuje
rozdział niżej.

## Przejście między ZHA a zigbee2mqtt

Mostek nadaje kanałom te same oznaczenia co przy zigbee2mqtt, a urządzenie
rozpoznaje po adresie IEEE, który nie zmienia się przy ponownym
parowaniu. Po przeniesieniu urządzenia z ZHA do zigbee2mqtt (albo
odwrotnie) i przy tym samym `identities.json` urządzenie powinno więc
zostać w Supli tym samym urządzeniem, z historią. Przełączniki
wielokanałowe i piloty mogą dostać inne kanały: nowe dochodzą, a stare
zostają w Supli bez połączenia. Takiego przejścia nie sprawdzałem jeszcze
na prawdziwym urządzeniu.
