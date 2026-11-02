// sleep_player.ino — Night-time sound player for Pico on a Cytron Maker Pi Pico
//
// Plays MP3s from the onboard micro-SD slot through the 3.5 mm jack (PWM audio
// on GP18/GP19) into powered speakers. Each track loops until you change it.
// Sleep timer fades the volume out slowly, then pauses.
//
// Libraries (Library Manager):
//   BackgroundAudio   by Earle F. Philhower
//   U8g2              by oliver
//   Adafruit NeoPixel by Adafruit
// Core: Raspberry Pi Pico/RP2040/RP2350 (Earle Philhower), board "Raspberry Pi Pico"
//
// Maker Pi Pico: set the BUZZER switch to MUTE (buzzer shares GP18 with audio).
//
// OLED (1.3" 128x64, 7-pin SPI) wiring:
//   GND -> GND        VCC -> 3V3
//   SCK/D0 -> GP2     SDA/D1 -> GP3
//   RES -> GP7        DC -> GP6        CS -> GP5
//
// SD card: FAT32. Put .mp3 files in a folder named "sounds" (or the card's
// root). Subfolders are ignored. They play in alphabetical order, so number
// them: "01 Rain.mp3", "02 Brown noise.mp3" ...
// The leading number is hidden on screen.
//
// Buttons:
//   GP20  play / pause (always works, even with the screen off)
//   GP21  short: next sound      long: previous sound
//   GP22  short: sleep timer (off -> 30 -> 60 -> 90 min)   long: volume step
//   With the screen asleep, GP21/GP22 first just wake it.

#include <BackgroundAudio.h>
#include <PWMAudio.h>
#include <SD.h>
#include <SPI.h>
#include <U8g2lib.h>
#include <Adafruit_NeoPixel.h>
#include <vector>
#include <algorithm>

// ---------- Display type ----------
// Most 1.3" 128x64 OLEDs are SH1106. If the picture is shifted by 2 columns
// or garbled, set this to 0 to use the SSD1306 driver instead.
#define OLED_IS_SH1106 1

// ---------- Pins ----------
#define PIN_AUDIO_L   18        // R is GP19 automatically
#define PIN_SD_SCK    10
#define PIN_SD_MOSI   11
#define PIN_SD_MISO   12
#define PIN_SD_CS     15
#define PIN_OLED_SCK  2
#define PIN_OLED_MOSI 3
#define PIN_OLED_MISO 4         // unused by the OLED, but SPI0 needs a valid RX pin
#define PIN_OLED_CS   5
#define PIN_OLED_DC   6
#define PIN_OLED_RES  7
#define PIN_BTN_PLAY  20
#define PIN_BTN_NEXT  21
#define PIN_BTN_TIMER 22
#define PIN_RGB       28

// ---------- Behavior ----------
const float    VOLUME_STEPS[]   = {0.10, 0.20, 0.35, 0.55, 0.80, 1.00};
const int      NUM_VOL          = sizeof(VOLUME_STEPS) / sizeof(VOLUME_STEPS[0]);
int            volIdx           = 3;          // start volume
const int      TIMER_STEPS[]    = {0, 30, 60, 90};   // minutes
const uint32_t FADE_MAX_MS      = 10UL * 60 * 1000;  // fade over last 10 min
const uint32_t SCREEN_ON_MS     = 8000;       // screen auto-off
const uint8_t  LED_MAX          = 25;         // night-light brightness (0-255)
const uint8_t  LED_R = 255, LED_G = 80, LED_B = 8;   // warm amber

// ---------- Objects ----------
PWMAudio audio(PIN_AUDIO_L);
BackgroundAudioMP3Class<RawDataBuffer<16 * 1024>> mp3(audio);

#if OLED_IS_SH1106
U8G2_SH1106_128X64_NONAME_F_4W_HW_SPI oled(U8G2_R0, PIN_OLED_CS, PIN_OLED_DC, PIN_OLED_RES);
#else
U8G2_SSD1306_128X64_NONAME_F_4W_HW_SPI oled(U8G2_R0, PIN_OLED_CS, PIN_OLED_DC, PIN_OLED_RES);
#endif

Adafruit_NeoPixel led(1, PIN_RGB, NEO_GRB + NEO_KHZ800);

// ---------- State ----------
std::vector<String> tracks;
int      trackIdx   = 0;
File     f;
uint32_t dataStart  = 0;          // file offset after the ID3 tag (loop point)
uint8_t  filebuff[512];

