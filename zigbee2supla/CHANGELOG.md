# Zmiany

## 0.4.0-beta.2

- Opcje `include` i `exclude` przyjmują adres IEEE także w zapisie z Home
  Assistanta (`A4:C1:38:75:21:38:45:E3`), nie tylko `0xa4c13875213845e3`.
- Przepływ (np. zawór SONOFF SWV) jako pomiar ogólny w m³/h.

## 0.4.0-beta.1

- Urządzenia można brać z ZHA (integracja ZigBee w Home Assistant)
  zamiast z zigbee2mqtt: opcja `source` ustawiona na `zha`. MQTT nie jest
  wtedy potrzebne. Opis: docs/zha.md w repozytorium.
- Dodatek nie wymaga już dodatku Mosquitto, jeśli urządzenia są w ZHA.
- W Dockerze kreator `z2s.sh` pyta też o ZHA.
- Nazwa mostka: Zigbee to Supla bridge.

## 0.3.0-beta.3

- Nowy sposób instalacji w Dockerze, także bez Home Assistanta: skrypt
  `z2s.sh` z paczki wydania sam konfiguruje mostek, a przy własnym
  koordynatorze (USB albo sieciowym) dokłada zigbee2mqtt i Mosquitto.
- Sam dodatek bez zmian.

## 0.3.0-beta.2

- Identyfikator klienta MQTT ma postać `zigbee2supla-<nazwa hosta>`,
  więc dwa mostki przy jednym brokerze już się nie rozłączają.
- Nowa opcja `mqtt_client_id`.

## 0.3.0-beta.1

- Pierwsze wydanie publiczne. Każde urządzenie z zigbee2mqtt jest w Supli
  osobnym urządzeniem: przekaźniki i gniazdka (także z licznikiem energii),
  czujniki, przyciski i piloty, termostaty i głowice.
