# Pico Sleep Sound Player

A night-time ambient sound player built on a **Raspberry Pi Pico** and a **Cytron Maker Pi Pico**. It plays long MP3s (rain, waterfalls, noise) from the onboard micro-SD slot through the 3.5 mm jack into powered speakers. Each sound loops until you change it, and an optional sleep timer fades the volume out slowly before pausing.

## Features

- MP3 decoding on the Pico itself (no DFPlayer or other audio module)
- Seamless-enough looping: each loop restarts after the ID3 tag, so playback starts right at the audio
- Sleep timer: off / 30 / 60 / 90 minutes, with a 10-minute fade-out
- Dim warm-amber night-light on the onboard RGB LED that fades with the timer
- 1.3" OLED status screen at low contrast that turns itself off after 8 seconds
- Three-button control, safe to use in the dark

## Hardware

| Part | Notes |
| --- | --- |
| Raspberry Pi Pico (or Pico W / Pico 2 / Pico 2 W) | Original Pico works; see the -O3 note below |
| Cytron Maker Pi Pico (Rev 1.2) | Provides the SD slot, audio jack, buttons, and RGB LED |
| 1.3" 128x64 OLED, 7-pin SPI | SH1106 controller (SSD1306 also supported) |
| Powered speakers with a 3.5 mm input | The jack is unamplified line-level PWM audio |
| micro-SD card, FAT32 | Not every card works in SPI mode; see Troubleshooting |

## Pinout

### Onboard (Maker Pi Pico, no wiring needed)

| Function | GPIO | Notes |
| --- | --- | --- |
| Audio left | GP18 | PWM; shared with the piezo buzzer |
| Audio right | GP19 | PWM |
| SD SCK | GP10 | SPI1 |
| SD MOSI | GP11 | SPI1 TX |
| SD MISO | GP12 | SPI1 RX |
| SD CS | GP15 | |
| SD DAT1 / DAT2 | GP13 / GP14 | Pulled up in software (required in SPI mode) |
| Button: play / pause | GP20 | Active low |
| Button: next / previous | GP21 | Active low |
| Button: timer / volume | GP22 | Active low |
| RGB LED (WS2812) | GP28 | |

### OLED (wired by hand)

| OLED pin | Pico | Notes |
| --- | --- | --- |
| GND | GND | |
| VCC | 3V3(OUT) | |
| SCK / D0 | GP2 | SPI0 SCK |
| SDA / D1 | GP3 | SPI0 TX |
| RES | GP7 | |
| DC | GP6 | |
| CS | GP5 | |

GP4 is assigned as SPI0 RX in software but is not connected; the OLED is write-only.

**Set the Maker Pi Pico's buzzer switch to MUTE.** The buzzer shares GP18 with the left audio channel and will play along otherwise.

## Setup

### Arduino IDE

1. Install the **Raspberry Pi Pico/RP2040/RP2350** board package by Earle F. Philhower. Use **4.4.0 or newer**.
2. Install these libraries from the Library Manager:
   - **BackgroundAudio** by Earle F. Philhower
   - **U8g2** by oliver
   - **Adafruit NeoPixel** by Adafruit
3. Select **Tools → Board → Raspberry Pi Pico/RP2040/RP2350 → Raspberry Pi Pico** (or your Pico variant).
4. Select **Tools → Optimize → Optimize Even More (-O3)**.
5. Upload `sleep_player.ino`. For the very first upload, hold BOOTSEL while plugging in.

> If you also have Arduino's **Mbed OS RP2040** core installed, it has its own board named "Raspberry Pi Pico". Picking that one causes `AudioOutputBase.h: No such file or directory`. Select the board through the Philhower submenu, or remove the Mbed core.

### SD card

1. Format the card as **FAT32**.
2. Create a folder named `sounds` in the root.
3. Copy MP3s into it. They play in alphabetical order, so number them:

```
/sounds/01_Rain.mp3
/sounds/02_Woodland_Waterfall.mp3
```

