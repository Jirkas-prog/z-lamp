# Z-lamp pro Android

Nativní česká aplikace v Kotlinu pro Android 8.0+ (API 26). Ovládá T-Display
firmware **Wi-Fi + BLE 2.10.4 (doporučená verze)** přes zabezpečené Bluetooth LE. C6 zůstává Zigbee
rozhraním s firmwarem 2.7.1. Časovače vyžadují Wi-Fi firmware 2.10.0. Aplikace nevyžaduje účet ani internet;
neobsahuje analytiku, cloudové služby nebo oprávnění INTERNET.

## Čas potvrzení příchodu — Android 1.6.1

**Nastavení → Automatický příchod → Potvrzení silného signálu** přijímá
celé nezáporné sekundy. **0 = ihned při prvním platném měření na prahu
nebo nad ním**. Kladná hodnota vyžaduje silný signál po zadanou dobu;
pokles pod práh, neplatné měření nebo mezera delší než 3,5 s čekání vynuluje.
Pevný požadavek tří měření a klouzavý průměr byly odstraněny, takže 0 s
nemá skryté potvrzovací zpoždění a 1 s nepotřebuje tři vzorky.
Měření probíhá přibližně jednou za sekundu ve společné frontě BLE.

Nově je výchozí čas 0 s. Dříve uložená hodnota zůstane zachovaná — pro
okamžité rozsvícení ji změň na 0 a ulož. Po jedné akci zůstává příchod
vyřízený až do odpojení; nastavitelné minimální odpojení se nemění.
Noční jas, vybraná žárovka a interní časovač fungují stejně jako dříve.
Stačí aktualizace APK `X:\Z-lamp-1.6.1.apk`; firmware 2.10.4 / C6 2.7.1
se nemění. Aplikaci neodinstalovávej, aby se zachovalo nastavení.

Uživatel 12. 10. 2026 potvrdil, že předchozí oprava 1.6.0 / 2.10.4 funguje.
Konkrétní scénáře dlouhodobého provozu na pozadí nebyly v potvrzení rozlišeny.

## Automatické připojování — Android 1.6.0 / Wi-Fi 2.10.4

V aplikaci otevři **Nastavení → Systémové připojování → Zapnout** a jednou
schval propojení v okně Androidu. Funkce je pro Android 12+ s podporou
Companion Device Manager. Android hlídá pevnou adresu již uložené lampy,
spouští CompanionDeviceService a udržuje proces v době její přítomnosti.
BLE komunikaci stále obsluhuje jeden společný klient aplikace. Nejde o
profil sluchátek a není nutné rušit původní Bluetooth párování.
Samotné připojení nerozsvěcí; pro rozsvícení zvlášť zapni **Automatický příchod**.
Vynucené zastavení aplikace a omezení výrobce mohou vyžadovat její otevření.

Při běžném výpadku klient zachová nativní registraci `autoConnect=true`.
Zruší nepotvrzené povely a částečné odpovědi; po návratu znovu nastaví GATT
a načte aktuální stav. Chyba 133 vyvolá nové spojení s odstupem ve službě.
Opakované Připojit ke stejné lampě už neruší čekání ani nevypíná Příchod.
Hledání jiné lampy automatiku jen pozastaví; její výběr, ruční Odpojit,
Zapomenout nebo Ukázka ji vypnou. Vypnutí systémového připojování nemění
párovací klíče. Schválené propojení s Androidem se zruší při Zapomenout
nebo výběru jiné lampy; Bluetooth bond se nemaže.

Wi-Fi 2.10.4 odstranila duplicitní zásobníkový buffer odpovědi (1537 bajtů,
nahrazen 128bajtovým bufferem chyby); platné odpovědi vznikají přímo v cache.
`/api/ble` nově vrací pevnou adresu, `knownOnConnect`, `repeatPairings`,
`keyStoreStatus` a osm posledních událostí v `history` (uptime ms / název / kód).
Zachytí načtení a uložení klíčů, opakované párování, zaplnění úložiště,
šifrování i důvod odpojení. Klíče samotné se do diagnostiky nevypisují.

Na hardwaru OTA zachovala nastavení a původní bond. Prošlo 100 požadavků
ve dvou spojeních, dlouhé odpovědi a vícefragmentové UTF-8 zápisy. Po restartu
BLE se načetly uložené klíče a druhé spojení bylo rozpoznáno jako známé,
bez opakovaného párování. ID i boot brány zůstaly během testu stejné.
Příčina uživatelem hlášeného občasného opakovaného potvrzení na Samsungu
není prokázaná. Systémová přítomnost a chování Samsungu v pozadí vyžadují
zkoušku na telefonu po instalaci 1.6.0; telefon není dostupný přes ADB.