bool     paused     = false;
int      timerIdx   = 0;
uint32_t timerStart = 0;
uint32_t timerLenMs = 0;
float    fadeFactor = 1.0;

bool     screenOn   = true;
uint32_t screenOnAt = 0;
uint32_t lastDraw   = 0;
bool     needDraw   = true;
int      lastLedLevel = -1;

// ---------- Buttons ----------
struct Button {
  uint8_t  pin;
  bool     down = false;
  bool     longFired = false;
  uint32_t downAt = 0;
  uint32_t changedAt = 0;
};
Button bPlay{PIN_BTN_PLAY}, bNext{PIN_BTN_NEXT}, bTimer{PIN_BTN_TIMER};

enum { EV_NONE, EV_SHORT, EV_LONG };

int pollButton(Button& b) {
  bool pressed = digitalRead(b.pin) == LOW;
  uint32_t now = millis();
  if (now - b.changedAt < 30) return EV_NONE;          // debounce
  if (pressed && !b.down) {
    b.down = true; b.longFired = false; b.downAt = now; b.changedAt = now;
  } else if (!pressed && b.down) {
    b.down = false; b.changedAt = now;
    if (!b.longFired) return EV_SHORT;
  } else if (pressed && !b.longFired && now - b.downAt > 700) {
    b.longFired = true;
    return EV_LONG;
  }
  return EV_NONE;
}

// ---------- Helpers ----------
String displayName(const String& path) {
  String n = path.substring(path.lastIndexOf('/') + 1);
  int dot = n.lastIndexOf('.');
  if (dot > 0) n = n.substring(0, dot);
  int i = 0;                                           // strip "01 ", "02_", "3-"
  while (i < (int)n.length() && isDigit(n[i])) i++;
  while (i < (int)n.length() && (n[i] == ' ' || n[i] == '_' || n[i] == '-' || n[i] == '.')) i++;
  if (i < (int)n.length()) n = n.substring(i);
  return n;
}

// Returns the offset of the first byte after an ID3v2 tag (0 if none)
uint32_t skipID3(File& file) {
  uint8_t h[10];
  file.seek(0);
  if (file.read(h, 10) == 10 && h[0] == 'I' && h[1] == 'D' && h[2] == '3') {
    uint32_t sz = ((uint32_t)(h[6] & 0x7F) << 21) | ((uint32_t)(h[7] & 0x7F) << 14) |
                  ((uint32_t)(h[8] & 0x7F) << 7)  |  (uint32_t)(h[9] & 0x7F);
    uint32_t start = 10 + sz + ((h[5] & 0x10) ? 10 : 0);
    if (start < file.size()) return start;
  }
  return 0;
}

// Lists .mp3 files in ONE folder (no subfolders), so a card full of other
// data doesn't slow startup.
void scanDirectory(const char* dirname) {
  File root = SD.open(dirname);
  if (!root || !root.isDirectory()) return;
  Serial.printf("Scanning %s\n", dirname);
  while (true) {
    File e = root.openNextFile();
    if (!e) break;
    if (!e.isDirectory()) {
      String name = e.name();
      String lower = name; lower.toLowerCase();
      if (lower.endsWith(".mp3") && name[0] != '.') {     // skip macOS "._" junk
        tracks.push_back(String(dirname) + name);
      }
    }
    e.close();
  }
  root.close();
}

void openTrack(int i) {
  int n = tracks.size();
  if (n == 0) return;
  trackIdx = ((i % n) + n) % n;
  if (f) f.close();
  mp3.flush();
  f = SD.open(tracks[trackIdx]);
  if (!f) {
    Serial.printf("Could not open %s\n", tracks[trackIdx].c_str());
    return;
  }
  dataStart = skipID3(f);
  f.seek(dataStart);
  Serial.printf("Now playing [%d/%d]: %s\n", trackIdx + 1, n, tracks[trackIdx].c_str());
  needDraw = true;
}

// Keep the decoder's input buffer full, looping the file at EOF
void feedAudio() {
  int guard = 0;
  while (f && mp3.availableForWrite() > 512 && guard++ < 64) {
    int len = f.read(filebuff, sizeof(filebuff));
    if (len > 0) mp3.write(filebuff, len);
    if (len < (int)sizeof(filebuff)) {
      if (len <= 0 && f.position() <= dataStart) {      // empty or unreadable file
        Serial.println("File has no audio data, skipping");
        openTrack(trackIdx + 1);
        return;
      }
      f.seek(dataStart);                                 // loop
    }
  }
}

