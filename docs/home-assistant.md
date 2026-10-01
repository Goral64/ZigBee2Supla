# Home Assistant: duplikaty urządzeń

## Problem

Gdy w Supla Cloud jest włączony **broker MQTT**, serwer Supli publikuje
MQTT discovery dla Home Assistant **dla każdego kanału** na koncie. HA dodaje
wtedy automatycznie wszystkie urządzenia Supli.

Urządzenia ZigBee trafiają do HA również z zigbee2mqtt. Gdy zigbee2supla
zarejestruje je w Supli, pojawią się w HA **drugi raz**, tym razem jako
urządzenia Supli:

```
zigbee2mqtt ──discovery──► HA: „Salon/czujnik” (zigbee2mqtt)   ✔ właściwe
     │
     └─► zigbee2supla ──► Supla ──discovery──► HA: „Salon/czujnik” (Supla)   ✘ duplikat
```

W Supli nie ma dziś ustawienia, które wyłączałoby discovery dla
wybranych urządzeń. Serwer pomija tylko urządzenia wyłączone i kanały
oznaczone jako ukryte, a ukryty kanał znika też z aplikacji Supla. HA nie
potrafi filtrować discovery MQTT, a urządzenie można w nim tylko ręcznie
wyłączyć.

## Rozwiązanie w zigbee2supla: wyłączanie duplikatów w HA

Mostek łączy się z API Home Assistant (WebSocket) i co 5 minut wyłącza
w rejestrze urządzeń HA te urządzenia Supli, które pochodzą od mostka:

* rozpoznaje je jednoznacznie: identyfikator `supla-iodevice-<id>`
  (discovery Supli) i wersja oprogramowania zaczynająca się od `z2s `
  (wysyła ją zigbee2supla),
* **nie rusza** pozostałych urządzeń Supli ani urządzeń z zigbee2mqtt,
* każde wyłączone urządzenie zapamiętuje w `ha_disabled_devices.json`
  (w katalogu stanu). Jeśli sam włączysz je z powrotem w HA, mostek go
  ponownie nie wyłączy,
* wyłączone urządzenie zostaje w HA, ale bez encji. W liście urządzeń
  widać je po włączeniu filtra „pokaż wyłączone”.

Pierwsze sprawdzenie odbywa się minutę po starcie, potem co 5 minut. Nowe
urządzenie ZigBee zniknie więc z HA jako duplikat najpóźniej po kilku
minutach od rejestracji w Supli.

### Dodatek Home Assistant

Działa bez konfiguracji: opcja `ha_disable_supla_duplicates` jest domyślnie
włączona, a dodatek łączy się z HA przez Supervisor (token dostaje
automatycznie). Żeby wyłączyć tę funkcję, odznacz opcję.

### Docker i uruchomienie ręczne

1. W HA utwórz token: profil użytkownika → Bezpieczeństwo → **Tokeny
   dostępu o długim terminie ważności**. Wymagane jest konto
   administratora, bo rejestr urządzeń wymaga uprawnień administratora.
2. Ustaw:

   | Opcja / zmienna | Przykład |
   |---|---|
   | `ha_disable_supla_duplicates` / `Z2S_HA_DISABLE_SUPLA_DUPLICATES` | `true` |
   | `ha_websocket_url` / `Z2S_HA_WEBSOCKET_URL` | `ws://192.168.1.10:8123/api/websocket` (dla HTTPS: `wss://…`) |
   | `ha_token` / `Z2S_HA_TOKEN` (lub `Z2S_HA_TOKEN_FILE`) | token z punktu 1 |

W logach mostka pojawią się wpisy `HA: disabled duplicate Supla device '…'`.
Błędy połączenia lub zły token są logowane jako ostrzeżenia `HA: …` i nie
wpływają na pracę mostka.

## Rozwiązanie docelowe w Supli (w przygotowaniu)

Właściwym miejscem na takie ustawienie jest Supla Cloud: przełącznik
„publikuj w Home Assistant” przy urządzeniu, podobny do ustawień integracji
z Amazon Alexa i Google Home, domyślnie wyłączony dla urządzeń z mostków
takich jak zigbee2supla. Wymaga to zmian w `supla-core` (serwer MQTT)
i `supla-cloud` (ustawienie i interfejs). Propozycja takich zmian jest
przygotowana i przetestowana (ustawienie kanału „Home Assistant” w Supla
Cloud oraz flaga, którą urządzenie może poprosić o domyślne wyłączenie
discovery); zgłoszenia: SUPLA/supla-core#625 i SUPLA/supla-cloud#1097.
Gdy Supla udostępni takie ustawienie, zigbee2supla będzie ustawiać tę
flagę, a funkcję opisaną wyżej będzie można wyłączyć.
