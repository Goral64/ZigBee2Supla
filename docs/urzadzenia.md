# Urządzenia ZigBee w Supli

Jak mostek przenosi urządzenia z zigbee2mqtt do Supli. Mapowanie odbywa
się na podstawie opisu urządzenia w zigbee2mqtt (`exposes`
z `zigbee2mqtt/bridge/devices`), więc nie zależy od producenta ani modelu:
urządzenie obsługiwane przez zigbee2mqtt dostaje kanały według tego, co
udostępnia.

## Spis kanałów

| zigbee2mqtt | Kanał Supli |
|---|---|
| `switch` → `state`, `state_l1`, `state_l2`… | przekaźnik (włącznik zasilania) |
| `light` → `state` | przekaźnik (włącznik światła) |
| `temperature` + `humidity` | termometr z higrometrem |
| `temperature` | termometr |
| `humidity` | higrometr |
| `pressure` | ciśnienie |
| `contact` | czujnik otwarcia drzwi (1 = zamknięte) |
| `occupancy`, `presence` | czujnik ruchu |
| `water_leak` | czujnik zalania |
| `smoke`, `gas`, `carbon_monoxide`, `vibration` | czujnik binarny |
| `energy` (kWh) z `power`, `voltage`, `current` | licznik energii (jedna faza) |
| `power` (W), `voltage` (V), `current` (A) bez `energy` | pomiar ogólny, osobno każda wartość (bez urządzeń bateryjnych) |
| `illuminance` (lx) | pomiar ogólny „natężenie światła” |
| `action` (przyciski, piloty) | wyzwalacz akcji, osobny kanał na każdy przycisk |
| `climate` z `current_heating_setpoint` (głowice, termostaty) | termometr + termostat |