void wakeScreen() {
  if (!screenOn) oled.setPowerSave(0);
  screenOn = true;
  screenOnAt = millis();
  needDraw = true;
}

uint32_t timerRemainingMs() {
  if (timerLenMs == 0) return 0;
  uint32_t el = millis() - timerStart;
  return el >= timerLenMs ? 0 : timerLenMs - el;
}

void updateTimerAndGain() {
  if (timerLenMs) {
    uint32_t rem = timerRemainingMs();
    uint32_t fadeLen = min(FADE_MAX_MS, timerLenMs);
    if (rem == 0) {
      mp3.pause();
      paused = true;
      timerLenMs = 0; timerIdx = 0;
      fadeFactor = 1.0;
      Serial.println("Sleep timer done, paused");
      needDraw = true;
    } else if (rem < fadeLen) {
      fadeFactor = (float)rem / fadeLen;
      fadeFactor *= fadeFactor;                           // ears hear log-ish; square it
    } else {
      fadeFactor = 1.0;
    }
  } else {
    fadeFactor = 1.0;
  }
  mp3.setGain(VOLUME_STEPS[volIdx] * fadeFactor);
}

void updateLed() {
  int level = paused ? 0 : (int)(LED_MAX * fadeFactor + 0.5f);
  if (level == lastLedLevel) return;
  lastLedLevel = level;
  led.setPixelColor(0, led.Color(LED_R * level / 255, LED_G * level / 255, LED_B * level / 255));
  led.show();
}

void drawScreen() {
  oled.clearBuffer();

  oled.setFont(u8g2_font_6x10_tf);
  oled.drawStr(0, 9, paused ? "|| Paused" : "> Playing");
  char buf[24];
  snprintf(buf, sizeof(buf), "%d/%d", trackIdx + 1, (int)tracks.size());
  oled.drawStr(128 - oled.getStrWidth(buf), 9, buf);
  oled.drawHLine(0, 12, 128);

  oled.setFont(u8g2_font_7x13B_tf);
  String name = tracks.empty() ? String("No MP3s found") : displayName(tracks[trackIdx]);
  if (name.length() > 18) name = name.substring(0, 17) + "~";
  oled.drawStr(0, 30, name.c_str());

  oled.setFont(u8g2_font_6x10_tf);
  // Volume bar
  oled.drawStr(0, 46, "Vol");
  for (int i = 0; i < NUM_VOL; i++) {
    int x = 24 + i * 9;
    if (i <= volIdx) oled.drawBox(x, 38, 7, 8);
    else             oled.drawFrame(x, 38, 7, 8);
  }
  // Timer
  if (timerLenMs) {
    uint32_t s = timerRemainingMs() / 1000;
    snprintf(buf, sizeof(buf), "Sleep in %lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
  } else {
    snprintf(buf, sizeof(buf), "Sleep timer off");
  }
  oled.drawStr(0, 62, buf);

  oled.sendBuffer();
}

void fatal(const char* line1, const char* line2) {
  Serial.printf("%s %s\n", line1, line2);
  oled.clearBuffer();
  oled.setFont(u8g2_font_7x13B_tf);
  oled.drawStr(0, 24, line1);
  oled.setFont(u8g2_font_6x10_tf);
  oled.drawStr(0, 44, line2);
  oled.sendBuffer();
  while (true) {
    led.setPixelColor(0, led.Color(40, 0, 0)); led.show(); delay(300);
    led.setPixelColor(0, 0);                   led.show(); delay(300);
  }
}

// ---------- Setup / loop ----------
void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 2000) {}

  pinMode(PIN_BTN_PLAY, INPUT_PULLUP);
  pinMode(PIN_BTN_NEXT, INPUT_PULLUP);
  pinMode(PIN_BTN_TIMER, INPUT_PULLUP);

  led.begin();
  led.clear();
  led.show();

  // OLED on SPI0 — must move SPI0 off its default pins (GP18/19 are the audio jack)
  SPI.setSCK(PIN_OLED_SCK);
  SPI.setTX(PIN_OLED_MOSI);
  SPI.setRX(PIN_OLED_MISO);
  oled.begin();
  oled.setContrast(40);           // dim; it's for a dark room
  oled.clearBuffer();
  oled.setFont(u8g2_font_7x13B_tf);
  oled.drawStr(0, 30, "Sleep Sounds");
  oled.sendBuffer();

  // SD card (Maker Pi Pico's onboard slot). The slot is wired for 4-bit SD
  // mode; in SPI mode its DAT1/DAT2 lines (GP13/GP14) must be pulled up or
  // many cards won't answer.
  pinMode(13, INPUT_PULLUP);
  pinMode(14, INPUT_PULLUP);
  pinMode(PIN_SD_CS, OUTPUT);
  digitalWrite(PIN_SD_CS, HIGH);
  delay(10);

  SPI1.setSCK(PIN_SD_SCK);
  SPI1.setTX(PIN_SD_MOSI);
  SPI1.setRX(PIN_SD_MISO);

  bool sdOk = false;
  const uint32_t speeds[] = {SD_SCK_MHZ(12), SD_SCK_MHZ(4), SD_SCK_MHZ(1)};
  for (uint32_t s : speeds) {
    Serial.printf("SD (SPI mode) at %lu Hz ... ", (unsigned long)s);
    if (SD.begin(PIN_SD_CS, s, SPI1)) { Serial.println("OK"); sdOk = true; break; }
    Serial.println("failed");
    delay(100);
  }
  if (!sdOk) {
    // Fall back to native 4-bit SDIO mode: CLK GP10, CMD GP11, DAT0-3 GP12-15
    SPI1.end();
    Serial.print("SD (SDIO mode) ... ");
    sdOk = SD.begin(PIN_SD_SCK, PIN_SD_MOSI, PIN_SD_MISO);
    Serial.println(sdOk ? "OK" : "failed");
  }
  if (!sdOk) fatal("No SD card", "Reseat card/reformat");

  scanDirectory("/sounds/");                 // preferred: a "sounds" folder
  if (tracks.empty()) scanDirectory("/");    // fallback: MP3s in the card's root
  std::sort(tracks.begin(), tracks.end(),
            [](const String& a, const String& b) { return a.compareTo(b) < 0; });
  Serial.printf("Found %d MP3 file(s)\n", (int)tracks.size());
  for (auto& t : tracks) Serial.printf("  %s\n", t.c_str());
  if (tracks.empty()) fatal("No MP3s found", "Copy .mp3 files to SD");

  mp3.setGain(VOLUME_STEPS[volIdx]);
  mp3.begin();
  openTrack(0);
  wakeScreen();
}

