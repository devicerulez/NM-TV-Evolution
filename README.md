# NM-TV Meteo & Aria

Firmware personalizzato in italiano per **NM-TV-154** (ESP32-WROOM-32E, 4 MB, display ST7789 240x240).
Dati meteo e qualita' dell'aria da [Open-Meteo](https://open-meteo.com) (gratis, nessuna chiave API).

## Uso del pulsante touch (in alto)

| Dove | 1 tocco veloce | 2 tocchi veloci | Tieni premuto |
|---|---|---|---|
| **Menu** (Meteo / Aria) | schermata successiva | schermata precedente | entra |
| **Dentro una schermata** | funzione successiva (ciclico) | funzione precedente | torna al menu |

Tieni premuto **8 secondi** (da qualsiasi punto) per aprire il portale di configurazione.

## Schermate

- **Meteo**
  1. Principale: orologio grande con barra dei secondi, data in italiano con anno, icona meteo, temperatura, min/max, umidita'. La riga in basso cambia ogni 4 s: condizioni, temperatura percepita, vento, pressione (Atm), alba, tramonto.
  2. Previsioni dei prossimi 3 giorni.
- **Aria**: indice europeo EAQI su quadrante a 6 fasce con indicatore, livello a parole (ottima, buona, discreta, scadente, molto scadente, pessima), PM2.5 e PM10 con barretta colorata.

Aggiornamento automatico: meteo ogni 15 minuti, aria ogni 30 minuti. Ora via NTP con fuso Europe/Rome (ora legale/solare automatica).

## Compilare il .bin con GitHub

1. Crea un nuovo repository su GitHub e carica **tutto il contenuto** di questa cartella (compresa la cartella nascosta `.github`).
2. Vai su **Actions**: il workflow "Compila firmware" parte da solo ad ogni push (oppure *Run workflow*).
3. A fine compilazione scarica l'artifact **nmtv-firmware**, che contiene:
   - `nmtv-firmware-completo.bin`: immagine intera, da scrivere all'indirizzo `0x0` (primo flash)
   - `nmtv-firmware-app.bin`: solo l'applicazione (indirizzo `0x10000`)
4. Se crei un tag (es. `v1.0.0`) i file vengono anche pubblicati nelle Releases.

## Primo flash

**Prima fai un backup del firmware originale** (poi potrai tornare indietro):

```
python -m esptool --chip esp32 --port COM3 read_flash 0 0x400000 backup-originale.bin
```

Poi scrivi il firmware (cambia `COM3` con la tua porta):

```
python -m esptool --chip esp32 --port COM3 --baud 460800 write_flash 0x0 nmtv-firmware-completo.bin
```

In alternativa puoi usare un flasher web da Chrome/Edge (es. https://web.esphome.io, scegliendo il file `.bin` completo).
Se il dispositivo non entra in download, tieni premuto il tasto BOOT (se presente) mentre lo colleghi.

## Prima configurazione

Al primo avvio compare "Configurazione" sullo schermo:

1. Collegati dal telefono alla rete WiFi **NM-TV-Setup**.
2. Apri `192.168.4.1` nel browser (di solito si apre da solo).
3. Scegli la rete WiFi, inserisci la password, la **citta'** e il formato ora (24 h / 12 h AM/PM). Salva.

Le impostazioni si possono cambiare in seguito da `http://nmtv.local` oppure dall'indirizzo IP mostrato in basso nel menu.

## Se lo schermo non e' perfetto

- Sembra un negativo: spunta "Inverti i colori" nella pagina di configurazione.
- Sottosopra: spunta "Ruota lo schermo di 180 gradi".
- Rosso e blu scambiati: in `platformio.ini` togli il `;` dalla riga `-DTFT_RGB_ORDER=TFT_BGR` e ricompila.
- Il pulsante risponde male: apri il monitor seriale (115200) e guarda i valori `Touch: base=... soglia...` stampati all'avvio.

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
