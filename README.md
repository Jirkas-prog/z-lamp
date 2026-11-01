# Z-Lamp

Ovladač Zigbee světel: XIAO ESP32-C6 jako koordinátor, původní TTGO T-Display jako Wi-Fi/BLE brána a Android klient.

## Sestavení a nastavení

ESP-IDF 5.3.1. V kořeni sestavte cíl esp32c6, v gateway cíl esp32. Host testy: cmake -S tests -B build-tests, cmake --build build-tests, ctest --test-dir build-tests -C Debug. Android: cd android a spusťte gradlew assembleDebug.

## Obsah repozitáře

Zdrojové kódy, konfigurace sestavení a dostupné testy. Soukromé přístupy, záznamy instalací, zálohy zařízení a dříve sestavené binární soubory sem nepatří. Příklady vyplňte vlastními údaji až místně.

Toto je zdrojový snímek uložený 1. 11. 2026. Stav konkrétních kontrol je uveden v `VALIDATION.md`; nahrání zdrojů neznamená nové ověření na fyzickém zařízení.

## Další projekty

[Přehled projektů Jirkas-prog](https://github.com/Jirkas-prog) · [Home Assistant doplňky](https://github.com/Jirkas-prog/home-assistant-addons)