The leading number and separator are hidden on screen. Only the `sounds` folder is scanned, not its subfolders. If `sounds` is missing or empty, MP3s in the card's root are used instead.

**Recommended encoding:** 128 kbps, 44.1 kHz. To convert m4a or other formats with ffmpeg:

```
ffmpeg -i input.m4a -vn -map_metadata -1 -c:a libmp3lame -b:a 128k 01_Rain.mp3
```

## Controls

| Button | Short press | Long press (0.7 s) |
| --- | --- | --- |
| GP20 | Play / pause | |
| GP21 | Next sound | Previous sound |
| GP22 | Sleep timer: off → 30 → 60 → 90 min | Volume step (6 levels) |

When the screen is off, the first press on GP21 or GP22 only wakes it. GP20 always acts immediately, so you can pause in the dark without looking.

## Design decisions

**Decode on the Pico instead of using a DFPlayer Mini.** The project started on a DFPlayer Mini, but its built-in amp gives well under a watt, which isn't enough for room-filling sleep sounds. The Maker Pi Pico already has an SD slot and an audio jack, so decoding on the Pico removed a module, its wiring, and its serial quirks. The line-level output feeds powered speakers, which supply the volume.

**BackgroundAudio for playback.** It decodes MP3 in an interrupt and outputs PWM audio by DMA, so the main loop only has to keep a 16 KB input buffer full from the SD card. That leaves room for buttons, the screen, and the LED without audio dropouts.

**Loop by seeking, not by restarting the decoder.** At end of file, the sketch seeks back to just past the ID3 tag and keeps feeding the same decoder. The decoder never sees metadata on a loop. MP3 encoder padding still leaves a tiny gap at the loop point, so long files (30–90 minutes) are recommended so the loop rarely happens.

**Squared fade curve.** The sleep timer fades over the last 10 minutes (or the whole timer if it's shorter). A linear fade sounds like it falls off a cliff at the end, because loudness perception isn't linear. Squaring the fade factor makes it sound gradual.

**Dark-room UI.** The OLED runs at low contrast and powers down after 8 seconds. The LED tops out at about 10% brightness. While the screen is off, only play/pause acts on the first press, so a fumbled button can't skip tracks or change the timer.

**Software volume, hardware volume knob halfway.** PWM audio has a small noise floor. Running the speakers at moderate volume and controlling loudness in software keeps the hiss lower in a silent room.

**Single-folder scan.** The first version searched the whole card recursively. On a card with other data, startup took minutes and could exhaust RAM on deep folder trees. Scanning one folder makes startup take about a second.

**Explicit SPI pin assignment.** The Pico core's default SPI0 pins include GP18 and GP19, which are the audio jack. The OLED's SPI0 pins are set before the display library starts, and the SD card uses SPI1.

**SD fallbacks.** The Maker's SD slot is wired for 4-bit SD mode. In SPI mode, the sketch pulls up DAT1/DAT2, tries 12, 4, and 1 MHz, and finally falls back to native SDIO mode.

## Troubleshooting

| Symptom | Fix |
| --- | --- |
| `AudioOutputBase.h: No such file or directory` | Wrong core selected (Mbed) or Philhower core older than 4.4.0 |
| "No SD card" | Reseat the card; try a different card. Some cards won't respond in SPI mode at all, even though they work in other devices |
| SD activity light stays on and the screen never updates | Old version scanning the whole card; use the `sounds` folder |
| Audio stutters | Set Optimize to -O3; use 128 kbps MP3s |
| Beeping along with the audio | Buzzer switch not set to MUTE |
| Screen garbled or shifted 2 pixels | Set `OLED_IS_SH1106` to `0` to use the SSD1306 driver |
| Faint hiss in a quiet room | Turn the speakers down and the player's volume up; a PCM5102 I2S DAC is the hardware fix |

Startup progress, the file list, and track changes are printed to the Serial Monitor at 115200 baud.

## Ideas for later

- Enclosure
- PCM5102 I2S DAC for cleaner audio
- Wi-Fi schedule on a Pico W / Pico 2 W