Podklady: [BLE na pozadí](https://developer.android.com/develop/connectivity/bluetooth/ble/background),
[CompanionDeviceService](https://developer.android.com/reference/android/companion/CompanionDeviceService).

## Stabilita BLE přenosu — Wi-Fi 2.10.3

Původní firmware odmítal další zápis chybou ATT 17, když klient už dostal
celou delší odpověď, ale pracovní úloha ještě čekala na její rádiové potvrzení.
Na lampě se to zopakovalo při čtvrtém požadavku po `get_light`.
Příjem dalšího příkazu se nyní uvolní před odesláním odpovědi; jeho vykonání
dál čeká na dokončení předchozí odpovědi. Připravený další příkaz se nemaže.

Po opětovném připojení se také čeká na první požadavek aplikace před
odesíláním oznámení změn. Samotné obnovení uloženého CCCD nestačí:
telefon může teprve zapínat příjem a jinak by zachytil až prostřední fragment.
Přísná kontrola fragmentů a zabezpečení zůstávají zachované.

Oba regresní testy používají skutečný příjem a pracovní úlohu firmwaru;
na původním kódu selhaly, po opravě prošlo všech 13 hostitelských sad.
Na lampě pak prošlo **100 požadavků** ve dvou spojeních, dlouhé odpovědi,
vícefragmentové UTF-8 zápisy a opětovné připojení s uloženým klíčem.
Testy neposílaly změny světel. Před prvním požadavkem nepřišla žádná indikace.
OTA 2.10.3 je potvrzená; nastavení a spárování telefonu zůstalo zachované.
Uživatel na Samsungu následně potvrdil změny jasu a barvy i opětovné
připojení bez chyby zápisu nebo fragmentu. Pozdější problém s připojováním
je řešen ve vydání 1.6.0 / 2.10.4; dlouhodobý provoz dál vyžaduje ověření. Podrobnosti: `releases/Wifi-BLE-2.10.3/hardware-2026-10-11.json`.

## Oprava párování — Wi-Fi 2.10.2

V IDF 5.3.1 se při vynuceném `MYNEWT_VAL_BLE_SM_SC_ONLY=1` požadavek
Secure Connections přijal, ale obsluha `ble_sm_pair_req_rx` nevytvořila
odpověď ani nespustila další krok. Párování proto zůstalo viset do timeoutu;
Android pak mohl vidět chybu 19 při ukončení spojení lampou.
Vynucení bylo odstraněno a sestavení nyní tuto chybnou volbu odmítne.
Legacy párování je dál zakázané, Secure Connections zapnuté a příkazy
vyžadují spárované spojení s 128bitovým šifrováním. PIN ani práh signálu nejsou.

Na fyzické lampě prošlo nové párování z Windows během několika sekund,
šifrovaný příkaz `get_info` a opětovné připojení s uloženým klíčem po restartu
Bluetooth. Dočasný záznam PC byl poté odstraněn na obou stranách.
OTA je potvrzená; nastavení, scény, skupiny a stavy světel zůstaly zachované.
Android **1.5.1** a C6 **2.7.1** se nemění. Uživatel na Samsungu potvrdil:
„Připojeno, světla fungují.“ V aplikaci stačí **Párovat → zlamp** a případné systémové potvrzení.
Diagnostika `/api/ble` nově uchovává výsledek párování, délku spojení,
stav šifrování a první důvod místního ukončení spojení.
Podrobný záznam: `releases/Wifi-BLE-2.10.2/hardware-2026-10-11.json`.

## Předchozí oprava — Android 1.5.1 / Wi-Fi 2.10.1

NimBLE měl z původního PIN režimu úroveň 4, která odmítá požadavek bez MITM.
Úroveň 2 nyní odpovídá Just Works; Secure Connections, zákaz legacy párování
a 128bitové šifrování příkazů zůstávají. Sestavení hlídá shodu nastavení.
Android čte výsledek ze zprávy o párování i při zpoždění interního stavu;
chybějící zprávu zachytí kontrolou nativního stavu. Opakované oznámení spojení
neprodlužuje čekání přes 30 sekund. 97 testů Androidu prošlo.
OTA 2.10.1 ověřena. Windows přečetl veřejné GATT info, ale párování stále
skončilo FAILED. Dokončení párování a šifrované ovládání se Samsungem je
potřeba ověřit na telefonu; zatím není potvrzeno.

## Ranní budík Samsung a časovače (Android 1.5.0 / Wi-Fi 2.10.0)

V aplikaci otevři **Nastavení → Ranní světlo s budíkem**. Povol Z-lamp přístup
k oznámením v nabídnutém systémovém nastavení a pro aplikaci nastav baterii
**Bez omezení**. Nejprve spáruj lampu, potom funkci zapni a ulož.
Výchozí okno je **05:00 včetně až 10:00 bez koncového okamžiku**, jas **10 %**
a vypnutí za **60 minut od oznámení zvonění**. Časy, jas, dobu 1–1440 minut
i cíl (všechny žárovky, skupina, jedna žárovka) lze změnit. Barva používá paměť
nebo nastavený preset probuzení. Displej se do ranního rozsvícení nezahrnuje.

Používají se pouze Hodiny Samsung (`com.sec.android.app.clockpackage`). Android
poskytne čas dalšího budíku přes `AlarmManager.getNextAlarmClock()`. Spuštění
vyžaduje čerstvé oznámení kategorie alarm s full-screen intentem od stejných
Hodin, poblíž dříve zveřejněného času budíku. Oznámení o příštím budíku,
souhrny, jiná aplikace a stará oznámení po restartu nic nerozsvěcují.
Text oznámení se neukládá. Diagnostika ukazuje jen kategorii, přítomnost celého
okna a shodu času. Oznámení před prvním povolením přístupu se neopakují.
Konkrétní podobu oznámení Hodin Samsung je nutné ověřit krátkým budíkem
na telefonu; pokud systém neposkytne potřebné údaje, aplikace nic nespustí.

Jednou za místní kalendářní den: odložení budíku, opakovaná oznámení ani
restart aplikace neprodlužují odpočet a nerozsvítí ručně zhasnutá světla.
Spotřeba události se uloží před příkazem. Připojení čeká nejvýše minutu;
nepovedené ranní rozsvícení se neodloží na dobu pozdějšího příchodu k lampě.
Po potvrzení běží vypnutí ve Wi-Fi desce, nezávisle na telefonu a BLE.
Zvolený jas a časovač se odešlou jediným příkazem, před rozsvícením se
termín bezpečně uloží. Pokud se potvrzení UART ztratí, vypnutí zůstává hlídané.

**Časovač vypnutí** je také u ovládání světel, v nastavení aplikace a na webu
v záložce Světla. Nastavení samotného časovače nerozsvěcí ani nemění jas.
Délka je 1–1440 minut, zbývající čas lze přečíst a časovač zrušit pro vybraný
cíl. Skupina se při uložení rozvine na své současné žárovky. Jas a barva
odpočet zachovají; ruční OFF ho zruší. Probíhající scéna zahrnující žárovku
se při vypršení zastaví, aby ji další snímek nerozsvítil znovu.

Odpočet nevyžaduje otevřený web ani připojenou aplikaci. Wi-Fi deska i C6
musí zůstat napájené. Pokud je Zigbee spojení při vypršení nedostupné,
vypnutí se opakuje po pěti sekundách do potvrzení. Trvale se ukládají cíle:
po restartu desky se čekající vypnutí provede ihned po obnovení spojení,
nikoli až po původním zbývajícím čase (sestava nemá zálohovaný čas).
Noční příchod má dál původní chování: jeho krátký časovač ruční změna zruší.

Podklady: [Android AlarmManager](https://developer.android.com/reference/android/app/AlarmManager#getNextAlarmClock()),
[NotificationListenerService](https://developer.android.com/reference/android/service/notification/NotificationListenerService),
[BLE na pozadí](https://developer.android.com/develop/connectivity/bluetooth/ble/background).

## Instalace a první spojení

**11. 10. 2026:** Wi-Fi **2.10.4** nahraná přes OTA, start a krátký interní
časovač bez klienta ověřené. C6 **2.7.1** zůstává beze změny. Nastavení a žárovky
zachované, BLE běží s trvale dostupným párováním. Web: `http://192.0.2.20`.
APK 1.6.1 je na `X:\Z-lamp-1.6.1.apk`; skutečný Samsung budík zbývá vyzkoušet.

1. Firmware `releases/Wifi-BLE-2.10.4/lamp_gateway.zota` je již nahraný do Wi-Fi
   desky a verze **2.10.4** potvrdila start. C6 **2.7.1** se nemění.
2. Nainstaluj `X:\Z-lamp-1.6.1.apk` přes předchozí aplikaci. Podpis zůstává
   stejný; aplikaci neodinstalovávej, ať zachováš její nastavení.

3. Spusť Z-lamp a povol **Zařízení v okolí**. Android 8–11 pro BLE hledání vyžaduje
   povolení i zapnutí polohy; aplikace souřadnice nečte ani neukládá.
4. Stiskni **Párovat**, najdi svoji lampu a **klepni na ni**. Žádný PIN, web,
   tlačítko na desce, časové okno ani požadovaný práh síly signálu.
   Pokud Android zobrazí systémové potvrzení párování, přijmi ho.
5. Příště stačí **Připojit Z-lamp**; stávající uložená spojení se zachovávají.

6. Pro automatické připojování schval **Nastavení → Systémové připojování**.

## Dlaždice v horní liště (1.4.0)

**Nastavení → Světla v horní liště → Přidat do horní lišty.** Android 13+ nabídne
systémové přidání; na Androidu 8–12 stáhni lištu dvakrát, otevři Upravit/tužku
a přetáhni Z-lamp mezi aktivní dlaždice. Podržení dlaždice otevře její nastavení.
Vybrat lze všechny žárovky (bez displeje), jednu skupinu nebo konkrétní světlo.

Klepnutí funguje bez otevřené aplikace a **nevolá odemykání telefonu**. Telefon
musí mít zapnuté BLE, již uloženou lampu a udělené oprávnění Zařízení v okolí.
Po restartu je potřeba jednou telefon odemknout. Dostupnost stažené lišty na
zamčené obrazovce určuje i nadstavba a nastavení výrobce; konkrétní telefon je
potřeba vyzkoušet. Nové párování se ze zamčené dlaždice nikdy nezahajuje.

Před přepnutím se načte stav vybraných světel z brány. Pokud některé svítí,
výběr zhasne; jinak se zapne prostým ON a použije nastavení probuzení žárovek.
Nejde o časový příchod. Ruční příkaz zruší noční časovač daných světel a vyřídí
příchod pro současné BLE spojení, aby automatika nepřepsala ruční zhasnutí.
Neznámý stav nebo samé nedostupné žárovky příkaz zastaví, nikoli odhadnou ON.

Krátká foreground služba přežije zavření lišty, ukáže dočasné oznámení a po
příkazu skončí; celý pokus trvá nejvýše 60 sekund. Další klepnutí během příkazu
se ignoruje. Příkaz se po chybě neopakuje naslepo. Sdílí se existující GATT
spojení s aplikací/Příchodem; zapnutý Příchod se dlaždicí nevypíná ani nezapíná.
Samotné otevření lišty nevytváří nové BLE spojení. Pokud už spojení existuje,
viditelná dlaždice obnovuje stav jednou za čtyři sekundy.

## Párování bez přípravy (Wi-Fi 2.9.2)

Telefon se vybírá v aplikaci přes **Párovat → vybrat lampu**. Firmware přijímá
nové telefony vždy při zapnutém Bluetooth. Síla signálu, vzdálenost, web nebo
tlačítko na desce nejsou podmínkou. Android spravuje nativní LE Secure Connections
Just Works a ukládá šifrovací klíče; může zobrazit potvrzení, nikoli zadání PINu.
Staré nastavení prahu pro párování se ignoruje. Práh automatického příchodu
zůstává nezávislý a zachovaný. Staré webové API `pair:true` pouze zapne BLE,
`pair:false` skryje případnou nápovědu LCD a nikdy nezavře párování.

K lampě se připojuje jeden telefon současně. Při změně telefonu použij Odpojit.
Ukládají se čtyři telefony; další nahradí nejstarší uložené spojení pomocí
[standardního správce úložiště NimBLE](https://github.com/apache/mynewt-nimble/blob/master/nimble/host/src/ble_store_util.c).
Párování nepředstírá ověření totožnosti aplikace: každý kompatibilní klient
v dosahu se může připojit podle požadovaného otevřeného režimu.
Po smazání klíčů pouze na jedné straně může Android vyžadovat odebrání starého
záznamu v nastavení Bluetooth. „Zapomenout výběr lampy“ v aplikaci maže pouze
uloženou adresu, nikoliv systémové párování ani Zigbee žárovky.

## Automatický příchod 1.3.0

**Nastavení → Automatický příchod.** Funkce je ve výchozím stavu vypnutá.
Nejprve připoj skutečnou lampu, vyber světlo / skupinu / všechny žárovky,
nastav práh a zapni přepínač; nastavení potvrď tlačítkem Uložit. Pro konkrétní
sadu světel si předem vytvoř skupinu. V nabídce **Večerní a noční režim** vyber
jednu noční žárovku a uprav časový plán, nebo časový režim vypni. Bez časového
režimu se odesílá pouze ON a každá žárovka použije svou paměť či preset probuzení.

- Výchozí práh **−60 dBm** je pouze výchozí hodnota pro kalibraci, ne odhad 2 metrů.
  Nastavit lze −95 až −30 dBm. −50 dBm je silnější než −70 dBm.
- Stav signálu sleduj přímo u dveří i z chodby, s telefonem v obvyklé kapse.
  Tlačítko **Použít aktuální signál jako práh** zachytí čerstvé měření.
  Ve viditelném dialogu nastavení je automatické rozsvěcení pozastavené.
- **Potvrzení od 0 s výš**: 0 spustí akci při prvním platném měření na prahu
  nebo nad ním. Kladný čas musí pokrýt souvislou řadu dostatečně silných měření.
  Pokles pod práh, neplatné měření nebo mezera nad 3,5 s potvrzování přeruší.
  Nový výchozí čas je 0 s, předchozí uložená hodnota zůstává zachovaná.
- Po odeslání ON je příchod **vyřízený až do odpojení**. Ruční zhasnutí,
  zeslabení / zesílení signálu, otevření nastavení nebo vypnutí a zapnutí
  funkce bez přerušení BLE spojení ho znovu nespustí. Odchod nezhasíná.
- **Minimální odpojení 0–120 s**: výchozí 0 znamená každý nový spoj. Například
  30 s ignoruje rychlé obnovení po krátkém výpadku. Rozhoduje doba od ztráty
  připraveného spojení do dokončení nového zabezpečeného spojení.
- Neúspěšný / nepotvrzený ON se v témže spojení neopakuje. Nové spojení při
  prodlevě 0 znovu povoluje příchod, i když předchozí spojení selhalo během ON.
  Stav „příkaz potvrzen bránou“ není důkaz optického rozsvícení každé žárovky.

Automatika patří tomuto telefonu a jedné uložené bráně. Síla signálu se měří
vůči **T-Display**, nikoli vůči jednotlivým Zigbee žárovkám. Tělo, kapsa, stěny
a poloha antény měření ovlivňují; RSSI samo nerozliší pokoj od chodby, pokud
jsou na obou místech podobné hodnoty.

### Večerní a noční režim

Plán vyhodnotí aktuální **místní čas a časové pásmo telefonu při příchodu**.
Výchozí denní jas je 100 %. Od 21:00 do 23:00 logaritmicky klesá ke 1 %:
ve 22:00 vychází 10 %, ve 22:30 přibližně 3 %. Od 23:00 do 7:00 se použije
pouze vybraná noční žárovka, 1 %, teplá bílá 2700 K a zhasnutí po pěti minutách.
Časy, oba jasy, žárovku, bílou i délku 1–60 minut lze změnit. Na úrovni Zigbee
je 1 % zaokrouhleno na 3 z 254 stupňů. Plán mění nové příchody; průběžně
nestmívá již zapnuté osvětlení. V časovém režimu jas určuje plán, barvu během
dne/večera paměť nebo preset žárovky. Běžné světlo či skupina musí obsahovat
Zigbee žárovky, nikoli displej.

Wi-Fi **2.9.0** poskytuje nové `arrival` RPC. Automatika přeskočí světla,
která již svítí, nemají známý dostupný stav, nebo při běžící scéně celý příchod.
Nevypíná ostatní žárovky. V noci se časovač smí vztahovat pouze na jedno ID
žárovky; skupiny a `all` jsou pro časovač odmítnuté. Časovač se uloží do NVS
**před** odesláním ON. Běží v bráně bez telefonu; při výpadku UART čeká a OFF
opakovaně předává C6. Po restartu brány se uložený nedokončený časovač považuje
za vypršený a OFF se provede, jakmile je dostupná C6. Po výpadku napájení se tedy
nezačíná dalších pět minut znovu. Samotné žárovky potřebují trvalé napájení.

Ruční ovládání dané žárovky přes aplikaci, web, IR nebo presetové tlačítko,
spuštění scény pro ni a její odebrání časovač zruší. Změna jiné žárovky ho neruší.
Zrušení i vypršení se ukládají; při chybě zápisu se příkaz odmítne, aby nezůstal
skrytý časovač pro nově ručně zapnuté světlo. Potvrzení bránou/C6 nezaručuje
fyzické provedení nedostupnou žárovkou; doručení přes Zigbee je třeba ověřit.

C6 **2.7.1** používá při zapnutí jediný Zigbee příkaz Move to Level with On/Off
s cílovým jasem a nulovým přechodem. Neposílá před ním samostatné ON, které by
mohlo krátce obnovit starý vysoký jas. Změny jasu už zapnutých světel zachovávají
svůj nastavený přechod. Gateway vyžaduje `deviceApi >= 4` pro časový příchod;
starší firmware je bezpečně odmítnut, bez náhradního zapnutí na plný jas.
Skutečný průběh světla včetně případného optického záblesku musí potvrdit
fyzická zkouška konkrétní Hue žárovky.

Oba OTA balíčky jsou připravené, **nejsou nahrané**. Při dokončení sestavení
lampa na poslední adrese `192.0.2.20` neodpovídala. Instalace nevyžaduje kabel.
Při starším známém problému `update_memory` vypni BLE ve webu, restartuj bránu,
proveď OTA a potom BLE znovu zapni. Nezapínej kvůli tomu automaticky hotspot.

### Připojení na pozadí a baterie

Aplikace sdílí jediný BLE klient mezi obrazovkou a službou Androidu typu
`connectedDevice`. U již spárované lampy čeká pomocí nativního `autoConnect`;
nehledá neustálým aktivním skenováním a v pozadí nezahajuje nové párování.
Po chybě obnovuje spojení s prodlužovaným odstupem do 60 s; rozpracované
uživatelské příkazy přes výpadek nepřenáší. Čtení RSSI sdílí frontu GATT
s příkazy, aby se vzájemně nepřekrývaly. Po vyřízeném příchodu se pravidelné
měření zastaví, spojení zůstává zachované.

Povol **oznámení** a v nastavení baterie telefonu nastav pro Z-lamp **Bez omezení**.
Oznámení ukazuje čekání / připojení / vyřízený příchod a umožňuje funkci vypnout.
Služba se pokusí obnovit po restartu telefonu, aktualizaci APK i po ukončení
procesu systémem. Vynucené ukončení Androidem nebo uživatelem a omezení výrobce
mohou vyžadovat opětovné otevření aplikace. Běžné zavření její obrazovky
spojení nepřeruší. Ruční Odpojit, připojení jiné lampy nebo Ukázka automatiku
vypnou; hledání ji pouze pozastaví. Připojení ke stejné lampě ji zachová.

Aby potvrzování fungovalo se zamčenou obrazovkou, služba drží CPU vzhůru pouze
při připojené lampě a nevyřízeném příchodu. Používá obnovované 60s časové zámky,
které uvolní při rozsvícení, odpojení, kalibraci nebo vypnutí služby. Pokud je
telefon dlouho připojený, ale pod prahem, může režim zvýšit spotřebu baterie.
Telefon ani aplikace nenahrávají polohu do sítě a funkce nepotřebuje Wi-Fi.

Ověřit na skutečném telefonu: příchod se zamčenou obrazovkou → jednou rozsvítit;
ručně zhasnout → chodba a zpět bez odpojení nesmí znovu rozsvítit; odejít z
Bluetooth dosahu → vrátit se → musí znovu rozsvítit. Dále vypnutí/zapnutí
Bluetooth, restart telefonu, delší pobyt mimo dosah a výdrž baterie přes noc.
Tyto fyzické zkoušky nelze nahradit unit testy; telefon při sestavení nebyl v ADB.

Implementace vychází z primárních podkladů Androidu:
[BLE na pozadí](https://developer.android.com/develop/connectivity/bluetooth/ble/background),
[služba connectedDevice](https://developer.android.com/develop/background-work/services/fgs/service-types#connected-device),
[čtení RSSI](https://developer.android.com/reference/android/bluetooth/BluetoothGatt#readRemoteRssi()),
[výjimky pro restart služby](https://developer.android.com/develop/background-work/services/fgs/restrictions-bg-start).

### Ověření vydání 1.3.0

67 testů Androidu prošlo (jednorázový příchod, výpadky, RSSI, životní cyklus
služby, půlnoc, noční příkaz a odmítnutí starého firmwaru), lint: 0 chyb,
23 upozornění převážně na české texty přímo v kódu. Prošlo všech 11 sad host
C testů včetně skutečné logiky brány pro časovač a konstrukce Zigbee příkazu.
Oba firmwary byly sestaveny ESP-IDF, velikosti a OTA obálky ověřeny. APK má
stejný podpis jako 1.2.0. Nativní snímky nastavení jsou v release/screenshots.
Optické chování Hue a dlouhodobý BLE provoz v pozadí zbývá ověřit s telefonem.

## Vlastní vizuální styl 1.2.0

„Noční světelné studio“: téměř černé pozadí, limetkový akcent, vlastní znak Z-lamp
a ikona aplikace. Náhledy světel a oblíbených barev mají podobu svítící čočky;
barva jemného světla za hlavním ovládáním vychází z požadovaného odstínu.
Výrazný číselný jas doplňuje posuvník s jemnou stupnicí, barevné kolo má vlastní
lem se značkami a spodní navigace tvoří samostatný zaoblený panel. Karty scén
přebírají barvy jejich palety. Ukázka má navíc scény Zlatá hodina a Noční proud;
do skutečné lampy se tyto ukázkové scény nepřidávají.

Grafika je kreslená nativně, bez obrázkových balíčků, nových knihoven a trvalé
animace na pozadí. Zůstává velká dotyková plocha, nativní posuvník i systémové
párování Androidu. Světelná záře je grafický náhled požadované barvy; potvrzený
stav žárovky se nadále čte z lampy. Firmware ani BLE protokol se nemění.

## Nové v 1.1.0

- Tmavé grafitové rozhraní s levandulovým zvýrazněním, vlastními linkovými ikonami,
  zaoblenými tlačítky s odezvou na dotyk a označenou aktivní položkou spodního menu.
- Průvodce **Připravit → Najít → Propojit**: zapamatovaná IP webu, kontrola adresy,
  průběh hledání, síla signálu, prázdný výsledek, pomoc a opakování připojení.
  Web se otevírá v prohlížeči; uživatel na něm zahájí párování. Kód se stále zadává
  do systémového dialogu Androidu (případně z oznámení), nikoli do falešného formuláře.
- Široké posuvníky s barevnou stupnicí, bílá/barva, velké tlačítko napájení,
  náhled požadované barvy a přesný odstín rozbalitelný pod barevným kolem.
  Posuvníky zůstávají nativní `SeekBar` pro TalkBack a klávesy. Jas 1 % nevypíná světlo.
- Čtení, Večer, Oceán a Růžová jako rychlé předvolby bez změny jasu. Navíc šest
  vlastních oblíbených barev s jasem pro každou kombinaci lampy a výběru světel.
  Ukládají se jen v telefonu; podržení vlastní barvy otevře potvrzení odebrání.
  Předvolba nemění zapnutí světla. Ukázka má vlastní oddělené oblíbené.
- Karty scén s paletou a režimem, přehlednější nastavení a návod pro přidání Zigbee
  žárovky. Detaily scén se čtou přes existující protokol; firmware se nemění.

Inspirace: [ovládání světel Home Assistant](https://www.home-assistant.io/blog/2023/03/01/release-20233/),
[oblíbené barvy Home Assistant](https://www.home-assistant.io/blog/2023/06/07/release-20236/)
a [posuvníky Material Design](https://m3.material.io/components/sliders/overview).
Vlastní vzhled a ikonky; žádná převzatá značka ani nové produkční závislosti.

Od verze 1.0.1 aplikace zahajuje systémové párování (`createBond`) ještě před
otevřením GATT. Čeká na dokončení párování konkrétní lampy, hlásí odmítnutí Androidem
a ignoruje pozdní události po zrušení pokusu. Tento bezpečnostní postup zůstává
zachovaný. Nový průvodce vyžaduje závěrečné vyzkoušení na fyzickém telefonu.

## Ovládání

- **Světla:** výběr jedné žárovky, všech žárovek, displeje nebo skupiny; ON/OFF,
  jas, teplota bílé, barevné kolo, odstín a sytost. Posuvníky ukazují požadovaný stav,
  karty poslední stav potvrzený žárovkou. U skupiny vycházejí hodnoty z prvního člena.
  Změna kteréhokoli parametru platí pro celý výběr. 100 % sytosti se posílá jako 100,
  směšování barev provádí společná logika firmwaru.
- **Nastavení světla:** název, identifikace, délka přechodu, kompatibilita XY,
  preset probuzení a chování po přivedení napájení. Presety zachytí požadované hodnoty
  konkrétního světla při otevření nastavení. Stav zápisu do žárovky je potřeba ověřit
  opětovným otevřením nastavení. Běžné odpárování i Force remove vyžadují potvrzení.
- **Skupiny:** vytvoření, změna názvu a členů, smazání skupiny.
- **Scény:** 12 hotových režimů a vlastní paleta 2–8 barev, výběr barevným kolem,
  jas, výdrž, plynulost přechodu, seznam a pořadí žárovek s počátečním krokem každé
  žárovky. Scéna pokračuje na lampě i po odpojení telefonu.
- **IR ovladač:** vlastní výběr světel, teplota a jas bílé, automatické nebo vlastní
  kroky jasu, přiřazení FLASH / STROBE / FADE (MODE) / SMOOTH k uloženým scénám.
  Prázdný seznam světel používá dosavadní IR cíl z webu; „Bez přiřazení“ tlačítko
  deaktivuje. Zapnutí přijímače a učení kódů zůstává ve webu.
- **Nastavení:** otevření/zavření Zigbee párování, síťová adresa a odkaz na web pro
  Wi-Fi, displej a OTA obou desek. Telefon musí být ve stejné síti nebo hotspotu.
  Tato verze aplikace nepřenáší firmware přes BLE.

Volba **Prohlédnout ukázku bez lampy** zpřístupní obrazovky bez hardwaru. Vždy je
označena „UKÁZKA“; změny jsou pouze v paměti telefonu a nikdy nejdou na skutečnou
lampu. Ukázka nespouští skutečnou animaci scén. Data ukázky se při novém otevření
ukázky nebo obnově aktivity vrátí do výchozího stavu.

## Komunikace a výpadky

Kontrakt: `../docs/wifi-ble-v1.json`. Nativní Android GATT, write-with-response a
indications, systémové párování s kódem. Žádná knihovna Bluetooth nebo server navíc.
Jeden rozpracovaný požadavek, maximálně 32 čekajících, odstup začátků zpráv nejméně
110 ms. Sousední změny posuvníků stejného cíle se slučují. Příkaz ON zůstává samostatný,
aby fungoval preset probuzení. Cíl je určen už při dotyku a pozdější výběr jej nemění.

MTU 23–247, zprávy nejvýše 1536 bajtů UTF-8, kontrola pořadí fragmentů a timeoutů.
Po odpojení se fronta ruší. Není automatické opakování mutací s nejistým výsledkem.
Znovu se připoj a načti skutečný stav před opakováním příkazu. Hledání končí po
12 sekundách; aplikace nespouští službu na pozadí ani nekonečné hledání.

## Sestavení

JDK 17, Android SDK platform 35, Build Tools 34.0.0, Gradle 8.9, Android Gradle
Plugin 8.7.3, Kotlin 2.0.21. Produkční závislost je pouze Kotlin standard library;
JUnit, org.json a Robolectric jsou jen pro testy.

Na Windows s českými znaky v cestě použij skript, který kopíruje zdroje do ASCII
staging adresáře; SDK ani jiné projekty nemění:

```powershell
./tools/build_android.ps1 -Release `
  -Sdk 'C:/Android/sdk' `
  -Jdk 'C:/Java/jdk-17' `
  -Gradle 'C:/Gradle/gradle-8.9/bin/gradle.bat'
```

Spustí jednotkové/UI testy, Android lint a podepsané sestavení; výstup je
`build/android/Z-lamp-1.2.0.apk` a `build/android/reports`. Bez `-Release` vznikne
debug APK s jiným podpisem. Debug APK nelze instalovat přes vydaný podepsaný APK.

První `-Release` vytvoří stabilní lokální podpisový klíč v
`.cache/android-signing/zlamp.p12` a heslo v `signing.json` vedle něj. Tyto dva
soubory uchovej soukromě pro budoucí aktualizace. Nejsou součástí APK ani release
balíčku. Změna podpisového klíče by vyžadovala odinstalování aplikace.

Ve standardním ASCII checkoutu lze použít `android/gradlew.bat testDebugUnitTest
lintDebug assembleDebug` (nastav JAVA_HOME a SDK v local.properties). Pro release
z Gradle nastav ZLAMP_KEYSTORE a ZLAMP_STORE_PASSWORD na existující klíč.

## Ověření a omezení

Verze **1.4.0 (11. 10. 2026)**: **84 testů**, lint bez chyb (23 stávajících
varování), shodný podpis APK. Nové testy pokrývají dlaždici na API 26/35,
zamčený telefon bez otevření aktivity, čtení čerstvého stavu, ON bez přepsání
presetu, dvojité klepnutí, časový limit, skupiny, odmítnuté oprávnění a chybějící
bond. Ověřena i automatická volba blízké lampy a čekání na Just Works bond bez
`createBond()` v nové větvi. Robolectric 4.14 má chybu v simulaci destrukce
TileService; testy dlaždice používají `onStopListening()` pro úklid, zatímco
životní cyklus samostatné foreground služby je testovaný včetně destrukce.
Nativní UI bylo vykreslené a zkontrolované. Prošlo 11 sad testů firmwaru,
webový integrační test a kontrola OTA balíčků. **Hardware zatím neověřen:**
lampa na poslední IP neodpovídala, ADB telefon nebyl připojený. Nový firmware
ani APK se na zařízení v tomto běhu nenahrávaly.

Verze **1.2.0 (8. 10. 2026)**: znovu prošlo všech **33 testů** a lint bez chyb.
Vizuálně zkontrolováno ovládání bílé a barvy, párovací průvodce, scény a úzký
telefon s písmem 130 %. Podpis zůstává shodný s předchozími verzemi; nová grafika
nepřidává trvalou animaci na pozadí. Náhledy jsou v `releases/Android-1.2.0/screenshots/`.
Fyzický telefon v tomto běhu nebyl připojený; protokol a firmware se neměnily.

Verze **1.1.0 (8. 10. 2026)**: prošlo **33 testů**, lint nemá chyby; podpis APK je
ověřený a shodný s verzí 1.0.1. Náhledy skutečných Android views jsou uložené
v `releases/Android-1.1.0/screenshots/`. Kontrolováno také 320 dp a písmo 130 %.
Testy zahrnují průvodce, validaci webové adresy, oblíbené pro jednotlivé cíle,
zachování plné sytosti, ovládání jasu klávesami a stálou polohu barevného kola
při míchání bílé. Telefon nebyl připojen přes ADB; nový vzhled a průvodce je ještě
potřeba vyzkoušet na skutečném zařízení. Firmware tato aktualizace nemění.

Host testy pokrývají fragmentaci MTU 23–247, hranice velikosti, UTF-8, neplatné
fragmenty, timeouty, frontu a její zrušení, zachování ON presetu a nezávislost
ukázkových dat. Robolectric vykresluje skutečné Android views a ověřuje hlavní
obrazovky, uložení scén a IR nastavení, barevnou paletu, zamítnutí oprávnění a
potvrzování Force remove. Screenshoty jsou v reportech.

**Zatím neověřeno na fyzickém telefonu a lampě:** první párování, rádiový přenos,
stabilita Wi-Fi + BLE, uspávání telefonu a běh OTA za připojeného telefonu.
Testy obrazovek nenahrazují tyto hardwarové zkoušky. Firmware 2.8.3 byl do sestavy
nahrán přes Wi-Fi OTA; dokončení párování na Androidu je potřeba ověřit zvlášť.

Podklady: [Android BLE oprávnění](https://developer.android.com/develop/connectivity/bluetooth/bt-permissions),
[GATT klient](https://developer.android.com/reference/android/bluetooth/BluetoothGatt),
[Systémové párování](https://developer.android.com/reference/android/bluetooth/BluetoothDevice#createBond()),
[AGP 8.7](https://developer.android.com/build/releases/agp-8-7-0-release-notes),
[Robolectric kompatibilita](https://robolectric.org/compatibility_table/).
