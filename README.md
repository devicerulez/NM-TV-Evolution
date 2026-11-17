# Small TV Evolution Pro (V.1.0.7)

Firmware personalizzato in italiano per **NM-TV-154** (ESP32-WROOM-32E, 4 MB, display ST7789 240x240).
Dati meteo, qualita' dell'aria, UV e pollini da [Open-Meteo](https://open-meteo.com) (gratis, nessuna chiave API).

## Avvio

All'accensione compare il titolo **Small TV Evolution Pro** con l'indirizzo IP sotto. In questi secondi **non toccare il pulsante**: il sensore touch si calibra. Poi appare il menu.

## Uso del pulsante touch (in alto)

| Dove | 1 tocco veloce | 2 tocchi veloci | Tieni premuto |
|---|---|---|---|
| **Menu** (Meteo / Aria / Cripto / Quadranti / Sistema) | schermata successiva | schermata precedente | entra |
| **Dentro una schermata** | funzione successiva (ciclico) | funzione precedente | torna al menu |

Tieni premuto **8 secondi** (da qualsiasi punto) per aprire il portale di configurazione.

Gli stessi comandi sono nell'interfaccia web, sopra: **freccia sinistra** = doppio click, **entra/esci** = pressione lunga, **freccia destra** = click singolo.

## Schermate

Nel menu restano solo il titolo, la grafica e i pallini di scorrimento.

- **Meteo**
  1. Principale: orologio grande con barra dei secondi, data in italiano con anno, icona meteo, temperatura, riquadri centrati Min / Max / Umidita'. La riga in basso cambia ogni 4 s: condizioni, temperatura percepita, vento, pressione (Atm), alba, tramonto.
  2. Previsioni dei prossimi 3 giorni.
  3. Grafico della temperatura delle prossime 24 ore, con ora attuale, minimo e massimo.

  Nella schermata principale, a destra della temperatura, una **freccia di tendenza** (disegnata a codice): su = in aumento, giu = in calo, orizzontale = stabile (stima sulle prossime 3 ore).
- **Aria**
  1. Indice europeo EAQI su quadrante a 6 fasce con indicatore, livello a parole (ottima, buona, discreta, scadente, molto scadente, pessima), PM2.5 e PM10 con barretta colorata.
  2. **Salute** (come in SmallTV Evolution): indice UV (numero grande, livello, consiglio, scala a 5 fasce con indicatore e massimo previsto oggi) e pollini di graminacee, olivo e betulla (da assenti a molto alti). I pollini sono forniti solo per l'Europa e in stagione: altrimenti la schermata lo indica.

- **Cripto**: andamento in tempo reale delle criptovalute scelte dal web (le 20 principali: BTC, ETH, BNB, XRP, SOL, DOGE, ADA, TRX, AVAX, LINK, DOT, TON, SHIB, LTC, BCH, XLM, UNI, ATOM, NEAR, POL). Per ognuna: prezzo, variazione sul periodo mostrato e **grafico a candele** (verdi = rialzo, rosse = ribasso) con minimo, massimo e ultimo prezzo. Si passa da una cripto all'altra da sole ogni N secondi (1-60, predefinito 5); con **1 tocco** (avanti) o **2 tocchi** (indietro) si cambia subito, senza aspettare. Pallini e barra in basso mostrano quale cripto e' in corso e quanto manca. I dati arrivano da Binance (dati pubblici, nessuna chiave API) e vengono letti solo mentre la schermata e' aperta.
- **Quadranti**: quattro orologi, si cambia con 1 tocco (avanti) / 2 tocchi (indietro)
  1. **Semplice**: ora grande, data, icona meteo e temperatura.
  2. **Analogico**: quadrante rotondo con lancette di ore, minuti e secondi.
  3. **Flip**: cartellini che si girano per ore, minuti e secondi.
  4. **Matrix**: pioggia di caratteri verdi con ora, data e temperatura.
- **Sistema**
  1. **Matrix**: pioggia di caratteri verdi con un pannello di statistiche: nome della rete Wi-Fi, indirizzo IP, qualita' del segnale (a parole, percentuale e dBm), uptime e memoria libera.

Aggiornamento automatico: meteo ogni 10 minuti, aria/salute ogni 30 minuti. Ora via NTP con fuso Europe/Rome (ora legale/solare automatica).

## Temi

Gli stessi 5 temi di SmallTV Evolution: **Neon** (cyberpunk, predefinito), Aurora, Tramonto, Ghiaccio, Ambra. Si scelgono dall'interfaccia web.

## Interfaccia web

Apri `http://nmtv.local` oppure l'indirizzo IP mostrato all'avvio:

- tre tasti di navigazione (indietro, entra/esci, avanti) con la schermata attuale;
- rete WiFi, citta', formato ora 24 h / 12 h (AM/PM), tema, luminosita', inversione colori, rotazione;
- **Aggiorna**: applica subito sul dispositivo le opzioni cambiate (tema, luminosita', ecc.) senza riavviare (anche cambiando rete WiFi o citta');
- **Cripto**: scegli quali criptovalute mostrare, per quanti secondi (1-60) e l'intervallo delle candele (5 min, 15 min, 1 ora, 4 ore);
- **Schermata all'avvio**: scegli un orologio (o un'altra funzione) e all'accensione il dispositivo entra direttamente li'; con **Aggiorna** ci va subito;
- **Riavvia**: riavvia il dispositivo (le modifiche non applicate con Aggiorna vanno perse);
- **modalita' notturna**: fascia oraria (Dalle / Alle) e luminosita' notturna 0-100% (0 = schermo spento);
- **aggiornamento firmware da browser (OTA)**, senza cavo;
- in fondo, **ripristino di fabbrica** (cancella tutte le impostazioni e riavvia in configurazione).