void loop() {
  feedAudio();

  // --- Buttons ---
  int ePlay = pollButton(bPlay);
  int eNext = pollButton(bNext);
  int eTimer = pollButton(bTimer);

  if (ePlay == EV_SHORT) {
    paused = !paused;
    if (paused) mp3.pause(); else mp3.unpause();
    Serial.println(paused ? "Paused" : "Playing");
    wakeScreen();
  }

  if ((eNext || eTimer) && !screenOn) {
    wakeScreen();                       // first press only wakes the screen
  } else {
    if (eNext == EV_SHORT) { openTrack(trackIdx + 1); wakeScreen(); }
    if (eNext == EV_LONG)  { openTrack(trackIdx - 1); wakeScreen(); }
    if (eTimer == EV_SHORT) {
      timerIdx = (timerIdx + 1) % (int)(sizeof(TIMER_STEPS) / sizeof(TIMER_STEPS[0]));
      timerLenMs = (uint32_t)TIMER_STEPS[timerIdx] * 60UL * 1000UL;
      timerStart = millis();
      if (timerLenMs && paused) { paused = false; mp3.unpause(); }
      Serial.printf("Sleep timer: %d min\n", TIMER_STEPS[timerIdx]);
      wakeScreen();
    }
    if (eTimer == EV_LONG) {
      volIdx = (volIdx + 1) % NUM_VOL;
      Serial.printf("Volume step %d/%d\n", volIdx + 1, NUM_VOL);
      wakeScreen();
    }
  }

  // --- Timer, fade, LED ---
  updateTimerAndGain();
  updateLed();

  // --- Screen ---
  uint32_t now = millis();
  if (screenOn && now - screenOnAt > SCREEN_ON_MS) {
    oled.setPowerSave(1);
    screenOn = false;
  }
  if (screenOn && (needDraw || now - lastDraw > 1000)) {
    drawScreen();
    lastDraw = now;
    needDraw = false;
  }
}