Oprócz tego każdy kanał pokazuje stan urządzenia: baterię, zasilanie
i siłę sygnału ZigBee (rozdział [Bateria i sygnał](#bateria-i-sygnał)).

**Układ kanałów jest stały.** Kanały, które urządzenie dostało przy
pierwszym połączeniu z Suplą, zostają na zawsze w tej samej kolejności:
Supla nie pozwala zmieniać ich typu ani usuwać ich z działającego
urządzenia. Nowe możliwości (np. po aktualizacji mostka) są dopisywane na
końcu. Kanał, którego urządzenie już nie udostępnia, jest offline.

## Pomijane urządzenia

* koordynator, urządzenia wyłączone w zigbee2mqtt (`disabled`),
  nieobsługiwane przez zigbee2mqtt i jeszcze nie sparowane do końca,
* urządzenia bez żadnej z funkcji z tabeli,
* urządzenia, których główna funkcja nie jest jeszcze obsługiwana: rolety
  (`cover`), zamki (`lock`), wentylatory (`fan`) i urządzenia `climate`
  bez ustawianej temperatury grzania (np. klimatyzatory). Ich dodatkowe
  przełączniki i czujniki utworzyłyby w Supli układ kanałów, którego nie
  da się potem zmienić. Mostek wypisuje w logu, które urządzenia pominął
  z tego powodu.

Które urządzenia trafiają do Supli, można też ograniczyć opcjami
`include` i `exclude` (README, rozdział „Konfiguracja”).

## Nazwy i podpisy

Urządzenie w Supli ma nazwę z zigbee2mqtt (`friendly_name`). Zmiana nazwy
w zigbee2mqtt zmienia ją w Supli; urządzenie zostaje tym samym urządzeniem
(tożsamość zależy od adresu IEEE).

Po rejestracji mostek podpowiada podpis każdego kanału: „<nazwa
urządzenia> – <rodzaj kanału>”, np. „Salon – termometr”, „Kuchnia –
przekaźnik L1”, „Drzwi – kontaktron”. Serwer Supli zapisuje go tylko
wtedy, gdy kanał nie ma jeszcze podpisu, więc podpisy nadane w Supla Cloud
nie są zmieniane.

## Przekaźniki

Włączanie i wyłączanie z Supli; stan zmieniony poza Suplą (przyciskiem,
w Home Assistant) jest widoczny w Supli w ciągu ułamka sekundy od
komunikatu zigbee2mqtt. Przekaźnik, którego stan nie jest jeszcze znany,
jest offline i nie przyjmuje poleceń; mostek pyta o stan zaraz po starcie.

**Włączanie na czas.** Jeśli przekaźnik ma własny timer, w aplikacji Supli
można ustawić czas przed włączeniem, a w Cloud wybrać funkcję „włącznik
schodowy”. Odlicza i wyłącza samo urządzenie, więc zadziała także przy
awarii mostka, brokera albo połączenia z Suplą; aplikacja pokazuje
pozostały czas.

| zigbee2mqtt | Urządzenia (przykłady) | Zakres |
|---|---|---|
| `countdown`, `countdown_l1`, `countdown_l2`… (s) | gniazdka i przełączniki Tuya i pokrewne | do `value_max`, zwykle 12 h, co 1 s |
| `timer` (min) razem z `time_left` | sterowniki nawadniania, np. Lidl Parkside PSBZS A1 | do `value_max`, co 1 min (czas zaokrąglany w górę) |

Przekaźnik bez timera nie oferuje włączania na czas: mostek nie odlicza
zamiast urządzenia i takie polecenie odrzuca. Czas dłuższy, niż pozwala
urządzenie, też jest odrzucany.

## Czujniki

Temperatura, wilgotność, ciśnienie i czujniki dwustanowe trafiają do Supli
od razu po każdym raporcie urządzenia. Czujnik bez żadnego odczytu jest
offline.

Czujnik otwarcia (`contact`) ma w Supli wartość 1, gdy drzwi są zamknięte
(odwrotnie niż w zigbee2mqtt). Czujniki dymu, gazu, CO i drgań są na razie
ogólnymi czujnikami binarnymi.

**Pomiar ogólny** (natężenie światła, moc, napięcie, prąd bez licznika)
dostaje domyślną jednostkę i liczbę miejsc po przecinku (np. „lx”, „W”)
oraz włączoną historię. Ustawienia zmienione w Supla Cloud zostają.

## Licznik energii

Gniazdko z pomiarem energii dostaje kanał licznika: energię, moc,
napięcie i prąd, z historią i kosztami w Cloud. Jeśli urządzenie ma jeden
przekaźnik, mostek podpowiada serwerowi, że licznik go mierzy, więc
aplikacja pokazuje pobór przy włączniku; powiązanie wybrane w Cloud ma
pierwszeństwo.

Licznik jest przekazywany tak, jak podaje go urządzenie; zerowania
z Supli nie ma. Licznik bez znanej energii jest offline, żeby do historii
nie trafiło zero. Zmiana energii jest wysyłana od razu, a moc, napięcie
i prąd najczęściej co 5 s.

## Przyciski i piloty

Każdy przycisk to w Supli wyzwalacz akcji; reakcje przypisujesz w Supla
Cloud. Wartości `action` z zigbee2mqtt:

| zigbee2mqtt | Supla |
|---|---|
| `single`, `click`, `press` | krótkie naciśnięcie ×1 |
| `double` / `triple` / `quadruple` | krótkie naciśnięcie ×2 / ×3 / ×4 |
| `hold`, `long` | przytrzymanie |
| `on`, `off` | włącz, wyłącz |
| `rotate_left`, `rotate_right` | obrót w lewo, w prawo |

Numer przycisku w wartości (`1_single`, `button_2_hold`) daje osobny kanał
na przycisk („przycisk 1”, „przycisk 2”…). Inne wartości (np.
`brightness_move_up`, `release`, `single_left`) są pomijane. Akcja
zachowana przez broker (*retain*) albo z czasu, gdy urządzenie nie było
połączone z Suplą, nie jest wysyłana: spóźniona akcja uruchomiłaby
automatyzację w nieoczekiwanej chwili.

## Termostaty i głowice

Urządzenie `climate` z ustawianą temperaturą grzania, temperaturą
pomieszczenia i trybem `heat` dostaje dwa kanały: termometr
(`local_temperature`, główny termometr termostatu) i termostat. Termostat
reguluje sam; Supla nim steruje:

* **włączanie i wyłączanie** (`system_mode`) i **temperatura zadana**
  (w zakresie i z krokiem urządzenia),
* ustawienie temperatury przełącza urządzenie z jego własnego programu
  w tryb ręczny (`preset` `manual` albo `hold`), bo inaczej program by ją
  nadpisał,
* **„tryb programu”** w Supli przełącza urządzenie na jego **własny**
  program (`preset` `schedule` albo `program`); Supla pokazuje, że
  urządzenie pracuje według programu,
* Supla pokazuje, czy urządzenie grzeje, a przy głowicach otwarcie
  zaworu.

Nowa temperatura i tryb są widoczne w Supli od razu, choć urządzenie
potwierdza je z opóźnieniem (termostaty Tuya po 10–25 s). Do urządzenia
idzie tylko to, co się zmienia.

Ograniczenia:

* **harmonogram z zakładki „Tydzień” aplikacji Supli nie jest
  wykonywany.** Termostat ma harmonogram tylko dlatego, że aplikacja bez
  niego nie pokazuje sterowania; jego edycja nic nie zmienia. Program
  urządzenia ustawia się jak dotąd: na urządzeniu albo w zigbee2mqtt,
* włączanie na czas, presety (eco, komfort, boost) i dodatkowe ustawienia
  urządzenia (wykrywanie okna, blokada przycisków…) nie są przekazywane;
  działają w urządzeniu jak dotąd,
* głowice bateryjne nie odpowiadają na pytanie o stan, więc ich termostat
  jest w Supli offline do pierwszego raportu urządzenia (zwykle kilka
  minut).

## Bateria i sygnał

Każdy kanał pokazuje w Supli stan całego urządzenia:

* poziom baterii (`battery`), a gdy urządzenie podaje tylko `battery_low`
  – stan baterii (słaba / w porządku),
* zasilanie z baterii lub sieci (`power_source`),
* siłę sygnału ZigBee (`linkquality` 0–255, przeliczone liniowo na
  0–100 %).

Zmiana baterii jest wysyłana od razu; siła sygnału, która zmienia się
prawie z każdą wiadomością, tylko wtedy, gdy otworzysz informacje
o kanale w aplikacji.

## Dostępność

Dostępność urządzenia w zigbee2mqtt (`<urządzenie>/availability`) decyduje
o jego połączeniu z Suplą: urządzenie zgłoszone jako niedostępne rozłącza
się od serwera Supli, tak jak wyłączone urządzenie, i łączy ponownie, gdy
wróci do sieci ZigBee. Wymaga to włączonej funkcji *availability*
w zigbee2mqtt; urządzenia zasilane z sieci są wtedy uznawane za
niedostępne po kilku minutach, a bateryjne po około dobie bez żadnej
wiadomości.

Gdy niedostępny jest sam zigbee2mqtt albo broker MQTT, urządzenia
pozostają połączone z Suplą, a ich kanały przechodzą w stan offline.