## Aggiornamento senza cavo (OTA)

1. Apri l'interfaccia web, sezione **Aggiornamento firmware (OTA)**.
2. Scegli `nmtv-firmware-app.bin` (**non** quello "completo": il dispositivo lo rifiuta) e premi **Aggiorna**.
3. Attendi la barra al 100%: il dispositivo si riavvia da solo. Le impostazioni restano.

**Dalla 1.0.4** si aggiorna dal browser con `nmtv-firmware-app.bin`: la 1.0.7 si installa cosi', senza cavo, e le impostazioni restano. Il flash via cavo con il file "completo" serve solo se parti da una versione precedente alla 1.0.4.

## Novita' 1.0.7

- Nuova schermata **Cripto** (tra Aria e Quadranti) con candele in tempo reale, rotazione automatica e scelta dal web.

## Novita' 1.0.6

- Aggiornamento meteo ogni 10 minuti (prima 15).
- Il pulsante "Salva e riavvia" diventa **Riavvia**.
- Freccia di tendenza della temperatura nel Meteo.
- Nuova schermata **Quadranti** con 4 orologi, selezionabili anche dal web (avvio diretto).

## Novita' 1.0.5

- Nuova schermata **Sistema** con interfaccia Matrix e statistiche.
- Pulsante **Aggiorna** nell'interfaccia web.
- Modalita' notturna per la luminosita' dello schermo.
- Ripristino di fabbrica dall'interfaccia web.

## Primo flash

**Prima fai un backup del firmware originale** (SCELTA CONSIGLIATA!):

```
python -m esptool --chip esp32 --port COMx read_flash 0 0x400000 backup-originale.bin
```

Poi scrivi il firmware (cambia `COMx` con la tua porta):

```
python -m esptool --chip esp32 --port COMx --baud 460800 write_flash 0x0 nmtv-firmware-completo.bin
```

In alternativa un flasher web da Chrome/Edge (es. https://web.esphome.io) con il file `.bin` completo.

## Prima configurazione

Al primo avvio compare "Configurazione" sullo schermo:

1. Collegati dal telefono alla rete WiFi **NM-TV-Setup**.
2. Apri `192.168.4.1` nel browser (di solito si apre da solo).
3. Scegli la rete WiFi, inserisci la password, la **citta'**, il formato ora e il tema. Salva.

## Se lo schermo non e' perfetto

- Sembra un negativo: l'opzione "Inverti i colori" e' attiva di default; se serve, toglila dall'interfaccia web.
- Sottosopra: spunta "Ruota lo schermo di 180 gradi".
- Rosso e blu scambiati: in `platformio.ini` togli il `;` dalla riga `-DTFT_RGB_ORDER=TFT_BGR` e ricompila.
- Il pulsante risponde male: in `platformio.ini` togli il `;` da `-DTOUCH_DEBUG=1` per vedere i valori del sensore in basso nel menu.

## Pin usati

| Funzione | GPIO |
|---|---|
| SPI CLK / MOSI | 14 / 13 |
| DC / CS | 2 / 15 |
| RST | non collegato |
| Backlight (PWM, attivo basso) | 19 |
| Alimentazione pannello (LOW) | 21 |
| Touch | 32 (T9) |

I testi sullo schermo non usano lettere accentate (i font integrati non le hanno), la pagina web invece si'.
