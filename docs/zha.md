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

## Przejście między ZHA a zigbee2mqtt

Mostek nadaje kanałom te same oznaczenia co przy zigbee2mqtt, a urządzenie
rozpoznaje po adresie IEEE, który nie zmienia się przy ponownym
parowaniu. Po przeniesieniu urządzenia z ZHA do zigbee2mqtt (albo
odwrotnie) i przy tym samym `identities.json` urządzenie powinno więc
zostać w Supli tym samym urządzeniem, z historią. Przełączniki
wielokanałowe i piloty mogą dostać inne kanały: nowe dochodzą, a stare
zostają w Supli bez połączenia. Takiego przejścia nie sprawdzałem jeszcze
na prawdziwym urządzeniu.
