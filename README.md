# T-Display Bin Day

A bin collection reminder for the LilyGO T-Display and T-Display-S3. It downloads your property's collection calendar from Horsham District Council's website and shows the next collection and which bins go out, with a "Bins out tonight" reminder the day before.

## What it shows

- **Headline**: "Bins out tonight" the day before a collection (from the time set by the Reminder setting, "Tomorrow" before then), "Collection today" on the day, otherwise the date of the next collection with the number of days to go underneath
- **Bins**: a coloured bin for each one being collected: green (refuse), blue (recycling), orange-top food caddy and brown (garden waste)
- **Bins are out**: once you've put the bins out, press CHANGE and the headline turns to a green "Bins are out" until collection day. Press it again to undo
- **Changes**: if the council marks a collection as different from the usual arrangements (shown in bold on their site, for example over Christmas), "(changed)" appears in red
- **Along the bottom**: a reminder to press CHANGE while the bins need putting out, otherwise the collection after the next one
- **Along the top**: WiFi status and when the calendar was last downloaded

The calendar is downloaded at start-up and every 6 hours after that, or every 15 minutes after a failed attempt. The last good copy is kept in flash, so the device keeps working through power cuts and when the council's website is down.

## Settings menu

Outside the menu, CHANGE marks the bins as out (see above). If the screen is off for the night, the first press of either button just lights it up for 30 seconds.

| Action | What it does |
|---|---|
| Hold MENU (0.8 s) | Open the settings menu. A yellow bar at the top shows the item and its value |
| Press MENU | Go to the next item |
| Press CHANGE | Step the value forwards. Changes show straight away |
| Hold CHANGE | Step the value backwards |
| Hold MENU again | Save and close. The menu also saves and closes after 10 seconds without a press (60 seconds on Info) |

| Item | Values |
|---|---|
| Brightness | 10%, 25%, 50%, 75% or 100% |
| Night | Off, Dim (10%) or Screen off, between the Night from and Night until times. The screen stays at full brightness while the bins need putting out |
| Night from, Night until | Any hour |
| Reminder | When "Bins out tonight" starts the day before a collection: all day, or from any hour |
| Screen | Normal or Flipped (rotated 180 degrees) |
| Preview | Shows the evening before, and the day of, the next two collections, changing every 5 seconds. Not saved, so a restart turns it off |
| Update | Press CHANGE to download the calendar now |
| Setup | Press CHANGE to start the setup portal (see below) |
| Info | IP address, WiFi signal, last and next update, number of collections saved and the firmware build date |

On the T-Display-S3, MENU is the BOOT button (GPIO0) and CHANGE is GPIO14. On the T-Display they're GPIO0 and GPIO35.

## Setup

The first time it starts, the device runs a setup portal, much like hotel WiFi:

1. The screen shows a network name like `BinDay-B80D` and a password. The password changes each time setup starts.
2. Join that network on your phone. The setup page opens by itself, or you can go to `http://192.168.4.1`.
3. Choose your WiFi network and enter its password. The device joins it while your phone stays on the setup network.
4. Enter your postcode, pick your address from the list (or type in a UPRN), and tap **Save and restart**.

Setup also starts:

- from the **Setup** menu item, for example to change the address or WiFi network. The device stays connected to its current network, so you can go straight to the address
- by holding MENU while the screen shows "Connecting WiFi" at power-on
- by itself if the saved network can't be joined within 2 minutes of power-on. If nobody joins the setup network within 10 minutes, the device restarts and tries the saved network again, so it gets going by itself after a router outage

While setup is running, hold MENU, or tap **Cancel setup** on the page, to restart without changing anything. Cancelling isn't possible during first-time setup, because there's nothing to go back to.

The setup is saved in NVS. To start again from scratch, erase the flash with `idf.py erase-flash` and flash the firmware again.

### Without the portal

For development, `main/secrets.h` can hold your WiFi details and UPRN, which are used whenever nothing has been saved from the portal:

```bash
cp main/secrets.example.h main/secrets.h
```

Then edit `main/secrets.h`. It is git-ignored, so your credentials and UPRN stay out of version control. To look up your UPRN without the portal, replace the postcode with yours and run:

