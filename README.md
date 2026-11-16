# Small TV Evolution Pro (V.1.0.4)

Firmware personalizzato in italiano per **NM-TV-154** (ESP32-WROOM-32E, 4 MB, display ST7789 240x240).
Dati meteo, qualita' dell'aria, UV e pollini da [Open-Meteo](https://open-meteo.com) (gratis, nessuna chiave API).

## Avvio

All'accensione compare il titolo **Small TV Evolution Pro** con l'indirizzo IP sotto. In questi secondi **non toccare il pulsante**: il sensore touch si calibra. Poi appare il menu.

## Uso del pulsante touch (in alto)

| Dove | 1 tocco veloce | 2 tocchi veloci | Tieni premuto |
|---|---|---|---|
| **Menu** (Meteo / Aria) | schermata successiva | schermata precedente | entra |
| **Dentro una schermata** | funzione successiva (ciclico) | funzione precedente | torna al menu |

Tieni premuto **8 secondi** (da qualsiasi punto) per aprire il portale di configurazione.

Gli stessi comandi sono nell'interfaccia web, sopra: **freccia sinistra** = doppio click, **entra/esci** = pressione lunga, **freccia destra** = click singolo.

## Schermate

Nel menu restano solo il titolo, la grafica e i pallini di scorrimento.

- **Meteo**
  1. Principale: orologio grande con barra dei secondi, data in italiano con anno, icona meteo, temperatura, riquadri centrati Min / Max / Umidita'. La riga in basso cambia ogni 4 s: condizioni, temperatura percepita, vento, pressione (Atm), alba, tramonto.
  2. Previsioni dei prossimi 3 giorni.
  3. Grafico della temperatura delle prossime 24 ore, con ora attuale, minimo e massimo.
- **Aria**
  1. Indice europeo EAQI su quadrante a 6 fasce con indicatore, livello a parole (ottima, buona, discreta, scadente, molto scadente, pessima), PM2.5 e PM10 con barretta colorata.
  2. **Salute** (come in SmallTV Evolution): indice UV (numero grande, livello, consiglio, scala a 5 fasce con indicatore e massimo previsto oggi) e pollini di graminacee, olivo e betulla (da assenti a molto alti). I pollini sono forniti solo per l'Europa e in stagione: altrimenti la schermata lo indica.

Aggiornamento automatico: meteo ogni 15 minuti, aria/salute ogni 30 minuti. Ora via NTP con fuso Europe/Rome (ora legale/solare automatica).

## Temi

Gli stessi 5 temi di SmallTV Evolution: **Neon** (cyberpunk, predefinito), Aurora, Tramonto, Ghiaccio, Ambra. Si scelgono dall'interfaccia web.

## Interfaccia web

Apri `http://nmtv.local` oppure l'indirizzo IP mostrato all'avvio:

- tre tasti di navigazione (indietro, entra/esci, avanti) con la schermata attuale;
- rete WiFi, citta', formato ora 24 h / 12 h (AM/PM), tema, inversione colori, rotazione;
- **aggiornamento firmware da browser (OTA)**, senza cavo.

## Compilare il .bin con GitHub

1. Carica **tutto il contenuto** di questa cartella nel tuo repository (compresa la cartella nascosta `.github`).
2. Vai su **Actions**: il workflow "Compila firmware" parte da solo ad ogni push (oppure *Run workflow*).
3. A fine compilazione scarica l'artifact **nmtv-firmware**, che contiene:
   - `nmtv-firmware-completo.bin`: immagine intera, da scrivere all'indirizzo `0x0` (primo flash via cavo)
   - `nmtv-firmware-app.bin`: solo l'applicazione, da usare per l'**aggiornamento OTA** dall'interfaccia web (o all'indirizzo `0x10000` via cavo)
4. Se crei un tag (es. `v1.0.4`) i file vengono anche pubblicati nelle Releases.

## Aggiornamento senza cavo (OTA)

1. Apri l'interfaccia web, sezione **Aggiornamento firmware (OTA)**.
2. Scegli `nmtv-firmware-app.bin` (**non** quello "completo": il dispositivo lo rifiuta) e premi **Aggiorna**.
3. Attendi la barra al 100%: il dispositivo si riavvia da solo. Le impostazioni restano.

**Importante per la 1.0.4:** questa versione cambia la tabella delle partizioni (piu' spazio per il firmware), quindi **questa volta** va flashata **via cavo** con `nmtv-firmware-completo.bin` all'indirizzo `0x0`. Dalla 1.0.5 in poi si potra' aggiornare dal browser. Le impostazioni salvate (WiFi, citta') restano.

## Primo flash

**Prima fai un backup del firmware originale** (poi potrai tornare indietro):

```
python -m esptool --chip esp32 --port COM3 read_flash 0 0x400000 backup-originale.bin
```

Poi scrivi il firmware (cambia `COM3` con la tua porta):

```
python -m esptool --chip esp32 --port COM3 --baud 460800 write_flash 0x0 nmtv-firmware-completo.bin
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
