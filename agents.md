# Zalozenia projektu

Ten plik zbiera podstawowe zalozenia projektu dla osob i agentow pracujacych w tym repo.

## Cel systemu

Projekt sluzy do zdalnego zbierania pomiarow z wag pasiecznych i przesylania ich do serwera.
Architektura systemu jest dwustopniowa:

- `waga` wykonuje lokalny pomiar masy ula i napiecia baterii,
- `centrala` odbiera dane od wielu wag przez `ESP-NOW`,
- `centrala` wysyla zbiorczy raport przez `GPRS/HTTP` do backendu PHP/MySQL.

## Glowne role w repo

- `src/waga.cpp` - firmware wagi pasiecznej na `FireBeetle 2 ESP32-C6` lub starszej plytce `Wemos ESP32`.
- `src/centrala.cpp` - firmware centrali na `TTGO T-Call (ESP32 + SIM800)`.
- `src/kalibracja.cpp` - prosty program do kalibracji modulu `HX711`.
- `src/waga_odbior.php` - backend odbierajacy JSON z centrali i zapisujacy dane do bazy.
- `platformio.ini` - definicje srodowisk builda i docelowych plytek.

## Zalozenia funkcjonalne

- Jedna centrala obsluguje wiele wag.
- Waga nie laczy sie bezposrednio z internetem.
- Komunikacja lokalna miedzy waga i centrala odbywa sie przez `ESP-NOW`.
- Centrala publikuje dane do zewnetrznego serwera przez modem `SIM800` i `GPRS`.
- System ma dzialac energooszczednie, dlatego oba urzadzenia wiekszosc czasu spedzaja w `deep sleep`.

## Zalozenia dla wagi

- Waga mierzy mase przez `HX711`.
- Waga mierzy takze napiecie wlasnej baterii.
- Waga budzi sie cyklicznie, wykonuje pomiar i probuje wyslac dane do centrali.
- Jesli nie zna MAC centrali, uruchamia procedure `discovery`.
- Po udanej transmisji oczekuje na synchronizacje czasu z centrali.
- `DS3231` w wadze jest uzywany jako lokalne zrodlo czasu po pierwszym sparowaniu.
- W pamieci `RTC RAM` i `NVS` przechowywany jest MAC centrali oraz stan potrzebny do wznowienia pracy po snie.
- W wadze istnieje przycisk resetu do wyczyszczenia zapisanej konfiguracji centrali.

## Zalozenia dla centrali

- Centrala budzi sie o ustalonych godzinach i nasluchuje wag przez ograniczone okno czasowe.
- Aktualnie w kodzie przyjeto dwa glowne okna pracy: okolo `06:00` i `20:00`.
- Centrala moze zakonczyc nasluch wczesniej po odebraniu oczekiwanej liczby unikalnych wag.
- Po odebraniu pakietu od wagi centrala odsyla synchronizacje czasu.
- Nawet jesli zadna waga sie nie zglosi, centrala moze wyslac raport zawierajacy wlasny stan, np. napiecie baterii.
- Centrala korzysta z `DS3231` do wybudzania i utrzymania harmonogramu pracy.
- Logi lokalne sa zapisywane w `LittleFS`.

## Harmonogram pracy

- Waga ma dwa glowne cykle dzienne: poranny i wieczorny.
- W aktualnym firmware wagi wystepuje etap `presync` okolo `05:30` i `19:30`.
- Wlasciwa wysylka danych z wagi jest planowana okolo `06:00` i `20:00`.
- Centrala budzi sie zgodnie z alarmami `DS3231` i nasluchuje danych z wag w tych samych oknach.
- Gdy transmisja sie nie powiedzie, waga podejmuje ograniczona liczbe ponownych prob z krotkim uspieniem pomiedzy nimi.

## Format i przeplyw danych

- Waga wysyla do centrali mase, napiecie baterii, identyfikator urzadzenia i statystyki dryfu czasu.
- Centrala agreguje pomiary z wielu wag do jednego dokumentu JSON.
- JSON zawiera identyfikator centrali, wersje firmware, stan baterii centrali, sile sygnalu GSM, temperature z `DS3231` i liste pomiarow z wag.
- Backend PHP zapisuje informacje o centrali, urzadzeniach i pomiarach do bazy danych MySQL.

## Zalozenia techniczne

- Repo jest projektem `PlatformIO`.
- Domyslnym srodowiskiem builda jest obecnie `waga`.
- Wazne srodowiska to `waga`, `kalibracja`, `waga_wemos` i `centrala`.
- Firmware jest pisane w `Arduino/C++`.
- Projekt zaklada rzeczywisty hardware, wiec czesc funkcji jest silnie zwiazana z pinami i konkretnymi modulami.

## Zasady bezpiecznych zmian

- Nie zmieniac formatow pakietow `ESP-NOW` bez jednoczesnej aktualizacji obu stron: `waga` i `centrala`.
- Nie zmieniac formatu JSON wysylanego do backendu bez sprawdzenia zgodnosci z `src/waga_odbior.php`.
- Przy zmianach harmonogramu trzeba zachowac zgodnosc czasu pracy wagi i centrali.
- Przy zmianach pinow lub plytek nalezy najpierw sprawdzic odpowiednie srodowisko w `platformio.ini`.
- Pinow ustawionych aktualnie w programie nie wolno zmieniac podczas modyfikacji kodu, chyba ze uzytkownik wyraznie wskaze, ktory pin ma zostac zmieniony i na jaka wartosc.
- Zmiany w kalibracji `HX711` powinny byc dokumentowane razem z wartosciami `zero` i `faktor`.
- Po kazdej zmianie kodu nalezy zwiekszyc `FIRMWARE_VERSION`, najlepiej o jeden krok minor, np. `3.0` -> `3.1`; przy wiekszych zmianach uzytkownik sam poda skok major, np. `3` -> `4`.
- Na poczatku zmodyfikowanego pliku dodawaj krotki komentarz opisujacy, co konkretnie zmieniono w kodzie.

## Co warto zakladac przy dalszym rozwoju

- System ma byc odporny na chwilowy brak lacznosci lokalnej i GSM.
- Najwazniejsze sa niski pobor energii, stabilnosc transmisji i przewidywalny harmonogram.
- Kod wagi i centrali powinien pozostac mozliwie prosty diagnostycznie, bo dziala w warunkach terenowych.