```bash
curl -s -d "App=RH12 1RL&Submit=Search" \
  https://satellite.horsham.gov.uk/environment/refuse/cal2.asp \
  | tr -d '\r\n\t' | grep -oE "<option value='[0-9]+'>[^<]*" \
  | sed -E "s/<option value='([0-9]+)'> */\1  /"
```

Each line is a UPRN followed by its address. Note the number for your address, including any leading zeros.

## How it gets the calendar

The council's [Personalised Bin Calendar](https://satellite.horsham.gov.uk/environment/refuse/cal_details.asp) has no API, but the address picker is two plain HTML forms:

1. `POST cal2.asp` with `App=<postcode>&Submit=Search` returns the addresses for a postcode. Each has a 12-digit UPRN (Unique Property Reference Number).
2. `POST cal_details.asp` with `uprn=<UPRN>` returns the calendar for that property. Opening the page without a UPRN gives a 500 error.

A property's UPRN never changes, so the setup portal does step 1 once, and after that the device only does step 2.

The device tries HTTPS first, but at the moment that always fails and it falls back to plain HTTP. The council's server only offers TLS 1.2 with RSA key exchange, which Mbed TLS 4 (used by ESP-IDF 6) has removed. Over HTTP your UPRN is sent unencrypted, and the reply could in theory be altered on the way, so the worst case is wrong bin days on the screen. If the council updates their server, the device will start using HTTPS without any changes. The page is about 20 KB, and `main/bin_calendar.c` picks out each date and the coloured bullet (`class='bullet green'` and so on) next to each bin.

If the council changes the page layout, the parser may stop finding collections. `test/cal_details_sample.html` shows the layout the parser expects. The [UKBinCollectionData](https://github.com/robbrad/UKBinCollectionData) project's `HorshamDistrictCouncil.py` scraper is a useful place to see what changed.

## Getting started

Requires ESP-IDF v6.1.

1. Set up the ESP-IDF environment (this is the `get_idf` alias):
   ```bash
   . ~/esp/esp-idf/export.sh
   ```
2. Choose your board. This only needs doing once, and again whenever you switch boards:
   ```bash
   idf.py set-target esp32     # T-Display
   idf.py set-target esp32s3   # T-Display-S3
   ```
3. Optionally, change the NTP server or timezone:
   ```bash
   idf.py menuconfig
   ```
   The settings are under **Bin Day Configuration**.
4. Build, flash and watch the log (press Ctrl+] to exit the monitor):
   ```bash
   idf.py flash monitor
   ```
5. Follow the [setup](#setup) steps on the screen.

To keep a separate build for each board, give each its own build folder and config:

```bash
idf.py -B build-s3 -D SDKCONFIG=build-s3/sdkconfig -D IDF_TARGET=esp32s3 flash monitor
```

## Testing the parsers

The parsers for the calendar and the postcode search are plain C, so they can be tested on a PC, using the collections table from the council's calendar page (with the address replaced) and made-up addresses:

```bash
cc -Wall -Imain test/test_bin_calendar.c main/bin_calendar.c -lm -o test_bin_calendar && ./test_bin_calendar
```

## Hardware

The display driver and board definitions come from [tdisplay-flip-clock](../tdisplay-flip-clock). Everything that differs between the two boards is in `main/board.h`.

| | T-Display | T-Display-S3 |
|---|---|---|
| ESP-IDF target | `esp32` | `esp32s3` |
| Screen | 1.14" 240×135, SPI | 1.9" 320×170, 8-bit parallel |

## Licence and credits

This project is released under the [MIT Licence](LICENSE).

It includes material from other projects under their own licences:

- **DNS server** in `components/dns_server`: from ESP-IDF's captive portal example, Copyright (c) 2021-2025 Espressif Systems, public domain (Unlicense or CC0).
- **5×7 text font** in `main/display.c`: from the [Adafruit GFX Library](https://github.com/adafruit/Adafruit-GFX-Library) (`glcdfont.c`), Copyright (c) 2012 Adafruit Industries, BSD licence. The full licence is in [LICENSES/Adafruit-GFX.txt](LICENSES/Adafruit-GFX.txt).

The built firmware also contains ESP-IDF and the libraries that come with it (such as FreeRTOS, lwIP and Mbed TLS), each under its own licence. If you hand out flashed devices, include the notices listed on Espressif's [Copyrights and Licenses](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/COPYRIGHT.html) page.
