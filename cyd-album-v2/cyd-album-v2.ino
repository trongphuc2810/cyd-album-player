// ============================================================
// cyd-album-v2 — CYD Album Player (ST7789V) — NO BLE
// Based on malaq88/CYDAlbumPlayer structure; BLE removed,
// audio out via I2S -> MAX98357A (BCLK22 LRC27 DIN17).
// /music/<album>/tracks. Settings: invert, brightness, WiFi.
// Idle clock (black bg, yellow pixel). Touch keyboard for WiFi.
// ============================================================
#include <Arduino.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <SD.h>
#include <SPI.h>
#include <Preferences.h>
#include <WiFi.h>
#include <time.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <AudioFileSourceSD.h>
#include <AudioGeneratorMP3.h>
#include <AudioGeneratorWAV.h>
#include <AudioOutputI2S.h>
#define A2DP_SPP_SUPPORT 0        // free ~20-40KB: no serial-port-profile needed
#include <BluetoothA2DPSink.h>
#include <esp_avrc_api.h>
#include <esp_sntp.h>          // sntp_set_time_sync_notification_cb (real sync check)
// Chimes embedded in flash (~752 KB of PCM). Build with
// -DJINGLE_NO_EMBED for a ~750 KB smaller image: the /sounds/*.wav files on
// the SD card then become the only source of the custom chimes (BT mode,
// which never mounts the card, falls back to the built-in synth chime).
#ifdef JINGLE_NO_EMBED
  #define JINGLE_EMB_RATE 44100
  static const uint32_t JINGLE_EMB_LEN[3] = {0, 0, 0};
  static const int16_t* const JINGLE_EMB_PTR[3] = {nullptr, nullptr, nullptr};
#else
  #include "jingles_pcm.h"
#endif

// Matrix screensaver glyphs (8x8 katakana + digits) - needed in EVERY build,
// so it must sit OUTSIDE the JINGLE_NO_EMBED branch (the lite build skips the
// jingles include entirely).
#include "katakana8x8.h"

// ── Hardware pins ───────────────────────────────────────────
#define SD_CS      5
#define TOUCH_CS   33
#define TOUCH_IRQ  36
#define TFT_BL     21
#define BOOT_BUTTON_PIN 0
#define TOUCH_MOSI 32
#define TOUCH_MISO 39
#define TOUCH_CLK  25

// I2S -> MAX98357A (mono amp)
#define I2S_BCLK   22
#define I2S_LRCK   27
#define I2S_DOUT   17

SPIClass touchSPI(HSPI);
TFT_eSPI tft = TFT_eSPI();
XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);

// ── Preferences (NVS) ──────────────────────────────────────
Preferences prefs;
#define NS_CFG "cyd2"          // display invert / brightness / wifi
#define BT_DEVICE_NAME "CYD-32-BP"
#define JINGLE_GAIN 0.05f      // boot/connect/disconnect chimes: fixed 5% (Phúc, 12/9), own gain

// Small subclass that exposes the protected AVRCP volume-notify call so the
// ESP volume buttons can move the phone's volume too (two-way sync).
class BtSink : public BluetoothA2DPSink {
 public:
  void notifyVolumeToPhone(uint8_t vol) { volume_set_by_local_host(vol); }
  // Pin the lib's per-frame scaling to full (no AVRCP notify to the phone).
  void pinVolumeFull() {
    A2DPVolumeControl *vc = volume_control();
    vc->set_volume(127);
    vc->set_enabled(true);
  }
};
#define DISP_ROTATION 0        // changed 2->0 per user: reverse orientation
static bool cfgInvert = false;
static int  cfgBright = 60;    // 0..100
static char cfgSSID[33] = "";
static char cfgPass[65] = "";

// ── Bluetooth A2DP Sink state (phone -> CYD-32-BP -> MAX98357A) ──
static BtSink btSink;
static bool bootModeBt = false;    // NVS "bootmode"=="bt": clean BT-only boot
static bool btModeActive = false;   // BT status screen visible
static bool btConnected  = false;   // A2DP link up
static bool btUiDirty    = false;   // redraw BT screen from main loop
static bool btDrawnConn   = false;  // what the BT screen currently shows (state
static bool btDrawnSplash = false;  // transition gate: no redraw unless it changed)
static unsigned long btSuccessUntil = 0;   // 5 s SUCCESS splash after a new link
static volatile uint32_t btRateBytes = 0;  // incremented by the raw (pre-volume) callback
static uint32_t btSpeedKbps = 0;           // 1 s window, smoothed, drawn on the BT screen
// Rows pulled down (Phúc 12/9) now that the Clock chip is gone: 168/240 with
// 26 px of bottom margin, so the screen is balanced instead of leaving a hole.
#define BT_ROW_VOL_Y 168       // volume control row
#define BT_ROW_BRI_Y 240       // brightness control row
// Control row geometry: 16 | 40 | 16 | 96 | 16 | 40 | 16 = 240 px. The buttons
// are 40x36 (they were 44x44 and felt oversized), sit 16 px clear of the
// readout, and the caption below keeps 10 px of air so nothing looks stacked.
#define BT_CTRL_L 16           // minus button x0
#define BT_CTRL_LW 40
#define BT_CTRL_BAR 72         // % display x0 (centre of the row = x 120)
#define BT_CTRL_BARW 96
#define BT_CTRL_PLUS 184       // plus button x0
#define BT_CTRL_H 36

// ── Colors (dark DAP theme) ────────────────────────────────
#define COL_BG       TFT_BLACK
#define COL_TEXT     TFT_WHITE
#define COL_DIM      tft.color565(120,120,120)
#define COL_BTN      tft.color565(30,30,30)
#define COL_BTN_ACT  tft.color565(70,70,70)
#define COL_DIR      tft.color565(35,45,70)
#define COL_YEL      tft.color565(255,200,0)

static const int SCR_W = 240;
static const int SCR_H = 320;

// Player screen layout
static const int PL_HEADER_H     = 38;
static const int PL_TITLE_Y      = 40;
static const int PL_CASSETTE_TOP = 52;
static const int PL_PROGRESS_TOP = 164;
static const int PL_PROGRESS_H   = 56;
static const int PL_VOLUME_Y     = 224;
static const int PL_TRANSPORT_Y  = 262;
static const int PL_BACK_BTN_X   = 166;
static const int PL_BACK_BTN_Y   = 4;
static const int PL_BACK_BTN_W   = 70;
static const int PL_BACK_BTN_H   = 30;
static const int PL_INV_BTN_X    = 126;
static const int PL_INV_BTN_Y    = 5;
static const int PL_INV_BTN_W    = 36;
static const int PL_INV_BTN_H    = 20;

static inline uint16_t colReelRed()  { return tft.color565(215,45,50); }
static inline uint16_t colTapeBody() { return tft.color565(42,42,46); }
static inline uint16_t colTapeEdge() { return tft.color565(75,75,80); }
static inline uint16_t colInfoCyan() { return tft.color565(88,190,245); }
static inline uint16_t colTopBarBg() { return tft.color565(10,10,12); }

static unsigned long lastTouchTime = 0;
static const unsigned long TOUCH_DEBOUNCE_MS = 180;

// ── Display backlight / idle clock ─────────────────────────
static const unsigned long DISPLAY_IDLE_OFF_MS = 300000;  // 5 min no touch -> clock
static bool displayBacklightOn = true;
static unsigned long lastUserActivityMs = 0;
static bool screenClockActive = false;   // idle -> big clock
static bool clockForceRedraw = true;
static uint8_t bootBtnPhase = 0;
static unsigned long bootBtnMs = 0;


static void noteUserActivity() {
  lastUserActivityMs = millis();
  if (screenClockActive) { screenClockActive = false; clockForceRedraw = true; }
}

static void pollBootButton();
static void redrawCurrentScreen();
static void enterClockScreen();
static void svDrawLifeFull();      // BT screensavers (defined with the engines below)
static void svDrawMatrixFull();
static void btSwitchToBt();

// ── Backlight PWM ──────────────────────────────────────────
void setBrightnessPct(int pct) {
  cfgBright = constrain(pct, 5, 100);
  if (displayBacklightOn) {
    int duty = (cfgBright * 255) / 100;
    ledcWrite(TFT_BL, duty);
  }
}

void displaySetOn(bool on) {
  displayBacklightOn = on;
  if (on) {
    int duty = (cfgBright * 255) / 100;
    ledcWrite(TFT_BL, duty);
    lastUserActivityMs = millis();
  } else {
    ledcWrite(TFT_BL, 0);
  }
}

// ── Invert display (runtime, NVS-persisted) ────────────────
void applyInvert() {
  tft.invertDisplay(cfgInvert);
}
void toggleInvert() {
  cfgInvert = !cfgInvert;
  applyInvert();
  prefs.begin(NS_CFG, false);
  prefs.putBool("invert", cfgInvert);
  prefs.end();
}

// ── Audio output: I2S + visualizer sample tap ──────────────
#define VIS_BUF_LEN 512
#define VIS_N       256
#define NUM_VIS_BARS 16
static int16_t visRing[VIS_BUF_LEN];
static volatile uint32_t visWritePos = 0;
static float visBandVal[NUM_VIS_BARS];
static float visBandEnv[NUM_VIS_BARS];
static const float VIS_FREQ_HZ[NUM_VIS_BARS] = {
  80.f,125.f,200.f,320.f,500.f,800.f,1250.f,2000.f,
  3000.f,4500.f,6000.f,8000.f,10000.f,12000.f,15000.f,18000.f
};

/** I2S output with software gain + silent fade + spectrum tap. */
class I2SOutTap : public AudioOutputI2S {
public:
  float vol = 0.30f;       // user volume 0..1
  float gainNow = 0.0f;    // actual applied gain (fade target = vol)
  bool fading = false;
  bool mutePending = false;
  bool jingleMode = false;  // true while a boot/connect chime plays (fixed gain)

  bool ConsumeSample(int16_t sample[2]) override {
    // fade toward target (a chime ignores the fade: it has its own gain)
    if (fading && !jingleMode) {
      float target = mutePending ? 0.0f : vol;
      gainNow += (target - gainNow) * 0.25f;
      if (fabsf(target - gainNow) < 0.002f) {
        gainNow = target;
        fading = false;
      }
    }
    // mono mix (MAX98357A is a mono amp; send same mix to both I2S channels)
    float g = jingleMode ? JINGLE_GAIN : gainNow;
    int32_t m = (int32_t)(((int32_t)sample[0] + (int32_t)sample[1]) * g * 0.5f);
    if (m >  32767) m =  32767;
    if (m < -32768) m = -32768;
    int16_t mono = (int16_t)m;
    visRing[visWritePos & (VIS_BUF_LEN - 1)] = mono;
    visWritePos++;
    int16_t s2[2] = { mono, mono };
    return AudioOutputI2S::ConsumeSample(s2);
  }

  void requestFade(bool toMute) { mutePending = toMute; fading = true; }
  void setVolumeFloat(float v)  { vol = constrain(v, 0.0f, 1.0f); if (!fading) gainNow = vol; }
};

static I2SOutTap* audioOut = nullptr;
static AudioFileSourceSD* audioFile = nullptr;
static AudioGeneratorMP3* mp3 = nullptr;
static AudioGeneratorWAV* wav = nullptr;

enum AudioType { AUDIO_NONE, AUDIO_MP3, AUDIO_WAV };
static AudioType currentType = AUDIO_NONE;
enum PlayerState { STATE_STOPPED, STATE_PLAYING, STATE_PAUSED };
static PlayerState playerState = STATE_STOPPED;

/** Keep I2S fed with silence so the amp never pops on stop/pause.
 *  Calls base ConsumeSample directly: bypasses fade logic + visRing. */
static void pumpSilence(int frames) {
  if (!audioOut) return;
  int16_t z[2] = {0,0};
  for (int i = 0; i < frames; i++) {
    if (!audioOut->AudioOutputI2S::ConsumeSample(z)) break;  // DMA full -> paced
  }
}

static void audioPumpDecode(int maxLoops) {
  if (playerState != STATE_PLAYING) return;
  for (int i = 0; i < maxLoops; i++) {
    bool ok = false;
    if (currentType == AUDIO_MP3 && mp3 && mp3->isRunning()) ok = mp3->loop();
    else if (currentType == AUDIO_WAV && wav && wav->isRunning()) ok = wav->loop();
    else break;
    if (!ok) break;
  }
}

// ── Playlist (scan /music/<album>) ─────────────────────────
#define MAX_TRACKS 300
#define MAX_ALBUMS 32
#define MAX_ALBUM_NAME_LEN 48
#define MUSIC_ROOT "/music"

static char* playlist[MAX_TRACKS];
static int trackCount = 0;
static char albums[MAX_ALBUMS][MAX_ALBUM_NAME_LEN];
static int albumCount = 0;
static int albumScroll = 0;
static int browseTrackScroll = 0;
enum BrowseLevel { BROWSE_ALBUMS, BROWSE_TRACKS };
static BrowseLevel browseLevel = BROWSE_ALBUMS;
static int browseTrackIndices[MAX_TRACKS];
static int browseTrackCount = 0;
static char currentAlbumFolder[MAX_ALBUM_NAME_LEN];
static int currentTrack = 0;
static int volumePercent = 30;

static void startTrack(int orderIdx, bool gapless = false);
static void stopTrack(bool flush = true);

static bool isMP3(const char* fn) { int l=strlen(fn); return l>=4 && strcasecmp(fn+l-4,".mp3")==0; }
static bool isWAV(const char* fn) { int l=strlen(fn); return l>=4 && strcasecmp(fn+l-4,".wav")==0; }

static void getDisplayName(const char* path, char* out, int maxLen) {
  const char* name = strrchr(path, '/');
  if (!name) name = path; else name++;
  strncpy(out, name, maxLen - 1);
  out[maxLen - 1] = '\0';
  char* dot = strrchr(out, '.');
  if (dot) *dot = '\0';
}


static void addFile(const char* path) {
  if (trackCount >= MAX_TRACKS) return;
  char* c = strdup(path);
  if (!c) return;
  playlist[trackCount++] = c;
}

static void scanDir(File dir, int depth) {
  if (depth > 2) return;
  while (true) {
    File entry = dir.openNextFile();
    if (!entry) break;
    if (entry.isDirectory()) {
      scanDir(entry, depth + 1);
    } else {
      const char* name = entry.name();
      if (isMP3(name) || isWAV(name)) {
        const char* p = entry.path();
        if (p) addFile(p);
      }
    }
    entry.close();
    audioPumpDecode(8);
  }
}

static void scanSD() {
  File root = SD.open(MUSIC_ROOT);
  if (!root || !root.isDirectory()) return;
  scanDir(root, 0);
  root.close();
}

static void scanAlbums() {
  albumCount = 0;
  for (int i = 0; i < trackCount && albumCount < MAX_ALBUMS; i++) {
    const char* path = playlist[i];
    const char* lastSlash = strrchr(path, '/');
    if (!lastSlash || lastSlash == path) continue;
    int folderLen = (int)(lastSlash - path);
    const char* folderStart = path;
    for (int j = folderLen - 1; j >= 0; j--) {
      if (path[j] == '/') { folderStart = path + j + 1; break; }
    }
    int nameLen = (int)(lastSlash - folderStart);
    if (nameLen <= 0 || nameLen >= MAX_ALBUM_NAME_LEN) continue;
    char candidate[MAX_ALBUM_NAME_LEN];
    strncpy(candidate, folderStart, nameLen);
    candidate[nameLen] = '\0';
    bool exists = false;
    for (int a = 0; a < albumCount; a++)
      if (strcmp(albums[a], candidate) == 0) { exists = true; break; }
    if (!exists) {
      strncpy(albums[albumCount], candidate, MAX_ALBUM_NAME_LEN - 1);
      albums[albumCount][MAX_ALBUM_NAME_LEN - 1] = '\0';
      albumCount++;
    }
  }
  if (albumCount < MAX_ALBUMS - 1) {
    for (int i = albumCount; i > 0; i--) {
      strncpy(albums[i], albums[i-1], MAX_ALBUM_NAME_LEN - 1);
      albums[i][MAX_ALBUM_NAME_LEN - 1] = '\0';
    }
    strncpy(albums[0], "[ All Tracks ]", MAX_ALBUM_NAME_LEN - 1);
    albums[0][MAX_ALBUM_NAME_LEN - 1] = '\0';
    albumCount++;
  }
}

static void sortBrowseTrackIndices() {
  if (browseTrackCount < 2) return;
  for (int i = 0; i < browseTrackCount - 1; i++)
    for (int j = 0; j < browseTrackCount - 1 - i; j++) {
      int a = browseTrackIndices[j], b = browseTrackIndices[j+1];
      if (strcmp(playlist[a], playlist[b]) > 0) {
        int t = browseTrackIndices[j]; browseTrackIndices[j] = browseTrackIndices[j+1]; browseTrackIndices[j+1] = t;
      }
      if ((j & 3) == 0) audioPumpDecode(12);
    }
}

static void loadBrowseAlbumTracks(const char* albumName) {
  browseTrackCount = 0;
  if (strcmp(albumName, "[ All Tracks ]") == 0) {
    for (int i = 0; i < trackCount && browseTrackCount < MAX_TRACKS; i++)
      browseTrackIndices[browseTrackCount++] = i;
    sortBrowseTrackIndices();
    return;
  }
  for (int i = 0; i < trackCount && browseTrackCount < MAX_TRACKS; i++) {
    const char* path = playlist[i];
    const char* lastSlash = strrchr(path, '/');
    if (!lastSlash) continue;
    const char* folderStart = path;
    int folderLen = (int)(lastSlash - path);
    for (int j = folderLen - 1; j >= 0; j--) {
      if (path[j] == '/') { folderStart = path + j + 1; break; }
    }
    int nameLen = (int)(lastSlash - folderStart);
    if (nameLen == (int)strlen(albumName) && strncmp(folderStart, albumName, nameLen) == 0)
      browseTrackIndices[browseTrackCount++] = i;
  }
  sortBrowseTrackIndices();
}

static void setCurrentAlbumFromPath(const char* path) {
  currentAlbumFolder[0] = '\0';
  const char* lastSlash = strrchr(path, '/');
  if (!lastSlash || lastSlash == path) return;
  const char* folderStart = path;
  int folderLen = (int)(lastSlash - path);
  for (int j = folderLen - 1; j >= 0; j--) {
    if (path[j] == '/') { folderStart = path + j + 1; break; }
  }
  int nameLen = (int)(lastSlash - folderStart);
  if (nameLen <= 0 || nameLen >= MAX_ALBUM_NAME_LEN) return;
  memcpy(currentAlbumFolder, folderStart, nameLen);
  currentAlbumFolder[nameLen] = '\0';
}

// WAV duration/rate
static uint32_t wavParseDurationAndRate(const char* path, uint32_t* outRate) {
  if (outRate) *outRate = 0;
  File f = SD.open(path, FILE_READ);
  if (!f) return 0;
  uint8_t riff[12];
  if (f.read(riff, 12) < 12 || memcmp(riff,"RIFF",4)!=0 || memcmp(riff+8,"WAVE",4)!=0) { f.close(); return 0; }
  uint16_t numCh=0, bitsPerSample=0; uint32_t sampleRate=0, dataSize=0; bool haveData=false;
  while (f.available()) {
    uint8_t cid[4], szb[4];
    if (f.read(cid,4)<4 || f.read(szb,4)<4) break;
    uint32_t chunkSize = szb[0]|((uint32_t)szb[1]<<8)|((uint32_t)szb[2]<<16)|((uint32_t)szb[3]<<24);
    if (memcmp(cid,"fmt ",4)==0) {
      if (chunkSize < 16) { f.seek(chunkSize+(chunkSize&1u), SeekCur); continue; }
      uint8_t fmt[16];
      if (f.read(fmt,16)<16) break;
      numCh = (uint16_t)(fmt[2]|(fmt[3]<<8));
      sampleRate = fmt[4]|((uint32_t)fmt[5]<<8)|((uint32_t)fmt[6]<<16)|((uint32_t)fmt[7]<<24);
      bitsPerSample = (uint16_t)(fmt[14]|(fmt[15]<<8));
      uint32_t skip = chunkSize - 16;
      if (skip > 0) f.seek(skip, SeekCur);
      if (chunkSize & 1u) f.seek(1, SeekCur);
    } else if (memcmp(cid,"data",4)==0) { dataSize = chunkSize; haveData = true; break; }
    else { f.seek(chunkSize+(chunkSize&1u), SeekCur); }
  }
  f.close();
  if (outRate) *outRate = sampleRate;
  uint32_t bps = sampleRate * numCh * (bitsPerSample/8u);
  if (bps == 0 || !haveData) return 0;
  return dataSize / bps;
}

static void formatTimeHMS(char* buf, size_t n, uint32_t sec) {
  uint32_t h = sec/3600u, m=(sec%3600u)/60u, s=sec%60u;
  snprintf(buf, n, "%02lu:%02lu:%02lu", (unsigned long)h,(unsigned long)m,(unsigned long)s);
}

// ── MP3 header parser: real bitrate/samplerate + Xing(VBR) frame count ──
// Supports MPEG1/2/2.5 Layer III, CBR + VBR. Returns duration seconds;
// *outKbps = detected bitrate (0 if unknown).
static uint32_t mp3ParseDuration(const char* path, uint32_t* outKbps) {
  if (outKbps) *outKbps = 0;
  File f = SD.open(path, FILE_READ);
  if (!f) return 0;
  uint32_t fileSize = f.size();

  // --- skip ID3v2 tag if present ---
  uint8_t hdr[10];
  uint32_t dataOff = 0;
  if (f.read(hdr, 10) == 10 && hdr[0]=='I' && hdr[1]=='D' && hdr[2]=='3') {
    uint32_t sz = ((uint32_t)(hdr[6]&0x7F)<<21) | ((uint32_t)(hdr[7]&0x7F)<<14)
                | ((uint32_t)(hdr[8]&0x7F)<<7)  | ((uint32_t)(hdr[9]&0x7F));
    dataOff = 10 + sz + ((hdr[5]&0x10) ? 10 : 0);   // +footer if flag
  }

  // --- read first chunk and find frame sync 0xFFE ---
  uint8_t buf[512];
  f.seek(dataOff, SeekSet);
  int got = (int)f.read(buf, 512);
  f.close();
  if (got < 8) return 0;

  int pos = -1;
  for (int i = 0; i < got - 4; i++) {
    if (buf[i]==0xFF && (buf[i+1]&0xE0)==0xE0) { pos = i; break; }
  }
  if (pos < 0) return 0;

  uint8_t b1 = buf[pos+1], b2 = buf[pos+2], b3 = buf[pos+3];
  int ver  = (b1 >> 3) & 0x3;      // 3=MPEG1, 2=MPEG2, 0=MPEG2.5
  int layer= (b1 >> 1) & 0x3;      // 1=Layer III
  int bri  = (b2 >> 4) & 0xF;      // bitrate index
  int sri  = (b2 >> 2) & 0x3;      // samplerate index
  bool pad  = (b2 >> 1) & 0x1;
  int chmode= (b3 >> 6) & 0x3;     // 3=mono
  if (ver==1 || layer!=1 || bri==0 || bri==15) return 0;  // reserved/invalid

  static const uint16_t brV1[16] = {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0};
  static const uint16_t brV2[16] = {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0};
  static const uint16_t srV1[4]  = {44100,48000,32000,0};
  static const uint16_t srV2[4]  = {22050,24000,16000,0};
  static const uint16_t srV25[4] = {11025,12000,8000,0};

  uint32_t bitrate = (ver==3) ? brV1[bri] : brV2[bri];   // kbps
  uint32_t sr = 0;
  if (ver==3)      sr = srV1[sri];
  else if (ver==2) sr = srV2[sri];
  else             sr = srV25[sri];
  if (bitrate==0 || sr==0) return 0;
  if (outKbps) *outKbps = bitrate;
  uint32_t spf = (ver==3) ? 1152 : 576;                  // samples per frame

  // --- Xing/Info (VBR) header right after side info ---
  // side info: MPEG1: 32B stereo / 17B mono ; MPEG2/2.5: 17B / 9B
  uint32_t siOff = pos + 4;
  if (ver==3)      siOff += (chmode==3) ? 17 : 32;
  else             siOff += (chmode==3) ? 9  : 17;
  if ((int)siOff + 16 <= got) {
    bool isXing = (buf[siOff]=='X'&&buf[siOff+1]=='i'&&buf[siOff+2]=='n'&&buf[siOff+3]=='g')
               || (buf[siOff]=='I'&&buf[siOff+1]=='n'&&buf[siOff+2]=='f'&&buf[siOff+3]=='o');
    if (isXing) {
      uint32_t flags = ((uint32_t)buf[siOff+4]<<24)|((uint32_t)buf[siOff+5]<<16)
                     | ((uint32_t)buf[siOff+6]<<8)|((uint32_t)buf[siOff+7]);
      if (flags & 1) {  // frame count present
        uint32_t fr = ((uint32_t)buf[siOff+8]<<24)|((uint32_t)buf[siOff+9]<<16)
                    | ((uint32_t)buf[siOff+10]<<8)|((uint32_t)buf[siOff+11]);
        if (fr > 0) {
          uint64_t dur = (uint64_t)fr * spf * 1000ull / sr;   // ms
          // refine average bitrate for VBR display
          if (outKbps) {
            uint64_t bps = (uint64_t)fileSize * 8000ull / dur;
            *outKbps = (uint32_t)(bps / 1000ull);
          }
          return (uint32_t)(dur / 1000ull);
        }
      }
    }
  }

  // --- CBR fallback: audio bytes * 8 / bitrate ---
  uint32_t audioBytes = (fileSize > dataOff) ? (fileSize - dataOff) : 0;
  if (audioBytes > 128) {
    // trim trailing ID3v1 (128B "TAG") if present
    File tf = SD.open(path, FILE_READ);
    if (tf) {
      tf.seek(fileSize - 128);
      uint8_t tag[3];
      if (tf.read(tag,3)==3 && tag[0]=='T'&&tag[1]=='A'&&tag[2]=='G') audioBytes -= 128;
      tf.close();
    }
  }
  if (audioBytes == 0) return 0;
  uint64_t durMs = (uint64_t)audioBytes * 8000ull / ((uint64_t)bitrate * 1000ull);
  (void)pad;
  return (uint32_t)(durMs / 1000ull);
}

// ── Timeline (wall-clock coherent) ─────────────────────────
static unsigned long trackWallStartMs = 0;
static unsigned long accumulatedPauseMs = 0;
static unsigned long pauseBeganMs = 0;
static uint32_t cachedDurationSec = 0;
static uint32_t cachedWavRateHz = 0;
static uint32_t cachedMp3Kbps = 0;   // real bitrate from MP3 header parser
static unsigned long lastProgressUiMs = 0;

static uint32_t clampElapsedSec(uint32_t el) {
  if (cachedDurationSec > 0 && el > cachedDurationSec) return cachedDurationSec;
  return el;
}
static uint32_t elapsedPlaybackSec() {
  if (playerState == STATE_STOPPED || trackWallStartMs == 0) return 0;
  if (playerState == STATE_PAUSED && pauseBeganMs >= trackWallStartMs)
    return clampElapsedSec((uint32_t)((pauseBeganMs - trackWallStartMs - accumulatedPauseMs)/1000ul));
  if (playerState == STATE_PLAYING)
    return clampElapsedSec((uint32_t)((millis() - trackWallStartMs - accumulatedPauseMs)/1000ul));
  return 0;
}

// ── Audio stop/start/next (pop-safe) ───────────────────────
static void stopTrack(bool flush) {
  // Fade to silence first so the amp doesn't pop
  if (audioOut && (playerState == STATE_PLAYING || playerState == STATE_PAUSED)) {
    audioOut->requestFade(true);
    // pump a few ms of decoded (now-fading) audio
    unsigned long t0 = millis();
    while (millis() - t0 < 12) audioPumpDecode(64);
    audioOut->requestFade(false);
    audioOut->gainNow = 0.0f;
    pumpSilence(64);
  }
  if (mp3 && mp3->isRunning()) mp3->stop();
  if (wav && wav->isRunning()) wav->stop();
  if (mp3) { delete mp3; mp3 = nullptr; }
  if (wav) { delete wav; wav = nullptr; }
  if (audioFile) { delete audioFile; audioFile = nullptr; }
  currentType = AUDIO_NONE;
  playerState = STATE_STOPPED;
  trackWallStartMs = 0; accumulatedPauseMs = 0; pauseBeganMs = 0;
  cachedDurationSec = 0; cachedWavRateHz = 0; cachedMp3Kbps = 0;
  (void)flush;
}

static void startTrack(int orderIdx, bool gapless) {
  if (trackCount <= 0) return;
  int idx = orderIdx;
  if (idx < 0) idx = trackCount - 1;
  if (idx >= trackCount) idx = 0;
  currentTrack = idx;
  stopTrack(!gapless);
  const char* path = playlist[currentTrack];
  setCurrentAlbumFromPath(path);

  audioFile = new AudioFileSourceSD(path);
  if (!audioFile || !audioFile->isOpen()) {
    if (audioFile) { delete audioFile; audioFile = nullptr; }
    return;
  }
  if (isWAV(path)) {
    wav = new AudioGeneratorWAV();
    if (!wav) return;
    wav->begin(audioFile, audioOut);
    currentType = AUDIO_WAV;
  } else {
    mp3 = new AudioGeneratorMP3();
    if (!mp3) return;
    mp3->begin(audioFile, audioOut);
    currentType = AUDIO_MP3;
  }
  playerState = STATE_PLAYING;
  trackWallStartMs = millis();
  accumulatedPauseMs = 0; pauseBeganMs = 0; cachedWavRateHz = 0;
  if (currentType == AUDIO_WAV) {
    cachedDurationSec = wavParseDurationAndRate(path, &cachedWavRateHz);
  } else if (audioFile) {
    // Real MP3 header parse: bitrate + Xing/VBR frames -> exact duration
    // from the first second (no more size/16000 = 128kbps guess).
    cachedMp3Kbps = 0;
    cachedDurationSec = mp3ParseDuration(path, &cachedMp3Kbps);
    if (cachedDurationSec == 0) {
      uint32_t sz = audioFile->getSize();
      cachedDurationSec = (sz > 0) ? (sz / 16000u) : 0;   // fallback only
      if (cachedDurationSec == 0 && sz > 8000u) cachedDurationSec = 1;
    }
  } else cachedDurationSec = 0;
  // fade volume back in
  audioOut->gainNow = 0.0f;
  audioOut->requestFade(false);
  audioOut->fading = true;
  audioPumpDecode(512);
}

static void nextTrack(bool gapless=false) { startTrack(currentTrack + 1, gapless); }
static void prevTrack() { startTrack(currentTrack > 0 ? currentTrack - 1 : trackCount - 1, false); }
static void startPlayingFromPlaylistIndex(int pi) { if (pi>=0 && pi<trackCount) startTrack(pi, false); }

// guard: if several files in a row finish instantly (corrupt/empty), stop auto-advance
static unsigned long lastAutoAdvanceMs = 0;
static int autoAdvanceBurst = 0;
static bool allowAutoAdvance() {
  unsigned long now = millis();
  if (now - lastAutoAdvanceMs > 600) autoAdvanceBurst = 0;  // healthy gap -> reset
  lastAutoAdvanceMs = now;
  autoAdvanceBurst++;
  return autoAdvanceBurst <= 3;
}

static void togglePause() {
  if (playerState == STATE_PLAYING) {
    audioOut->requestFade(true);          // quick silent pause
    audioOut->fading = true;
    unsigned long t0 = millis();
    while (millis()-t0 < 8) audioPumpDecode(64);
    audioOut->requestFade(false);
    audioOut->gainNow = 0.0f;
    playerState = STATE_PAUSED;
    pauseBeganMs = millis();
  } else if (playerState == STATE_PAUSED) {
    if (pauseBeganMs) { accumulatedPauseMs += (millis() - pauseBeganMs); pauseBeganMs = 0; }
    playerState = STATE_PLAYING;
    audioOut->gainNow = 0.0f;
    audioOut->fading = true;   // fade in on resume
    audioOut->requestFade(false);
  } else if (playerState == STATE_STOPPED && trackCount > 0) {
    startTrack(currentTrack, false);       // play current selection from stop
  }
}

static void applyVolumePercent() {
  volumePercent = constrain(volumePercent, 0, 100);
  if (audioOut) audioOut->setVolumeFloat((float)volumePercent / 100.0f);
}

// ── Visualizer ─────────────────────────────────────────────
static float visGoertzelMag(const float* x, int N, float freqHz, float sr) {
  const float PI_F = 3.14159265f;
  if (freqHz <= 0 || freqHz >= sr * 0.48f) return 0;
  float k = 0.5f + ((float)N * freqHz) / sr;
  int ki = (int)k; if (ki<1) ki=1; if (ki>=N) ki=N-1;
  float omega = (2.0f*PI_F*(float)ki)/(float)N;
  float sine=sinf(omega), cosine=cosf(omega), coeff=2.0f*cosine;
  float q0=0,q1=0,q2=0;
  for (int i=0;i<N;i++){ q0=coeff*q1-q2+x[i]; q2=q1; q1=q0; }
  float real=q1-q2*cosine, imag=q2*sine;
  return sqrtf(real*real+imag*imag)/(float)N;
}
static float visSampleRateHz() {
  if (currentType == AUDIO_WAV && cachedWavRateHz > 0) return (float)cachedWavRateHz;
  return 44100.0f;
}
static void computeVisBands() {
  if (visWritePos < (uint32_t)VIS_N) return;
  float wf[VIS_N];
  uint32_t end = visWritePos;
  for (int i=0;i<VIS_N;i++){
    uint32_t idx=(end-VIS_N+i)&(VIS_BUF_LEN-1);
    float s=(float)visRing[idx];
    wf[i]=s*(0.54f-0.46f*cosf(2.0f*3.14159265f*(float)i/(float)(VIS_N-1)));
  }
  float sr=visSampleRateHz();
  float raw[NUM_VIS_BARS];
  for (int b=0;b<NUM_VIS_BARS;b++){
    float fh=VIS_FREQ_HZ[b]; if (fh>=sr*0.45f) fh=sr*0.45f;
    raw[b]=visGoertzelMag(wf,VIS_N,fh,sr);
  }
  for (int b=0;b<NUM_VIS_BARS;b++){
    float treble=0.55f+(float)b*0.32f;
    float scaled=raw[b]*treble;
    visBandEnv[b]=visBandEnv[b]*0.90f+scaled*0.10f;
    if (visBandEnv[b]<1e-7f) visBandEnv[b]=1e-7f;
    float t=scaled/(visBandEnv[b]*1.35f+1e-6f);
    t=powf(t,0.55f);
    if (t>1.0f) t=1.0f;
    if (t>visBandVal[b]) visBandVal[b]=visBandVal[b]*0.28f+t*0.72f;
    else visBandVal[b]=visBandVal[b]*0.72f+t*0.28f;
  }
}

static const int VIS_PX=8, VIS_PY=PL_CASSETTE_TOP, VIS_PW=224, VIS_PH=108;
static const int VIS_IX=VIS_PX+10, VIS_IY=VIS_PY+22, VIS_IW=VIS_PW-20, VIS_IH=VIS_PH-34;

static uint16_t visBarColor(int b, float hNorm) {
  float t=(float)b/(float)((NUM_VIS_BARS>1)?(NUM_VIS_BARS-1):1);
  uint8_t r=(uint8_t)(20.f+t*120.f+hNorm*80.f);
  uint8_t g=(uint8_t)(200.f-t*140.f+hNorm*40.f);
  uint8_t bl=(uint8_t)(160.f+(1.0f-t)*60.f+hNorm*30.f);
  return tft.color565(r,g,bl);
}

// Only bars whose height changed are redrawn (anti-jank refresh)
static void redrawVisualizerBarsOnly() {
  const int gap=2;
  int barW=(VIS_IW-gap*(NUM_VIS_BARS-1)-4)/NUM_VIS_BARS;
  if (barW<2) barW=2;
  const int maxH=VIS_IH-8;
  const int baseY=VIS_IY+VIS_IH-4;
  for (int b=0;b<NUM_VIS_BARS;b++){
    int x=VIS_IX+2+b*(barW+gap);
    float h=visBandVal[b];
    int hh=(int)(h*(float)maxH);
    if (hh>maxH) hh=maxH;
    // clear column (full height) then draw bar -> no ghosting
    tft.fillRect(x, baseY-maxH, barW, maxH, tft.color565(4,4,8));
    if (hh>=1) tft.fillRect(x, baseY-hh, barW, hh, visBarColor(b,h));
  }
}

static void drawVisualizerPanel() {
  tft.fillRoundRect(VIS_PX,VIS_PY,VIS_PW,VIS_PH,11,colTapeEdge());
  tft.fillRoundRect(VIS_PX+4,VIS_PY+4,VIS_PW-8,VIS_PH-8,8,colTapeBody());
  tft.fillRoundRect(VIS_PX+36,VIS_PY+10,VIS_PW-72,12,4,tft.color565(28,28,32));
  tft.drawRoundRect(VIS_PX+36,VIS_PY+10,VIS_PW-72,12,4,tft.color565(50,50,56));
  tft.setTextSize(1);
  tft.setTextColor(colInfoCyan(), tft.color565(28,28,32));
  tft.setCursor(VIS_PX+48,VIS_PY+14);
  tft.print("SPECTRUM");
  redrawVisualizerBarsOnly();
}

static unsigned long lastVisAnimMs = 0;
static void updateVisualizerAnimation() {
  unsigned long now = millis();
  if (now - lastVisAnimMs < 60) return;   // ~16 fps, smooth + cheap
  lastVisAnimMs = now;
  if (playerState != STATE_PLAYING) {
    for (int i=0;i<NUM_VIS_BARS;i++){ visBandVal[i]*=0.80f; visBandEnv[i]*=0.85f; }
  } else {
    computeVisBands();
  }
  redrawVisualizerBarsOnly();
}

// ── Text helper (centered single line, size 1) ─────────────
static void drawTitleCentered(int y, int maxPx, const char* s) {
  char buf[52];
  strncpy(buf, s?s:"", sizeof(buf)-1); buf[sizeof(buf)-1]='\0';
  const int charW=6;
  int maxChars=maxPx/charW-2; if (maxChars<6) maxChars=6;
  int len=(int)strlen(buf);
  if (len>maxChars){ buf[maxChars-2]='\0'; strcat(buf,".."); }
  tft.setTextSize(1);
  int w=(int)strlen(buf)*charW;
  tft.setCursor((SCR_W/2)-w/2, y);
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.print(buf);
}

// ── Screens ────────────────────────────────────────────────
enum ScreenMode { SCREEN_BROWSER, SCREEN_PLAYER, SCREEN_SETTINGS,
                  SCREEN_WIFI_LIST, SCREEN_KEYBOARD, SCREEN_CONNECTING, SCREEN_CLOCK,
                  SCREEN_BT,
                  // BT-mode screensavers (Phúc 12/9): BT has no network, so no
                  // clock - these two alternate every 5 min of idle instead.
                  SCREEN_SAVER_LIFE, SCREEN_SAVER_MATRIX };
static ScreenMode screenMode = SCREEN_BROWSER;
// remember the screen showing before idle clock, to wake back to it
static ScreenMode wakeBackScreen = SCREEN_PLAYER;

static const int browseHeaderH=30, browseListY=55, browseFooterH=36, browseItemH=24;
static const int BROWSE_PLAYER_BTN_X=72, BROWSE_PLAYER_BTN_Y=4, BROWSE_PLAYER_BTN_W=72, BROWSE_PLAYER_BTN_H=22;
static const int BROWSE_PATH_PLAY_BTN_X=100, BROWSE_PATH_PLAY_BTN_Y=36, BROWSE_PATH_PLAY_BTN_W=40, BROWSE_PATH_PLAY_BTN_H=18;
static const int SETT_BTN_X=170, SETT_BTN_Y=4, SETT_BTN_W=62, SETT_BTN_H=22;  // gear in browser header

static inline int footerY(){ return SCR_H-browseFooterH; }
static inline int visibleSlots(){ int vis=(footerY()-browseListY)/browseItemH; return max(1,vis); }

// ---- Browser ----
static void drawBrowser() {
  tft.fillScreen(COL_BG);
  tft.fillRect(0,0,SCR_W,browseHeaderH,COL_BTN);
  tft.setTextColor(COL_TEXT, COL_BTN);
  tft.setTextSize(1);
  tft.setCursor(10, browseHeaderH/2-3);
  if (browseLevel == BROWSE_ALBUMS) tft.print("Albums");
  else tft.print("< List");

  // Settings (gear) button top-right
  tft.fillRoundRect(SETT_BTN_X, SETT_BTN_Y, SETT_BTN_W, SETT_BTN_H, 4, COL_BTN);
  tft.setTextColor(colInfoCyan(), COL_BTN);
  tft.setCursor(SETT_BTN_X+8, SETT_BTN_Y+7);
  tft.print("SETT");

  if (trackCount > 0) {  // Player button
    tft.fillRoundRect(BROWSE_PLAYER_BTN_X,BROWSE_PLAYER_BTN_Y,BROWSE_PLAYER_BTN_W,BROWSE_PLAYER_BTN_H,4,COL_BTN);
    uint16_t acc=colInfoCyan();
    int cx=BROWSE_PLAYER_BTN_X+12, cy=BROWSE_PLAYER_BTN_Y+BROWSE_PLAYER_BTN_H/2;
    tft.fillTriangle(cx-4,cy-5,cx-4,cy+5,cx+6,cy,acc);
    tft.setTextColor(COL_TEXT,COL_BTN);
    tft.setCursor(BROWSE_PLAYER_BTN_X+22, BROWSE_PLAYER_BTN_Y+8);
    tft.print("Player");
  }
  if (trackCount > 0) {  // path-row play
    int bx=BROWSE_PATH_PLAY_BTN_X, by=BROWSE_PATH_PLAY_BTN_Y, bw=BROWSE_PATH_PLAY_BTN_W, bh=BROWSE_PATH_PLAY_BTN_H;
    tft.fillRoundRect(bx,by,bw,bh,4,COL_BTN);
    uint16_t acc=colInfoCyan();
    int cx=bx+bw/2, cy=by+bh/2;
    tft.fillTriangle(cx-5,cy-6,cx-5,cy+6,cx+8,cy,acc);
  }

  int fY=footerY(), vis=visibleSlots();
  tft.fillRoundRect(8,fY+6,58,24,5,COL_BTN);
  tft.fillRoundRect(174,fY+6,58,24,5,COL_BTN);
  tft.setTextColor(COL_TEXT,COL_BTN);
  tft.setCursor(20,fY+15); tft.print("PREV");
  tft.setCursor(186,fY+15); tft.print("NEXT");
  int itemCount=(browseLevel==BROWSE_ALBUMS)?albumCount:browseTrackCount;
  int scroll=(browseLevel==BROWSE_ALBUMS)?albumScroll:browseTrackScroll;
  int totalPages=(itemCount+vis-1)/vis; if (totalPages<1) totalPages=1;
  int page=(scroll/vis)+1; if (page>totalPages) page=totalPages;
  tft.setTextColor(COL_DIM,COL_BG);
  tft.setCursor(104,fY+15);
  tft.printf("%d/%d",page,totalPages);

  for (int i=0;i<vis;i++){
    int idx=scroll+i;
    if (idx>=itemCount) break;
    int y=browseListY+i*browseItemH;
    tft.fillRoundRect(6,y+1,SCR_W-12,browseItemH-3,5,COL_DIR);
    tft.setTextSize(1);
    tft.setCursor(12,y+7);
    if (browseLevel==BROWSE_ALBUMS) {
      tft.setTextColor(COL_TEXT,COL_DIR);
      tft.print(albums[idx]);
    } else {
      int pi=browseTrackIndices[idx];
      char disp[40];
      getDisplayName(playlist[pi],disp,sizeof(disp));
      bool on=(pi==currentTrack && (playerState==STATE_PLAYING||playerState==STATE_PAUSED));
      tft.setTextColor(on?colInfoCyan():COL_TEXT,COL_DIR);
      tft.print(disp);
    }
    if ((i&1)==0) audioPumpDecode(24);
  }
  audioPumpDecode(128);
}

// ---- Player ----
static void drawPlayerProgressArea() {
  const int y0=PL_PROGRESS_TOP;

  // Live duration self-calibration for MP3 (CBR/VBR any bitrate):
  // duration ≈ elapsed * fileBytes / bytesConsumed. Fixes the wrong
  // "size/16000 = 128kbps" estimate (a 3-min 224kbps file showed 5 min).
  if (playerState==STATE_PLAYING && currentType==AUDIO_MP3 && audioFile) {
    uint32_t pos = audioFile->getPos();
    uint32_t sz  = audioFile->getSize();
    if (sz > 0 && pos > 0) {
      uint32_t el = elapsedPlaybackSec();
      if (el >= 5 && pos > sz/50u) {
        uint32_t dyn = (uint32_t)(((uint64_t)el * (uint64_t)sz) / (uint64_t)pos);
        if (dyn > 0 && dyn < 14400u) cachedDurationSec = dyn;  // sane: <4h
      }
    }
  }

  tft.fillRect(0,y0,SCR_W,PL_PROGRESS_H,COL_BG);
  uint32_t el=elapsedPlaybackSec();
  char tEl[16],tTot[16];
  formatTimeHMS(tEl,sizeof(tEl),el);
  if (cachedDurationSec>0) formatTimeHMS(tTot,sizeof(tTot),cachedDurationSec);
  else { strncpy(tTot,"--:--:--",sizeof(tTot)-1); tTot[sizeof(tTot)-1]='\0'; }
  const int bx=10,by=y0+2,bw=SCR_W-20,bh=6;
  tft.drawRoundRect(bx,by,bw,bh+2,2,COL_DIM);
  tft.fillRect(bx+1,by+1,bw-2,bh,tft.color565(22,22,26));
  if (cachedDurationSec>0){
    uint32_t fw=(uint32_t)(((uint64_t)el*(uint64_t)(bw-4))/(uint64_t)cachedDurationSec);
    if (fw>(uint32_t)(bw-4)) fw=(uint32_t)(bw-4);
    if (fw>0) tft.fillRect(bx+2,by+2,(int)fw,bh-2,colReelRed());
  }
  tft.setTextSize(1);
  tft.setTextColor(COL_TEXT,COL_BG);
  if (playerState==STATE_PLAYING) tft.fillTriangle(10,y0+18,10,y0+24,16,y0+21,COL_TEXT);
  else { tft.fillRect(10,y0+17,3,8,COL_TEXT); tft.fillRect(14,y0+17,3,8,COL_TEXT); }
  tft.setCursor(22,y0+16); tft.print(tEl);
  tft.setCursor(SCR_W-70,y0+16); tft.print(tTot);
  tft.setTextColor(COL_DIM,COL_BG);
  tft.setCursor(10,y0+28);
  tft.print(currentAlbumFolder[0]?currentAlbumFolder:"-");
  tft.setTextColor(colInfoCyan(),COL_BG);
  tft.setCursor(10,y0+40);
  if (currentType==AUDIO_WAV && cachedWavRateHz>0) tft.printf("WAV / %lu Hz / PCM",(unsigned long)cachedWavRateHz);
  else if (currentType==AUDIO_MP3) {
    if (cachedMp3Kbps > 0) {
      char lb[24];
      snprintf(lb,sizeof(lb),"MP3 / %lu kbps",(unsigned long)cachedMp3Kbps);
      tft.print(lb);
    } else tft.print("MP3");
  }
  else tft.print("---");
}

static void drawVolumeControls() {
  const int y=PL_VOLUME_Y;
  tft.fillRoundRect(10,y,56,30,6,COL_BTN);
  tft.fillRoundRect(74,y,92,30,6,COL_BTN);
  tft.fillRoundRect(174,y,56,30,6,COL_BTN);
  uint16_t fg=COL_TEXT; const int cy=y+15;
  { int cx=10+56/2; tft.fillRoundRect(cx-10,cy-3,20,6,2,fg); }
  char vbuf[10]; snprintf(vbuf,sizeof(vbuf),"%d%%",volumePercent);
  tft.setTextSize(1); tft.setTextColor(COL_TEXT,COL_BTN);
  int tw=(int)strlen(vbuf)*6;
  tft.setCursor((SCR_W/2)-(tw/2),y+11); tft.print(vbuf);
  { int cx=174+56/2;
    tft.fillRoundRect(cx-2,cy-10,4,20,1,fg);
    tft.fillRoundRect(cx-10,cy-2,20,4,2,fg); }
}

static void drawPlayerListBackIcon() {
  int bx=PL_BACK_BTN_X,by=PL_BACK_BTN_Y,bw=PL_BACK_BTN_W,bh=PL_BACK_BTN_H;
  tft.fillRoundRect(bx,by,bw,bh,4,tft.color565(36,36,42));
  uint16_t fg=colInfoCyan();
  int cy=by+bh/2, lineW=30, rowGap=7, lx0=bx+14;
  for (int row=0;row<3;row++){
    int ly=cy-8+row*rowGap;
    tft.fillRoundRect(lx0,ly+1,4,4,1,fg);
    tft.fillRoundRect(lx0+8,ly+2,lineW,2,1,fg);
  }
}

static void drawPlayer() {
  tft.fillScreen(COL_BG);
  tft.fillRect(0,0,SCR_W,PL_HEADER_H,colTopBarBg());
  tft.drawFastHLine(0,PL_HEADER_H-1,SCR_W,tft.color565(40,40,48));
  uint16_t noteCol=tft.color565(170,80,240);
  tft.fillCircle(9,17,3,noteCol); tft.fillCircle(17,15,3,noteCol);
  tft.fillRect(11,8,3,11,noteCol); tft.fillRect(19,6,3,11,noteCol);
  tft.drawFastHLine(12,6,8,noteCol);
  tft.setTextColor(COL_TEXT,colTopBarBg());
  tft.setTextSize(2);
  tft.setCursor(22,8);
  if (trackCount>0) tft.printf("%d/%d",currentTrack+1,trackCount);
  else tft.print("-/-");
  {
    char abuf[26];
    const char* src=currentAlbumFolder[0]?currentAlbumFolder:"-";
    strncpy(abuf,src,sizeof(abuf)-1); abuf[sizeof(abuf)-1]='\0';
    if ((int)strlen(abuf)>14){ abuf[14]='\0'; strcat(abuf,".."); }
    tft.setTextSize(1);
    tft.setCursor(22,26);
    tft.print(abuf);
  }
  // INV button
  tft.fillRoundRect(PL_INV_BTN_X,PL_INV_BTN_Y,PL_INV_BTN_W,PL_INV_BTN_H,3,
                    cfgInvert?tft.color565(88,190,245):tft.color565(30,30,34));
  tft.setTextSize(1);
  tft.setTextColor(cfgInvert?COL_BG:colInfoCyan(), cfgInvert?tft.color565(88,190,245):tft.color565(30,30,34));
  tft.setCursor(PL_INV_BTN_X+8,PL_INV_BTN_Y+6);
  tft.print("INV");
  drawPlayerListBackIcon();
  audioPumpDecode(80);

  char title[64];
  if (trackCount>0) getDisplayName(playlist[currentTrack],title,sizeof(title));
  else strncpy(title,"No track",sizeof(title)-1);
  title[sizeof(title)-1]='\0';
  drawTitleCentered(PL_TITLE_Y,220,title);

  drawVisualizerPanel();
  audioPumpDecode(120);
  drawPlayerProgressArea();
  drawVolumeControls();
  lastProgressUiMs=millis();
  audioPumpDecode(120);

  const int y=PL_TRANSPORT_Y;
  tft.fillRoundRect(10,y,56,42,7,COL_BTN);
  tft.fillRoundRect(74,y,92,42,7,COL_BTN);
  tft.fillRoundRect(174,y,56,42,7,COL_BTN);
  uint16_t fg=COL_TEXT; int cy=y+42/2;
  { int cxPrev=10+56/2;
    tft.fillRect(cxPrev-18,cy-14,3,28,fg);
    tft.fillTriangle(cxPrev-2,cy,cxPrev+9,cy-11,cxPrev+9,cy+11,fg);
    tft.fillTriangle(cxPrev+8,cy,cxPrev+19,cy-11,cxPrev+19,cy+11,fg); }
  { int cxPlay=74+92/2;
    if (playerState==STATE_PLAYING){
      tft.fillRect(cxPlay-12,cy-15,7,30,fg);
      tft.fillRect(cxPlay+5,cy-15,7,30,fg);
    } else {
      tft.fillTriangle(cxPlay-8,cy-11,cxPlay-8,cy+11,cxPlay+11,cy,fg);
    } }
  { int cxNext=174+56/2;
    tft.fillTriangle(cxNext-21,cy-11,cxNext-21,cy+11,cxNext-10,cy,fg);
    tft.fillTriangle(cxNext-13,cy-11,cxNext-13,cy+11,cxNext-2,cy,fg);
    tft.fillRect(cxNext+7,cy-14,3,28,fg); }
  audioPumpDecode(160);
}

// ── Clock idle screen (black + yellow pixel) ----
// ESP32 has no battery RTC: persist epoch+millis each minute so the clock
// still works after a reboot even before/without NTP sync.
static time_t clockNow() {
  time_t t = time(nullptr);
  if (t >= 1600000000) return t;          // NTP synced
  prefs.begin(NS_CFG, true);              // fallback: saved epoch + uptime
  long ep = prefs.getLong("epoch", 0);
  unsigned long savedMs = prefs.getULong("epms", 0);
  prefs.end();
  // savedMs belongs to the session that persisted it, so the uptime delta is
  // only valid while millis() has NOT wrapped past it (same session). On a fresh
  // boot millis() < savedMs, and the unsigned subtraction used to underflow into
  // a ~49.7-day jump: the clock showed a date ~50 days in the future whenever
  // NTP failed (looks like "no timezone"). Fall back to the saved epoch.
  if (ep >= 1600000000) {
    unsigned long up = millis();
    if (up >= savedMs) {
      time_t est = (time_t)ep + (time_t)((up - savedMs) / 1000ul);
      if (est >= 1600000000) return est;
    }
    return (time_t)ep;            // last known time (device was off an unknown while)
  }
  return 0;
}

static void clockPersist() {
  time_t t = time(nullptr);
  if (t < 1600000000) return;
  prefs.begin(NS_CFG, false);
  prefs.putLong("epoch", (long)t);
  prefs.putULong("epms", millis());
  prefs.end();
}

static void drawClockScreen() {
  tft.fillScreen(COL_BG);
  time_t now = clockNow();
  struct tm tmv;
  bool valid = (now >= 1600000000) && localtime_r(&now, &tmv);
  char big[8];
  if (valid) snprintf(big,sizeof(big),"%02d:%02d",tmv.tm_hour,tmv.tm_min);
  else snprintf(big,sizeof(big),"--:--");
  tft.setTextColor(COL_YEL, COL_BG);
  tft.setTextSize(5);
  int w=(int)strlen(big)*30;
  tft.setCursor((SCR_W-w)/2, 96);
  tft.print(big);
  if (valid) {
    tft.setTextSize(2);
    char secs[4]; snprintf(secs,sizeof(secs),"%02d",tmv.tm_sec);
    tft.setTextColor(COL_DIM,COL_BG);
    tft.setCursor((SCR_W-(int)strlen(secs)*12)/2, 180);
    tft.print(secs);
    tft.setTextSize(1);
    static const char* wd[]={"SUN","MON","TUE","WED","THU","FRI","SAT"};
    char dline[24];
    snprintf(dline,sizeof(dline),"%s %02d-%02d-%04d", wd[tmv.tm_wday], tmv.tm_mday, tmv.tm_mon+1, tmv.tm_year+1900);
    tft.setTextColor(COL_YEL,COL_BG);
    tft.setCursor((SCR_W-(int)strlen(dline)*6)/2, 224);
    tft.print(dline);
  } else {
    tft.setTextSize(1);
    const char* m="no time sync";
    tft.setTextColor(COL_DIM,COL_BG);
    tft.setCursor((SCR_W-(int)strlen(m)*6)/2, 180);
    tft.print(m);
  }
  // which zone the time is in + whether this boot really reached an NTP server
  tft.setTextSize(1);
  {
    bool ok = valid && time(nullptr) >= 1600000000;
    char z[32];
    snprintf(z, sizeof(z), "GMT+7 - NTP %s", ok ? "ok" : "fail");
    tft.setTextColor(ok ? COL_DIM : TFT_RED, COL_BG);
    tft.setCursor((SCR_W-(int)strlen(z)*6)/2, 258);
    tft.print(z);
  }
  const char* hint="tap to wake";
  tft.setTextSize(1);
  tft.setTextColor(COL_DIM,COL_BG);
  tft.setCursor((SCR_W-(int)strlen(hint)*6)/2, 300);
  tft.print(hint);
  clockForceRedraw = false;
}

/** Repaint only the small seconds field each second (no full-screen flicker). */
static void updateClockSeconds() {
  time_t now = clockNow();
  struct tm tmv;
  if ((now < 1600000000) || !localtime_r(&now, &tmv)) return;
  tft.setTextSize(2);
  char secs[4]; snprintf(secs,sizeof(secs),"%02d",tmv.tm_sec);
  tft.setTextColor(COL_DIM,COL_BG);
  tft.setCursor((SCR_W-(int)strlen(secs)*12)/2, 180);
  tft.fillRect((SCR_W-24)/2, 180, 24, 16, COL_BG); // clear old 2-digit seconds
  tft.print(secs);
}

static void enterClockScreen() {
  if (screenMode != SCREEN_CLOCK) wakeBackScreen = screenMode;
  screenMode = SCREEN_CLOCK;
  screenClockActive = true;
  clockForceRedraw = true;
  drawClockScreen();
}

// ---- Settings ----
// Settings layout (variant A: grouped cards, every control on one baseline).
// SET_ROW_Y is shared by drawSettings() and handleSettingsTouch() so the hit
// zones can never drift away from the drawn cards.
static const int SET_ROW_X=8, SET_CARD_W=224, SET_CARD_H=34;
// 5 rows since the WiFi row was removed (Phúc 12/9, variant A: pulled up with
// 50 px of breathing room at the bottom). Rows are NAMED - draw() and the touch
// handler both index through these, so a re-layout cannot drift one from the
// other (that class of bug already shipped once).
enum SetRow { SET_INVERT = 0, SET_BRIGHT, SET_BTMODE, SET_RECAL, SET_CLOCK, SET_NROWS };
static const int SET_ROW_Y[SET_NROWS] = {48, 86, 142, 198, 236};  // card tops
static const int SET_GRP_Y[3] = {37, 131, 187};                   // DISPLAY / MODE / SYSTEM
static const int SET_PILL_X=170, SET_PILL_W=50, SET_PILL_H=24;   // ON/OFF pill
static const int SET_STEP_L=145, SET_STEP_R=201, SET_STEP_BTN=22;  // - / value / + (in-card chips)
static inline uint16_t setEdge()  { return tft.color565(38,38,44); }
static inline uint16_t setCtrlBg(){ return tft.color565(45,45,52); }
static void drawSettings() {
  tft.fillScreen(COL_BG);
  tft.fillRect(0,0,SCR_W,30,colTopBarBg());
  tft.drawFastHLine(0,29,SCR_W,tft.color565(40,40,48));
  tft.setTextSize(1);
  tft.setTextColor(colInfoCyan(),colTopBarBg());
  tft.setCursor(10,12); tft.print("Settings");
  tft.setCursor(176,12); tft.print("< back");

  // group labels (WiFi row gone -> NETWORK renamed MODE, variant A spacing)
  tft.setTextColor(colInfoCyan(),COL_BG);
  tft.setCursor(14,SET_GRP_Y[0]); tft.print("DISPLAY");
  tft.setCursor(14,SET_GRP_Y[1]); tft.print("MODE");
  tft.setCursor(14,SET_GRP_Y[2]); tft.print("SYSTEM");

  for (int i=0;i<SET_NROWS;i++) {
    int y=SET_ROW_Y[i];
    tft.fillRoundRect(SET_ROW_X,y,SET_CARD_W,SET_CARD_H,8,COL_BTN);
    tft.drawRoundRect(SET_ROW_X,y,SET_CARD_W,SET_CARD_H,8,setEdge());
  }
  tft.setTextColor(COL_TEXT,COL_BTN);
  tft.setCursor(18,SET_ROW_Y[SET_INVERT]+13); tft.print("Display invert");
  tft.setCursor(18,SET_ROW_Y[SET_BRIGHT]+13); tft.print("Brightness");
  tft.setCursor(18,SET_ROW_Y[SET_BTMODE]+13); tft.print("Bluetooth mode");
  tft.setCursor(18,SET_ROW_Y[SET_RECAL]+13);  tft.print("Recalibrate touch");
  tft.setCursor(18,SET_ROW_Y[SET_CLOCK]+13);  tft.print("Clock");

  // invert toggle
  { int y=SET_ROW_Y[SET_INVERT];
    uint16_t col=cfgInvert?tft.color565(88,190,245):tft.color565(70,70,70);
    tft.fillRoundRect(SET_PILL_X,y+5,SET_PILL_W,SET_PILL_H,12,col);
    tft.setTextColor(cfgInvert?COL_BG:tft.color565(200,200,205),col);
    tft.setCursor(SET_PILL_X+((SET_PILL_W-6*(cfgInvert?2:3))/2),y+14);
    tft.print(cfgInvert?"ON":"OFF");
  }
  // brightness stepper - two 22x22 chips INSIDE the card (one frame only)
  { int y=SET_ROW_Y[SET_BRIGHT];
    tft.fillRoundRect(SET_STEP_L,y+6,SET_STEP_BTN,SET_STEP_BTN,5,setCtrlBg());
    tft.setTextColor(COL_TEXT,setCtrlBg()); tft.setCursor(SET_STEP_L+8,y+13); tft.print("-");
    char b[8]; snprintf(b,sizeof(b),"%d%%",cfgBright);
    tft.setTextColor(COL_TEXT,COL_BTN);
    tft.setCursor(184-3*(int)strlen(b),y+14); tft.print(b);
    tft.fillRoundRect(SET_STEP_R,y+6,SET_STEP_BTN,SET_STEP_BTN,5,setCtrlBg());
    tft.setTextColor(COL_TEXT,setCtrlBg()); tft.setCursor(SET_STEP_R+8,y+13); tft.print("+");
  }
  // bluetooth mode (state only, value right aligned)
  { const char* v = btConnected?"connected":"SD mode";
    tft.setTextColor(btConnected?tft.color565(60,200,90):COL_DIM,COL_BTN);
    tft.setCursor(214-6*(int)strlen(v),SET_ROW_Y[SET_BTMODE]+13); tft.print(v);
  }
  // action rows, value right aligned
  tft.setTextColor(COL_DIM,COL_BTN);
  tft.setCursor(214-60,SET_ROW_Y[SET_RECAL]+13); tft.print("run 4 taps");
  tft.setCursor(214-48,SET_ROW_Y[SET_CLOCK]+13); tft.print("view now");
}

// ---- WiFi list ----
#define WIFI_MAX 14
// Shared by drawWifiList() and handleWifiListTouch() so the hit zones can never
// drift from the drawn rows (24 px pitch like the album list -> 10 rows fit).
static const int WL_Y0=42, WL_ITEMH=24, WL_VIS=10;
static char wifiNames[WIFI_MAX][33];
static int wifiRssi[WIFI_MAX];
static bool wifiLocked[WIFI_MAX];
static int wifiCount=0, wifiScroll=0;
static bool wifiScanDone=false;
static int wifiScanRetries=0;
static unsigned long wifiScanStartedAt=0;
static int kbTargetSSIDIdx = -1;
// Boot-phase WiFi setup (Phúc 12/9): when no network is saved the SAME list +
// keyboard screens run from setup(), before the mode starts. bootWifiPhase
// turns the list's header from "< back" into "< skip" (at boot there is nothing
// to go back to, and the user must still be able to boot without a network).
static bool bootWifiPhase = false;
static bool bootWifiSkip = false;

static void startWifiScan() {
  wifiScanDone=false;
  wifiScanRetries=0;
  wifiScanStartedAt=millis();
  // Ensure radio is in STA mode and idle before scanning
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);                 // keep radio fully on for reliable scan
  if (WiFi.status() != WL_IDLE_STATUS && WiFi.status() != WL_DISCONNECTED) {
    WiFi.disconnect();
    delay(500);
  } else {
    delay(500);   // give the radio time to init after mode change
  }
  WiFi.scanDelete();
  int n = WiFi.scanNetworks(true); // async
  (void)n;
}

// forward decl (defined below with the wifi-connect block)
// Vietnam: UTC+7, no DST. POSIX TZ string - the sign is INVERTED on purpose
// (POSIX counts west-positive), so zone "+07" carries offset -7.
#define TZ_INFO "<+07>-7"

static bool wifiConnecting=false;

static void updateWifiScanResult() {
  if (wifiScanDone) return;
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) {
    // async scan can hang if radio busy; give it up to 12s then restart
    if (millis()-wifiScanStartedAt > 12000) {
      WiFi.scanDelete();
      if (wifiScanRetries < 3) {
        wifiScanRetries++;
        delay(300);
        WiFi.scanNetworks(true);
        wifiScanStartedAt=millis();
      } else {
        wifiScanDone=true; wifiCount=0;
        if (screenMode == SCREEN_WIFI_LIST && wifiConnecting==false) drawWifiList();
      }
    }
    return;
  }
  if (n == WIFI_SCAN_FAILED) {
    // radio may not be ready on first try — retry a few times
    WiFi.scanDelete();
    if (wifiScanRetries < 3) {
      wifiScanRetries++;
      delay(300);
      WiFi.scanNetworks(true);
      wifiScanStartedAt=millis();
    } else {
      wifiScanDone=true;
      wifiCount=0;
      if (screenMode == SCREEN_WIFI_LIST && wifiConnecting==false) drawWifiList();
    }
    return;
  }
  wifiCount = min(n, WIFI_MAX);
  for (int i=0;i<wifiCount;i++){
    strncpy(wifiNames[i], WiFi.SSID(i).c_str(), 32); wifiNames[i][32]='\0';
    if (!wifiNames[i][0]) strncpy(wifiNames[i],"(hidden)",32);
    wifiRssi[i]=WiFi.RSSI(i);
    wifiLocked[i]=(WiFi.encryptionType(i)!=WIFI_AUTH_OPEN);
  }
  WiFi.scanDelete();
  wifiScanDone=true;
  if (screenMode == SCREEN_WIFI_LIST && wifiConnecting==false) drawWifiList();
}

static void drawWifiList() {
  tft.fillScreen(COL_BG);
  tft.fillRect(0,0,SCR_W,30,COL_BTN);
  tft.setTextSize(2); tft.setTextColor(COL_TEXT,COL_BTN);
  tft.setCursor(10,6); tft.print("WiFi");
  tft.setTextSize(1); tft.setTextColor(colInfoCyan(),COL_BTN);
  // boot phase: nothing to go back to -> the same corner means "skip"
  tft.setCursor(176,10); tft.print(bootWifiPhase ? "< skip" : "< back");
  if (!wifiScanDone) {
    tft.setTextColor(COL_DIM,COL_BG);
    const char* m="Scanning...";
    tft.setCursor((SCR_W-6*(int)strlen(m))/2,140); tft.print(m);
    return;
  }
  if (wifiCount==0) {
    tft.setTextColor(COL_DIM,COL_BG);
    const char* m="No networks found";
    tft.setCursor((SCR_W-6*(int)strlen(m))/2,140); tft.print(m);
    tft.setTextColor(colInfoCyan(),COL_BG);
    tft.setCursor(10,300); tft.print("Rescan");
    tft.setTextColor(COL_DIM,COL_BG);
    tft.setCursor(104,300); tft.print("check router");
    return;
  }
  int vis=WL_VIS, y0=WL_Y0, itemH=WL_ITEMH;
  for (int i=0;i<vis;i++){
    int idx=wifiScroll+i;
    if (idx>=wifiCount) break;
    int y=y0+i*itemH;
    // same row geometry as the album browser: 24 px pitch, 21 px pill, text +7
    tft.fillRoundRect(6,y+1,228,itemH-3,5,COL_DIR);
    tft.setTextSize(1);
    char line[40];
    snprintf(line,sizeof(line),"%.24s",wifiNames[idx]);
    tft.setTextColor(COL_TEXT,COL_DIR);
    tft.setCursor(12,y+7); tft.print(line);
    // fixed-width signal meter (4 bars, 188..203) so it can never run into the lock
    int lvl = wifiRssi[idx] > -55 ? 4 : wifiRssi[idx] > -65 ? 3 : wifiRssi[idx] > -75 ? 2 : 1;
    for (int b=0;b<4;b++){
      int bh=2+b*2;
      uint16_t bc = (b<lvl) ? (lvl>=3?colInfoCyan():COL_DIM) : tft.color565(30,30,34);
      tft.fillRect(188+b*4, y+17-bh, 3, bh, bc);
    }
    if (wifiLocked[idx]) {
      tft.setTextColor(colInfoCyan(),COL_DIR);
      tft.setCursor(212,y+7); tft.print("*");
    }
  }
  int totalPages=max(1,(wifiCount+vis-1)/vis);
  tft.setTextColor(COL_DIM,COL_BG);
  tft.setCursor(104,300); tft.printf("%d/%d",(wifiScroll/vis)+1,totalPages);
  tft.setTextColor(colInfoCyan(),COL_BG);
  tft.setCursor(10,300); tft.print("Rescan");
  tft.setCursor(186,300); tft.print("More");
}

// ---- Keyboard (WiFi password) ----
static const char* KB_ROWS[] = {
  "qwertyuiop",
  "asdfghjkl",
  "zxcvbnm,.",
  "1234567890",
  "-_@#!.$"
};
static const int KB_NROW=5, KB_KEY_H=38, KB_Y0=86;
static bool kbShift=false;
static bool kbShowPass=false;   // reveal the typed password (one field, never two lines)
static char kbPass[65]="";
static int kbCur=0;
static char kbSSIDName[33]="";

static char kbCharAt(int row, int col) {
  const char* r=KB_ROWS[row];
  int len=(int)strlen(r);
  if (col>=len) return ' ';
  char c=r[col];
  if (row<3 && kbShift && c>='a' && c<='z') return c-'a'+'A';
  return c;
}

static void drawKeyboard() {
  tft.fillScreen(COL_BG);
  // header: title + back
  tft.fillRect(0,0,SCR_W,30,colTopBarBg());
  tft.drawFastHLine(0,29,SCR_W,tft.color565(40,40,48));
  tft.setTextSize(1);
  tft.setTextColor(colInfoCyan(),colTopBarBg());
  tft.setCursor(10,12); tft.print("WiFi password");
  tft.setCursor(186,12); tft.print("< back");
  // network + label on one line, then a single framed password field
  tft.setTextColor(colInfoCyan(),COL_BG);
  tft.setCursor(10,34); tft.print(kbSSIDName[0]?kbSSIDName:"(hidden)");
  tft.setTextColor(COL_DIM,COL_BG);
  tft.setCursor(146,34); tft.print("Password:");
  uint16_t fldBg = tft.color565(22,22,26);
  tft.fillRoundRect(8,46,224,28,6,fldBg);
  tft.drawRoundRect(8,46,224,28,6,tft.color565(70,70,78));
  char shown[80];
  if (kbShowPass) {
    strncpy(shown,kbPass,sizeof(shown)-1); shown[sizeof(shown)-1]='\0';
  } else {
    int n=kbCur; if (n>33) n=33;
    for (int i=0;i<n;i++) shown[i]='*';
    shown[n]='\0';
  }
  tft.setTextColor(COL_TEXT,fldBg);
  tft.setCursor(14,56); tft.print(shown);
  // the chip sits INSIDE the field frame with a real margin (was a 46x26 box
  // breaking the frame's right edge)
  uint16_t chipBg = tft.color565(45,45,52);
  tft.fillRoundRect(186,51,38,18,4,chipBg);
  tft.setTextColor(colInfoCyan(),chipBg);
  { const char* cs = kbShowPass?"Hide":"Show";
    tft.setCursor(186+(38-6*(int)strlen(cs))/2,56); tft.print(cs); }
  // keys
  int colW=24;
  for (int row=0;row<KB_NROW;row++){
    const char* r=KB_ROWS[row];
    int len=(int)strlen(r);
    int rowW=len*colW;
    int x0=(SCR_W-rowW)/2;
    int y=KB_Y0+row*KB_KEY_H;
    for (int c=0;c<len;c++){
      char ch=kbCharAt(row,c);
      int x=x0+c*colW;
      bool special=(row==4);
      tft.fillRoundRect(x+1,y+1,colW-2,KB_KEY_H-6,4,
        special?tft.color565(40,40,48):tft.color565(45,45,52));
      tft.setTextSize(1);
      tft.setTextColor(special?tft.color565(180,180,180):COL_TEXT,
                       special?tft.color565(40,40,48):tft.color565(45,45,52));
      char sb[2]={ch,'\0'};
      tft.setCursor(x+(colW-6)/2,y+KB_KEY_H/2-9);
      tft.print(sb);
    }
  }
  // bottom controls
  int by=KB_Y0+KB_NROW*KB_KEY_H+2;
  tft.fillRoundRect(8,by,58,30,6,COL_BTN);
  tft.setTextColor(COL_TEXT,COL_BTN); tft.setCursor(16,by+10); tft.print(kbShift?"ABC":"abc");
  tft.fillRoundRect(70,by,34,30,6,COL_BTN);
  tft.setTextColor(COL_TEXT,COL_BTN); tft.setCursor(76,by+10); tft.print("del");
  tft.fillRoundRect(108,by,34,30,6,COL_BTN);
  tft.setTextColor(COL_TEXT,COL_BTN); tft.setCursor(112,by+10); tft.print("clr");
  tft.fillRoundRect(146,by,86,30,6,tft.color565(88,190,245));
  tft.setTextColor(COL_BG,tft.color565(88,190,245)); tft.setCursor(158,by+10); tft.print("Connect");
}

static void kbAddChar(char c) {
  if (c==' ' ) return;                       // space unused in this row layout
  if (c=='\b') { if (kbCur>0) kbCur--; kbPass[kbCur]='\0'; return; }
  if (kbCur<63) { kbPass[kbCur]=c; kbCur++; kbPass[kbCur]='\0'; }
}

// ── WiFi connect flow ──────────────────────────────────────
static unsigned long wifiConnStart=0;


static void saveWifiCreds(const char* ssid, const char* pass) {
  strncpy(cfgSSID,ssid,32); cfgSSID[32]='\0';
  strncpy(cfgPass,pass,64); cfgPass[64]='\0';
  prefs.begin(NS_CFG,false);
  prefs.putString("ssid",cfgSSID);
  prefs.putString("pass",cfgPass);
  prefs.end();
}


static volatile bool sntpSynced = false;
static void sntpNotifyCb(struct timeval*) { sntpSynced = true; }

static void startNtp() {
  sntpSynced = false;
  sntp_set_time_sync_notification_cb(sntpNotifyCb);
  // NEVER use configTime(offset, 0, ...) here. The core's configTime() calls
  // setTimeZone(-offset, daylight), which appends a DST rule whenever
  // daylightOffset != 3600 - our old call produced the malformed TZ string
  // "UTC-7DST-7", tzset() failed and localtime() silently stayed on UTC, so the
  // clock read 7 h behind (Phúc 12/9: "kết nối được wifi nhưng không lấy time
  // zone"). configTzTime() sets a real POSIX TZ string instead.
  configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
}

// Boot rule: WiFi is only used to fetch the time. Once NTP has answered
// (or a timeout passes) the radio is powered fully off again.
/** Sync the clock, then release the radio. Returns true only when a real SNTP
 *  response arrived - the old check compared time() against a fixed threshold,
 *  which a STALE persisted epoch also passes. Retries once: NTP/DNS hiccups
 *  right after association are common. */
static bool ntpSyncThenRadioOff(unsigned long maxWaitMs) {
  bool ok = false;
  for (int attempt = 1; attempt <= 2 && !ok; attempt++) {
    startNtp();
    unsigned long t0 = millis();
    while (!sntpSynced && millis() - t0 < maxWaitMs) delay(40);
    ok = sntpSynced;
    Serial.printf("[NTP] attempt %d -> %s (epoch=%ld)\n", attempt, ok ? "ok" : "fail",
                  (long)time(nullptr));
  }
  if (ok) { delay(120); clockPersist(); }   // only persist a verified time
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  wifiConnecting = false;
  wifiScanDone = true;
  return ok;
}

static void beginWifiConnect(const char* ssid, const char* pass) {
  saveWifiCreds(ssid, pass);
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfgSSID, cfgPass);
  wifiConnecting=true;
  wifiConnStart=millis();
}

static void updateWifiConnecting() {
  if (!wifiConnecting) return;
  if (WiFi.status()==WL_CONNECTED) {
    // sync time (up to 6s), then power the radio off automatically
    ntpSyncThenRadioOff(6000);
    if (playerState != STATE_STOPPED) { screenMode=SCREEN_PLAYER; drawPlayer(); }
    else { screenMode=SCREEN_BROWSER; wakeBackScreen=SCREEN_BROWSER; drawBrowser(); }
    return;
  }
  if (millis()-wifiConnStart > 12000) {
    wifiConnecting=false;
    WiFi.disconnect();
    screenMode = SCREEN_WIFI_LIST;
    startWifiScan();
    drawWifiList();
    return;
  }
}

// ── WiFi radio power (user toggle: off saves power/interference after NTP) ──

// ═══════════════════════════════════════════════════════════
// Bluetooth A2DP Sink (clean integration on the vol5 base)
//  * SD mode is the default; BT only when opened from Settings.
//  * The BT stack is started ONCE and never ended/restarted at
//    runtime (restarting an A2DP sink crashes on no-PSRAM CYD).
//  * The BT callback never touches I2S: PCM goes into an SPSC
//    ring and the main loop drains it into the shared I2S out.
//  * Volume is ALWAYS our own gain (same % as SD): phone volume
//    events map onto volumePercent and the lib's own scaling is
//    pinned to max (set_volume(127)) so it never double-scales.
// ═══════════════════════════════════════════════════════════
#define BT_RING_FRAMES 8192          // ~186 ms at 44.1 kHz - anti-drop
static int16_t btRing[BT_RING_FRAMES * 2];
static volatile uint32_t btRingW = 0, btRingR = 0;
static volatile uint32_t btDropped = 0;

static int16_t btChunk[1024 * 2];    // audio task transfers in big chunks

// ═══════════════════════════════════════════════════════════
// Jingles — boot / phone-connect / phone-disconnect chimes
//  * Source: /sounds/<name>.wav on the SD card when present,
//    otherwise a built-in procedural chime (works with no files).
//  * Played at a FIXED 20% gain, independent of the saved volume
//    (SD mode boots at 10% and the chime must still be audible).
//  * SD mode: blocking playback from setup().
//  * BT mode: queued (jingleReq) and played by the core-0 audio
//    task — no I2S call ever runs inside a Bluedroid callback
//    (that trips the task watchdog on this no-PSRAM board).
// ═══════════════════════════════════════════════════════════
#define JINGLE_RATE   44100
#define JINGLE_MS_MAX 6000        // never hold the phone stream off longer

enum { JINGLE_BOOT = 0, JINGLE_CONN = 1, JINGLE_DISC = 2 };
static const char* const JINGLE_WAV[3] = {
  "/sounds/boot.wav", "/sounds/connect.wav", "/sounds/disconnect.wav"
};

// Built-in fallback chimes: {freq Hz, start ms, dur ms, amp %}
struct JNote { uint16_t f, startMs, durMs, amp; };
static const JNote J_BOOT[] = {{523,0,1300,50},{659,160,1300,45},{784,320,1400,42},{1047,480,900,12}};
static const JNote J_CONN[] = {{880,0,600,55},{1175,130,700,50}};
static const JNote J_DISC[] = {{659,0,600,45},{440,150,800,40}};
static const JNote* const J_NOTES[3] = {J_BOOT, J_CONN, J_DISC};
static const uint8_t  J_NCOUNT[3]    = {4, 2, 2};
static const uint16_t J_TOTAL_MS[3]  = {1700, 900, 1000};

static int16_t jSin[1024];
static void jingleInit() {
  for (int i = 0; i < 1024; i++) jSin[i] = (int16_t)(sinf(2.0f * PI * i / 1024.0f) * 32767.0f);
}
static inline float jSine(float turns) {   // turns = phase in whole cycles
  return jSin[((uint32_t)(turns * 1024.0f)) & 1023] * (1.0f / 32767.0f);
}

// ── SD wav streaming (PCM 8/16-bit, mono or stereo, any rate) ──
static File     jWav;
static bool     jWavIsOpen = false;
static uint8_t  jBuf[512];
static uint16_t jBufPos = 0, jBufLen = 0;
static uint32_t jSrcLeft = 0;        // source frames not yet consumed
static uint8_t  jFrameBytes = 2;
static bool     jSrc8 = false, jSrcStereo = false;
static float    jStep = 1.0f;        // source frames per output frame

// State the source puller needs (declared here: jPullSrc is defined first).
static int      jCur = 0;
static bool     jEmbMode = false;      // playing the flash copy instead of an SD file
static uint32_t jEmbPos = 0;

static bool jPullSrc(float* out) {
  if (jSrcLeft == 0) return false;
  if (jEmbMode) {                       // embedded PCM: mono 16-bit @ 44.1 kHz
    jSrcLeft--;
    *out = (int16_t)pgm_read_word(&JINGLE_EMB_PTR[jCur][jEmbPos++]) * (1.0f / 32768.0f);
    return true;
  }
  if ((uint32_t)jBufPos + jFrameBytes > jBufLen) {
    uint16_t want = sizeof(jBuf) - (sizeof(jBuf) % jFrameBytes);
    int n = jWav.read(jBuf, want);
    if (n < (int)jFrameBytes) { jSrcLeft = 0; return false; }
    n -= n % jFrameBytes;
    jBufLen = (uint16_t)n; jBufPos = 0;
  }
  uint8_t* p = &jBuf[jBufPos];
  jBufPos += jFrameBytes;
  jSrcLeft--;
  float l, r;
  if (jSrc8) {
    l = ((int)p[0] - 128) * (1.0f / 128.0f);
    r = jSrcStereo ? (((int)p[1] - 128) * (1.0f / 128.0f)) : l;
  } else {
    l = (int16_t)(p[0] | (p[1] << 8)) * (1.0f / 32768.0f);
    r = jSrcStereo ? ((int16_t)(p[2] | (p[3] << 8)) * (1.0f / 32768.0f)) : l;
  }
  *out = (l + r) * 0.5f;
  return true;
}

static bool jWavStart(const char* path) {
  jWav = SD.open(path, FILE_READ);
  if (!jWav) return false;
  uint8_t h[12];
  if (jWav.read(h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
    jWav.close(); return false;
  }
  uint16_t fmt = 0, ch = 0, bits = 0;
  uint32_t rate = 0, dataLen = 0;
  bool haveFmt = false, haveData = false;
  while (jWav.available() >= 8) {
    uint8_t ck[8];
    if (jWav.read(ck, 8) != 8) break;
    uint32_t sz = (uint32_t)ck[4] | ((uint32_t)ck[5] << 8) |
                  ((uint32_t)ck[6] << 16) | ((uint32_t)ck[7] << 24);
    if (!memcmp(ck, "fmt ", 4)) {
      uint8_t fb[16];
      if (jWav.read(fb, 16) < 16) break;
      fmt  = fb[0] | (fb[1] << 8);
      ch   = fb[2] | (fb[3] << 8);
      rate = (uint32_t)fb[4] | ((uint32_t)fb[5] << 8) |
             ((uint32_t)fb[6] << 16) | ((uint32_t)fb[7] << 24);
      bits = fb[14] | (fb[15] << 8);
      if (sz > 16) jWav.seek(jWav.position() + (sz - 16));
      haveFmt = true;
    } else if (!memcmp(ck, "data", 4)) {
      dataLen = sz; haveData = true;
      break;
    } else {
      jWav.seek(jWav.position() + sz + (sz & 1u));
    }
  }
  if (!haveFmt || !haveData || (fmt != 1 && fmt != 0xFFFE) ||
      (bits != 8 && bits != 16) || ch < 1 || ch > 2 || rate == 0 || rate > 96000) {
    jWav.close(); return false;
  }
  uint8_t fbytes = (bits / 8) * ch;
  uint32_t frames = dataLen / fbytes;
  uint32_t cap = (uint32_t)JINGLE_MS_MAX * rate / 1000u;
  if (frames > cap) frames = cap;
  if (frames == 0) { jWav.close(); return false; }
  jFrameBytes = fbytes; jSrc8 = (bits == 8); jSrcStereo = (ch == 2);
  jStep = (float)rate / (float)JINGLE_RATE;
  jSrcLeft = frames; jBufPos = jBufLen = 0;
  jWavIsOpen = true;
  Serial.printf("[JN] wav %s %uHz %uch %ubit %ums\n", path, (unsigned)rate,
                (unsigned)ch, (unsigned)bits, (unsigned)(frames * 1000u / rate));
  return true;
}
static void jWavStop() { if (jWavIsOpen) { jWav.close(); jWavIsOpen = false; } }

// ── playback state ──
static uint32_t jOutIdx = 0;
static float    jOutSample = 0.0f;
static bool     jUseWav = false, jSrcOk = false;
static float    jCurSrc = 0.0f, jFrac = 0.0f;

// true only after SD.begin() succeeded (SD mode). BT mode leaves this false so
// it never touches the card - Bluedroid needs the heap far more than SD does.
static bool sdReady = false;

// Source priority: /sounds/*.wav on SD (user can swap it any time) ->
// PCM embedded in flash -> procedural chime.
static void jingleStart(int idx) {
  jCur = idx; jOutIdx = 0; jOutSample = 0.0f;
  jUseWav = false; jSrcOk = false; jFrac = 0.0f;
  jEmbMode = false; jEmbPos = 0;
  jWavStop();
  if (sdReady && SD.cardType() != CARD_NONE && jWavStart(JINGLE_WAV[idx])) {
    jUseWav = true;
    jSrcOk = jPullSrc(&jCurSrc);
    return;
  }
  if (JINGLE_EMB_LEN[idx] > 0) {
    jEmbMode = true;
    jStep = (float)JINGLE_EMB_RATE / (float)JINGLE_RATE;
    jSrcLeft = JINGLE_EMB_LEN[idx];
    jSrcOk = jPullSrc(&jCurSrc);
  }
  Serial.printf("[JN] play %s src=%s\n", JINGLE_WAV[idx],
                jUseWav ? "sd-wav" : (jEmbMode ? "flash-pcm" : "synth"));
}

static float jProcSample(uint32_t i) {
  const JNote* notes = J_NOTES[jCur];
  int n = J_NCOUNT[jCur];
  float t = (float)i * (1.0f / (float)JINGLE_RATE);
  float acc = 0.0f;
  for (int k = 0; k < n; k++) {
    float dt = t - notes[k].startMs * 0.001f;
    if (dt < 0.0f || dt >= notes[k].durMs * 0.001f) continue;
    float env = expf(-4.5f * dt);
    if (dt < 0.006f) env *= dt / 0.006f;          // click-free attack
    float ph = notes[k].f * dt;
    float a = notes[k].amp * 0.01f;
    acc += a * (jSine(ph) + 0.20f * jSine(ph * 2.0f) + 0.05f * jSine(ph * 3.0f)) * env;
  }
  return acc * 0.55f;    // headroom: overlapping notes must not clip
}

// Next output frame (mono, full scale). false = chime finished.
static bool jNextOut() {
  if (jUseWav || jEmbMode) {
    if (!jSrcOk) return false;
    jOutSample = jCurSrc;
    jFrac += jStep;
    while (jFrac >= 1.0f) {
      float v;
      if (!jPullSrc(&v)) { jSrcOk = false; break; }
      jCurSrc = v; jFrac -= 1.0f;
    }
    jOutIdx++;
    return true;
  }
  if (jOutIdx >= (uint32_t)J_TOTAL_MS[jCur] * JINGLE_RATE / 1000u) return false;
  jOutSample = jProcSample(jOutIdx);
  jOutIdx++;
  return true;
}

// Blocking playback in the caller's context (SD mode: setup()).
static void playJingleBlocking(int idx) {
  if (!audioOut) return;
  audioOut->jingleMode = true;
  jingleStart(idx);
  int16_t fr[2];
  uint32_t frames = 0;
  while (jNextOut()) {
    int16_t s = (int16_t)(jOutSample * 32000.0f);
    fr[0] = fr[1] = s;
    int tries = 0;
    while (!audioOut->ConsumeSample(fr)) { delay(1); if (++tries > 500) break; }
    if (++frames > 400000UL) break;   // hard cap (~9 s): never hang the boot
  }
  audioOut->jingleMode = false;
  jWavStop();
  pumpSilence(96);
}

// BT mode: callbacks only queue a request; the core-0 audio task plays it.
static volatile int jingleReq = -1;
static volatile uint32_t jingleLastMs[3] = {0, 0, 0};
static volatile bool jinglesArmed = false;
static volatile uint32_t btStartMs = 0;   // BT mode boot time (boot-event guard)
static volatile bool btSawConnect = false; // a real link-up already happened

static void requestJingle(int idx) {
  if (idx < 0 || idx > 2) return;
  uint32_t now = millis();
  // The stack can report "disconnected" while it is still coming up - that
  // must not fire the disconnect chime right after the boot chime. Once a
  // real connect happened, disconnect events are always chime-worthy.
  if (idx == JINGLE_DISC && !btSawConnect && now - btStartMs < 3000) return;
  if (idx == JINGLE_CONN) btSawConnect = true;
  if (now - jingleLastMs[idx] < 1200) return;   // same chime twice -> ignore
  jingleLastMs[idx] = now;
  jingleReq = idx;
}

static void playJingleTask(int idx) {
  if (!audioOut) return;
  audioOut->jingleMode = true;
  jingleStart(idx);
  int16_t fr[2];
  while (jNextOut()) {
    int16_t s = (int16_t)(jOutSample * 32000.0f);
    fr[0] = fr[1] = s;
    int tries = 0;
    while (!audioOut->ConsumeSample(fr)) { vTaskDelay(1); if (++tries > 500) break; }
    if ((jOutIdx & 0x3FF) == 0) vTaskDelay(1);   // let the BT stack run
  }
  audioOut->jingleMode = false;
  jWavStop();
  btRingR = btRingW;      // phone frames buffered during the chime are stale
}

static void drawBtScreen();
static bool btHandleCtrl(int16_t tx, int16_t ty);

static void btUiWake() { btUiDirty = true; }

static void btAvrcConnCb(bool connected) {
  // Name is DELIBERATELY kept on disconnect: it is the remembered identity
  // shown while waiting for the phone again.
  // (phone AVRCP volume is intentionally NOT read: both sides stay independent)
  // Chime on link up/down. Only a volatile write here — the audio task
  // does the actual I2S work (never touch I2S from a BT callback).
  if (jinglesArmed) requestJingle(connected ? JINGLE_CONN : JINGLE_DISC);
  btUiWake();
}



static void btDataCb(const uint8_t* data, uint32_t len) {
  if (!btModeActive || !data) return;
  const int16_t* p = (const int16_t*)data;
  uint32_t n = len / 4;                       // bytes -> stereo s16 frames
  uint32_t w = btRingW;
  for (uint32_t i = 0; i < n; i++) {
    if ((uint32_t)(w - btRingR) >= BT_RING_FRAMES) { btDropped++; break; }
    uint32_t slot = w & (BT_RING_FRAMES - 1);
    btRing[slot*2] = p[2*i]; btRing[slot*2+1] = p[2*i+1];
    w++;
  }
  btRingW = w;

}

// Dedicated BT audio writer: pinned to core 0 with high priority so the
// phone stream never starves against the SD/UI loop on core 1. Volume is
// applied by the shared I2SOutTap (same volumePercent as SD mode).
static void btAudioTask(void*) {
  while (true) {
    if (!btModeActive) { vTaskDelay(20); continue; }
    // Boot / connect / disconnect chime has priority over the phone stream.
    int jr = jingleReq;
    if (jr >= 0) { jingleReq = -1; playJingleTask(jr); continue; }
    uint32_t w = btRingW;
    uint32_t avail = w - btRingR;
    if (avail == 0) {
      // keep the DMA fed with silence while linked but idle (no pops)
      static int16_t zero[2] = {0, 0};
      for (int i = 0; i < 256; i++)
        if (!audioOut->ConsumeSample(zero)) break;
      vTaskDelay(1);
      continue;
    }
    uint32_t take = avail > 1024 ? 1024 : avail;
    for (uint32_t i = 0; i < take; i++) {
      uint32_t slot = btRingR & (BT_RING_FRAMES - 1);
      btChunk[i * 2]     = btRing[slot * 2];
      btChunk[i * 2 + 1] = btRing[slot * 2 + 1];
      btRingR++;
    }
    int wrote = 0;
    while (wrote < (int)take) {      // never drop what we already took
      if (audioOut->ConsumeSample(&btChunk[wrote * 2])) wrote++;
      else vTaskDelay(1);
    }
  }
}

static void drawBtRune(int cx, int cy, uint16_t col) {
  tft.drawLine(cx, cy-11, cx, cy+11, col);
  tft.drawLine(cx, cy-11, cx+6, cy-5, col);
  tft.drawLine(cx+6, cy-5, cx, cy+1, col);
  tft.drawLine(cx, cy+11, cx-6, cy+5, col);
  tft.drawLine(cx-6, cy+5, cx, cy-1, col);
}

static void btCenterLine(const char* s, int y, int size, uint16_t col, uint16_t bg) {
  char buf[80];
  strncpy(buf, s ? s : "", sizeof(buf)-1);
  buf[sizeof(buf)-1] = 0;
  int maxChars = (size==2) ? (240-20)/12 : (240-20)/6;
  int len = (int)strlen(buf);
  if (len > maxChars) { buf[maxChars-2] = 0; strcat(buf, ".."); len = maxChars; }
  tft.setTextSize(size);
  tft.setTextColor(col, bg);
  tft.setCursor((SCR_W - len*6*size)/2, y);
  tft.print(buf);
  tft.setTextSize(1);
}

/** Raw reader: fires before the volume control with the same decoded PCM.
 *  Used ONLY to measure how much audio the phone actually delivers (bytes/s). */
static void btRawCb(const uint8_t* data, uint32_t len) {
  if (!btModeActive || !data) return;
  btRateBytes += len;
}

/** SPEED line on the BT screen: kbps the phone is delivering (0 when idle). */
static void drawBtSpeedLine() {
  if (screenMode != SCREEN_BT || screenClockActive) return;
  char s[32];
  // FIXED 4-char field: the string length never changes, so the opaque text
  // repaints over the previous digits and the band is never cleared (a
  // fillRect per second was visible as continuous flicker during playback).
  if (btSpeedKbps) snprintf(s, sizeof(s), "SPEED %4u kbps", (unsigned)btSpeedKbps);
  else             snprintf(s, sizeof(s), "SPEED ---- kbps");
  int len = (int)strlen(s);
  tft.setTextSize(1);
  tft.setTextColor(colInfoCyan(), COL_BG);
  tft.setCursor((SCR_W - len*6)/2, 132);
  tft.print(s);
}

/** BT-mode housekeeping: SUCCESS splash expiry, 1 s SPEED window, stats log.
 *  Called from both BT run loops (BT-only boot and switched-at-runtime). */
static void updateBtUi(unsigned long now) {
  if (btSuccessUntil && (long)(now - btSuccessUntil) >= 0) {
    btSuccessUntil = 0;
    btUiWake();                        // splash over -> steady screen
  }
  static unsigned long lastRateMs = 0;
  static uint32_t lastBytes = 0;
  static unsigned long lastStatMs = 0;
  if (now - lastRateMs >= 1000) {
    lastRateMs = now;
    uint32_t b = btRateBytes;
    uint32_t kbps = (uint32_t)(((uint64_t)(b - lastBytes) * 8ull) / 1000ull);
    lastBytes = b;
    uint32_t prev = btSpeedKbps;
    btSpeedKbps = kbps ? (prev ? (prev * 2u + kbps) / 3u : kbps) : 0;   // smooth, snap to 0
    if (btConnected && btSpeedKbps != prev && screenMode == SCREEN_BT) drawBtSpeedLine();
  }
  if (now - lastStatMs >= 10000) {     // log only: link health / clock drift check
    lastStatMs = now;
    Serial.printf("[BT] stat heap=%u drop=%u speed=%u\n",
                  (unsigned)ESP.getFreeHeap(), (unsigned)btDropped,
                  (unsigned)btSpeedKbps);
  }
}

static void drawBtScreen() {
  btUiDirty = false;
  tft.fillScreen(COL_BG);
  // header: title at size 1 (smaller) + the SD switch
  tft.fillRect(0,0,SCR_W,30,colTopBarBg());
  tft.drawFastHLine(0,29,SCR_W,tft.color565(40,40,48));
  tft.setTextSize(1); tft.setTextColor(colInfoCyan(),colTopBarBg());
  tft.setCursor(10,12); tft.print("Bluetooth");
  tft.setCursor(196,12); tft.print("< SD");

  const uint16_t GREEN = tft.color565(60,200,90);
  bool conn = btConnected;
  // NO peer-name row (Phúc, 12/9): the AVRCP name never arrives on some
  // phones and a placeholder reads as faked data - show the connection
  // state and the live numbers only.
  bool splash = conn && btSuccessUntil != 0 && (long)(millis() - btSuccessUntil) < 0;

  // Variant B (Phúc 12/9): state word at y=100, remembered phone name as a
  // small line under it (cyan = linked, grey = remembered but not linked).
  // No name saved yet -> nothing is drawn (never a placeholder).
  // No phone-name row (Phúc 12/9): the AVRCP name is unreliable on his phone,
  // so the screen shows the connection state + live numbers only.
  btDrawnConn = conn; btDrawnSplash = splash;   // what this frame shows
  if (splash) {
    // first 5 s after a new link: text only, no icon
    btCenterLine("SUCCESS", 76, 2, GREEN, COL_BG);
    btCenterLine("connected", 100, 1, GREEN, COL_BG);
  } else {
    drawBtRune(120, 52, conn ? GREEN : colInfoCyan());
    if (conn) {
      btCenterLine("connected", 100, 1, GREEN, COL_BG);
      drawBtSpeedLine();
    } else {
      btCenterLine("waiting for phone ...", 100, 1, COL_DIM, COL_BG);
    }
  }

  // volume control row (works on the ESP, syncs with the phone)
  tft.fillRoundRect(BT_CTRL_L, BT_ROW_VOL_Y, BT_CTRL_LW, BT_CTRL_H, 7, COL_BTN);
  tft.setTextSize(2); tft.setTextColor(COL_TEXT, COL_BTN);
  tft.setCursor(BT_CTRL_L + BT_CTRL_LW/2 - 6, BT_ROW_VOL_Y + 10); tft.print("-");
  tft.fillRoundRect(BT_CTRL_BAR, BT_ROW_VOL_Y, BT_CTRL_BARW, BT_CTRL_H, 7, COL_BTN);
  char vbuf[8]; snprintf(vbuf,sizeof(vbuf),"%d%%",volumePercent);
  tft.setTextColor(colInfoCyan(), COL_BTN);
  tft.setCursor(BT_CTRL_BAR + BT_CTRL_BARW/2 - (int)strlen(vbuf)*6, BT_ROW_VOL_Y + 10);
  tft.print(vbuf);
  tft.fillRoundRect(BT_CTRL_PLUS, BT_ROW_VOL_Y, BT_CTRL_LW, BT_CTRL_H, 7, COL_BTN);
  tft.setTextColor(COL_TEXT, COL_BTN);
  tft.setCursor(BT_CTRL_PLUS + BT_CTRL_LW/2 - 6, BT_ROW_VOL_Y + 10); tft.print("+");
  tft.setTextSize(1);
  btCenterLine("Volume", BT_ROW_VOL_Y + BT_CTRL_H + 10, 1, COL_DIM, COL_BG);

  // brightness control row
  tft.fillRoundRect(BT_CTRL_L, BT_ROW_BRI_Y, BT_CTRL_LW, BT_CTRL_H, 7, COL_BTN);
  tft.setTextSize(2); tft.setTextColor(COL_TEXT, COL_BTN);
  tft.setCursor(BT_CTRL_L + BT_CTRL_LW/2 - 6, BT_ROW_BRI_Y + 10); tft.print("-");
  tft.fillRoundRect(BT_CTRL_BAR, BT_ROW_BRI_Y, BT_CTRL_BARW, BT_CTRL_H, 7, COL_BTN);
  char bbuf[8]; snprintf(bbuf,sizeof(bbuf),"%d%%",cfgBright);
  tft.setTextColor(colInfoCyan(), COL_BTN);
  tft.setCursor(BT_CTRL_BAR + BT_CTRL_BARW/2 - (int)strlen(bbuf)*6, BT_ROW_BRI_Y + 10);
  tft.print(bbuf);
  tft.fillRoundRect(BT_CTRL_PLUS, BT_ROW_BRI_Y, BT_CTRL_LW, BT_CTRL_H, 7, COL_BTN);
  tft.setTextColor(COL_TEXT, COL_BTN);
  tft.setCursor(BT_CTRL_PLUS + BT_CTRL_LW/2 - 6, BT_ROW_BRI_Y + 10); tft.print("+");
  tft.setTextSize(1);
  btCenterLine("Brightness", BT_ROW_BRI_Y + BT_CTRL_H + 10, 1, COL_DIM, COL_BG);
  // No Clock button any more (Phúc 12/9): BT mode cannot fetch the time, so the
  // area is intentionally empty - the screensavers take over after 5 min idle.
}





// ── Touch (4-point affine calibration, NVS "cal2") ────────
static float calA=1,calB=0,calC=0,calD=0,calE=1,calF=0;
static bool  calDone=false;

static void loadCal() {
  prefs.begin("cal2", true);
  calDone = prefs.getBool("done", false);
  int calRot = prefs.getInt("rot", -1);
  if (calDone && calRot != DISP_ROTATION) {
    // display orientation changed -> old affine no longer valid
    calDone = false;
  }
  if (calDone) {
    calA=prefs.getFloat("a",1); calB=prefs.getFloat("b",0); calC=prefs.getFloat("c",0);
    calD=prefs.getFloat("d",0); calE=prefs.getFloat("e",1); calF=prefs.getFloat("f",0);
  }
  prefs.end();
}
static void saveCal() {
  prefs.begin("cal2", false);
  prefs.putBool("done", true);
  prefs.putInt("rot", DISP_ROTATION);
  prefs.putFloat("a",calA); prefs.putFloat("b",calB); prefs.putFloat("c",calC);
  prefs.putFloat("d",calD); prefs.putFloat("e",calE); prefs.putFloat("f",calF);
  prefs.end();
}

// raw -> screen via affine: X = a*x+b*y+c ; Y = d*x+e*y+f
static void applyCal(int rx, int ry, float* sx, float* sy) {
  *sx = calA*rx + calB*ry + calC;
  *sy = calD*rx + calE*ry + calF;
}

/** Least-squares solve of 3x3 normal equations for (a,b,c): x'=a*x+b*y+c */
static void solveABC(const float xs[], const float ys[], const float ds[], float* out) {
  // M = sum over points of [x;y;1]*[x y 1] ; rhs = sum d*[x;y;1]
  float m[3][3]={{0,0,0},{0,0,0},{0,0,0}}, rhs[3]={0,0,0};
  for (int i=0;i<4;i++){
    float x=xs[i], y=ys[i], d=ds[i];
    m[0][0]+=x*x; m[0][1]+=x*y; m[0][2]+=x;
    m[1][0]+=x*y; m[1][1]+=y*y; m[1][2]+=y;
    m[2][0]+=x;   m[2][1]+=y;   m[2][2]+=1;
    rhs[0]+=d*x;  rhs[1]+=d*y;  rhs[2]+=d;
  }
  // Gaussian elimination
  for (int col=0;col<3;col++){
    int piv=col; float mx=fabsf(m[col][col]);
    for (int r=col+1;r<3;r++) if (fabsf(m[r][col])>mx){ mx=fabsf(m[r][col]); piv=r; }
    for (int c=0;c<3;c++){ float t=m[col][c]; m[col][c]=m[piv][c]; m[piv][c]=t; }
    float t=rhs[col]; rhs[col]=rhs[piv]; rhs[piv]=t;
    float diag=m[col][col];
    if (fabsf(diag)<1e-9f) diag=1e-9f;
    for (int c=0;c<3;c++) m[col][c]/=diag;
    rhs[col]/=diag;
    for (int r=0;r<3;r++){ if (r==col) continue;
      float f=m[r][col];
      if (fabsf(f)<1e-12f) continue;
      for (int c=0;c<3;c++) m[r][c]-=f*m[col][c];
      rhs[r]-=f*rhs[col];
    }
  }
  out[0]=rhs[0]; out[1]=rhs[1]; out[2]=rhs[2];
}

static long rawCalX=0, rawCalY=0;

static bool waitOneTap(int cx, int cy) {
  // draw crosshair target; wait for a clean touch (average 8 raw reads)
  tft.fillScreen(COL_BG);
  tft.drawCircle(cx,cy,8,TFT_RED);
  tft.drawFastHLine(cx-16,cy,32,TFT_RED);
  tft.drawFastVLine(cx,cy-16,32,TFT_RED);
  unsigned long t0=millis();
  int got=0; long sx=0,sy=0;
  while (millis()-t0<8000) {
    if (ts.touched()) {
      TS_Point p=ts.getPoint();
      sx+=p.x; sy+=p.y; got++;
      if (got>=8) {
        t0=millis();
        while (ts.touched()) delay(2);
        break;
      }
    }
    delay(5);
  }
  if (got<8) return false;
  // keep raw average for caller via static
  rawCalX = sx/got; rawCalY = sy/got;
  return true;
}

static void runCalibration() {
  // 4 targets in display coords (portrait)
  const int txs[4]={30,210,210,30};
  const int tys[4]={30,30,290,290};
  float xs[4], ys[4], dsx[4], dsy[4];
  for (int i=0;i<4;i++){
    if (!waitOneTap(txs[i],tys[i])) { i--; continue; }  // keep waiting until tapped
    xs[i]=(float)rawCalX; ys[i]=(float)rawCalY;
    dsx[i]=(float)txs[i]; dsy[i]=(float)tys[i];
  }
  float abc[3], def[3];
  solveABC(xs,ys,dsx,abc);   // X = a*x+b*y+c
  solveABC(xs,ys,dsy,def);   // Y = d*x+e*y+f
  calA=abc[0]; calB=abc[1]; calC=abc[2];
  calD=def[0]; calE=def[1]; calF=def[2];
  saveCal();
  calDone=true;
  tft.fillScreen(COL_BG);
  tft.setTextSize(1);
  tft.setTextColor(tft.color565(50,205,50),COL_BG);  // COL_ACCENT
  const char* ok="Calibration OK";
  tft.setCursor((SCR_W-6*(int)strlen(ok))/2,150);
  tft.print(ok);
  delay(600);
}

static bool getTouchXY(int16_t &tx, int16_t &ty) {
  if (!ts.touched()) return false;
  TS_Point p = ts.getPoint();
  float fx,fy;
  applyCal(p.x,p.y,&fx,&fy);
  tx=(int16_t)fx; ty=(int16_t)fy;
  tx=constrain(tx,0,SCR_W-1);
  ty=constrain(ty,0,SCR_H-1);
  return true;
}

static void handleSettingsTouch() {
  int16_t tx,ty;
  if (!getTouchXY(tx,ty)) return;
  noteUserActivity();
  unsigned long now=millis();
  if (now-lastTouchTime<TOUCH_DEBOUNCE_MS) return;
  lastTouchTime=now;
  while (ts.touched()) { audioPumpDecode(40); delay(1); if (millis()-now>400) break; }
  // back
  if (ty<30 && tx>=170) { screenMode=SCREEN_BROWSER; drawBrowser(); return; }
  // rows come from SET_ROW_Y[] - the same array the drawing uses
  if (ty>=SET_ROW_Y[SET_INVERT] && ty<SET_ROW_Y[SET_INVERT]+SET_CARD_H) { toggleInvert(); drawSettings(); return; }
  if (ty>=SET_ROW_Y[SET_BRIGHT] && ty<SET_ROW_Y[SET_BRIGHT]+SET_CARD_H) {
    bool changed=false;
    if (tx>=SET_STEP_L && tx<SET_STEP_L+SET_STEP_BTN) { setBrightnessPct(cfgBright-10); changed=true; }
    else if (tx>=SET_STEP_R && tx<SET_STEP_R+SET_STEP_BTN) { setBrightnessPct(cfgBright+10); changed=true; }
    if (changed) { prefs.begin(NS_CFG,false); prefs.putInt("bright",cfgBright); prefs.end(); }
    drawSettings(); return;
  }
  if (ty>=SET_ROW_Y[SET_BTMODE] && ty<SET_ROW_Y[SET_BTMODE]+SET_CARD_H) { btSwitchToBt(); return; }
  if (ty>=SET_ROW_Y[SET_RECAL] && ty<SET_ROW_Y[SET_RECAL]+SET_CARD_H) {
    prefs.begin("cal2", false);
    prefs.clear();
    prefs.end();
    runCalibration();
    screenMode=SCREEN_SETTINGS;
    drawSettings();
    return;
  }
  if (ty>=SET_ROW_Y[SET_CLOCK] && ty<SET_ROW_Y[SET_CLOCK]+SET_CARD_H) {
    wakeBackScreen=SCREEN_SETTINGS;
    enterClockScreen();
    return;
  }
}

static void handleWifiListTouch() {
  int16_t tx,ty;
  if (!getTouchXY(tx,ty)) return;
  noteUserActivity();
  unsigned long now=millis();
  if (now-lastTouchTime<TOUCH_DEBOUNCE_MS) return;
  lastTouchTime=now;
  while (ts.touched()) { delay(1); if (millis()-now>400) break; }
  if (ty<30 && tx>=170) {
    if (bootWifiPhase) { bootWifiSkip = true; return; }   // boot phase: "< skip"
    screenMode = (wakeBackScreen==SCREEN_BROWSER || wakeBackScreen==SCREEN_PLAYER)
                 ? wakeBackScreen : SCREEN_SETTINGS;
    redrawCurrentScreen();
    return;
  }
  if (!wifiScanDone) return;
  if (ty>=290) {
    if (tx<80) { startWifiScan(); drawWifiList(); return; }       // rescan
    if (tx>=180 && (wifiScroll+WL_VIS)<wifiCount) { wifiScroll+=WL_VIS; drawWifiList(); return; } // more
    return;
  }
  int idx=wifiScroll+(ty-WL_Y0)/WL_ITEMH;
  if (idx<0 || idx>=wifiCount) return;
  if (!wifiLocked[idx]) {
    beginWifiConnect(wifiNames[idx], "");
    tft.fillScreen(COL_BG);
    tft.setTextSize(2);
    tft.setTextColor(COL_TEXT,COL_BG);
    const char* m1="Connecting";
    tft.setCursor((SCR_W-12*(int)strlen(m1))/2, 120);
    tft.print(m1);
    screenMode=SCREEN_CONNECTING;
    return;
  }
  kbTargetSSIDIdx=idx;
  strncpy(kbSSIDName,wifiNames[idx],32); kbSSIDName[32]='\0';
  kbCur=0; kbPass[0]='\0'; kbShift=false; kbShowPass=false;
  screenMode=SCREEN_KEYBOARD;
  drawKeyboard();
}

static void handleKeyboardTouch() {
  int16_t tx,ty;
  if (!getTouchXY(tx,ty)) return;
  noteUserActivity();
  unsigned long now=millis();
  if (now-lastTouchTime<TOUCH_DEBOUNCE_MS) return;
  lastTouchTime=now;
  while (ts.touched()) { delay(1); if (millis()-now>400) break; }
  // bottom controls
  int by=KB_Y0+KB_NROW*KB_KEY_H+2;
  if (ty>=by && ty<by+30) {
    if (tx<66) { kbShift=!kbShift; drawKeyboard(); return; }
    if (tx<104) { kbAddChar('\b'); drawKeyboard(); return; }
    if (tx<142) { kbCur=0; kbPass[0]='\0'; drawKeyboard(); return; }
    if (tx>=146) {
      if (kbTargetSSIDIdx>=0 && kbTargetSSIDIdx<wifiCount)
        beginWifiConnect(wifiNames[kbTargetSSIDIdx], kbPass);
      else if (kbSSIDName[0]) beginWifiConnect(kbSSIDName, kbPass);
      // show connecting feedback screen
      tft.fillScreen(COL_BG);
      tft.setTextSize(2);
      tft.setTextColor(COL_TEXT,COL_BG);
      const char* m1="Connecting";
      tft.setCursor((SCR_W-12*(int)strlen(m1))/2, 120);
      tft.print(m1);
      tft.setTextSize(1);
      tft.setTextColor(COL_DIM,COL_BG);
      tft.setCursor(10, 160); tft.print(kbSSIDName);
      screenMode=SCREEN_CONNECTING;
      return;
    }
    return;
  }
  // header back + Show/Hide (variant B layout)
  if (ty<30 && tx>=180) {            // < back -> WiFi list, password kept
    screenMode=SCREEN_WIFI_LIST; drawWifiList(); return;
  }
  if (ty>=51 && ty<=69 && tx>=186) { // reveal / hide the password (chip inside the field)
    kbShowPass=!kbShowPass; drawKeyboard(); return;
  }
  if (ty<KB_Y0) return;
  int row=(ty-KB_Y0)/KB_KEY_H;
  if (row<0 || row>=KB_NROW) return;
  const char* r=KB_ROWS[row];
  int len=(int)strlen(r);
  int colW=24;
  int rowW=len*colW;
  int x0=(SCR_W-rowW)/2;
  int col=(tx-x0)/colW;
  if (col<0 || col>=len) return;
  kbAddChar(kbCharAt(row,col));
  drawKeyboard();
}

static void handleBrowserTouch() {
  int16_t tx,ty;
  if (!getTouchXY(tx,ty)) return;
  noteUserActivity();
  unsigned long now=millis();
  if (now-lastTouchTime<TOUCH_DEBOUNCE_MS) return;
  lastTouchTime=now;
  { unsigned long rel=millis();
    while (ts.touched()){ audioPumpDecode(60); if (millis()-rel>500) break; delay(1);} }
  // header: < List back OR settings
  if (ty<browseHeaderH) {
    if (browseLevel==BROWSE_TRACKS && tx<64) { browseLevel=BROWSE_ALBUMS; browseTrackScroll=0; drawBrowser(); return; }
    if (tx>=SETT_BTN_X && tx<SETT_BTN_X+SETT_BTN_W) { screenMode=SCREEN_SETTINGS; drawSettings(); return; }
    return;
  }
  if (trackCount>0 &&
      ty>=BROWSE_PLAYER_BTN_Y && ty<BROWSE_PLAYER_BTN_Y+BROWSE_PLAYER_BTN_H &&
      tx>=BROWSE_PLAYER_BTN_X && tx<BROWSE_PLAYER_BTN_X+BROWSE_PLAYER_BTN_W) {
    screenMode=SCREEN_PLAYER; drawPlayer(); return;
  }
  if (trackCount>0 &&
      ty>=BROWSE_PATH_PLAY_BTN_Y && ty<BROWSE_PATH_PLAY_BTN_Y+BROWSE_PATH_PLAY_BTN_H &&
      tx>=BROWSE_PATH_PLAY_BTN_X && tx<BROWSE_PATH_PLAY_BTN_X+BROWSE_PATH_PLAY_BTN_W) {
    screenMode=SCREEN_PLAYER; drawPlayer(); return;
  }
  int fY=footerY(), vis=visibleSlots();
  int itemCount=(browseLevel==BROWSE_ALBUMS)?albumCount:browseTrackCount;
  // PREV
  if (ty>=fY && ty<=fY+browseFooterH && tx>=0 && tx<=72) {
    if (browseLevel==BROWSE_ALBUMS) albumScroll=max(0,albumScroll-vis);
    else browseTrackScroll=max(0,browseTrackScroll-vis);
    drawBrowser(); return;
  }
  // NEXT
  if (ty>=fY && ty<=fY+browseFooterH && tx>=168 && tx<=SCR_W) {
    if (browseLevel==BROWSE_ALBUMS){ if (albumScroll+vis<albumCount) albumScroll+=vis; }
    else { if (browseTrackScroll+vis<browseTrackCount) browseTrackScroll+=vis; }
    drawBrowser(); return;
  }
  int listTop=browseListY, listBottom=browseListY+vis*browseItemH;
  if (ty<listTop || ty>=listBottom) return;
  int indexInView=(ty-browseListY)/browseItemH;
  int scroll=(browseLevel==BROWSE_ALBUMS)?albumScroll:browseTrackScroll;
  int idx=scroll+indexInView;
  if (idx<0 || idx>=itemCount) return;
  int y=browseListY+indexInView*browseItemH;
  tft.fillRoundRect(6,y+1,SCR_W-12,browseItemH-3,5,COL_BTN_ACT);
  { unsigned long t0=millis(); while (millis()-t0<50) audioPumpDecode(120); }
  if (browseLevel==BROWSE_ALBUMS) {
    loadBrowseAlbumTracks(albums[idx]);
    browseLevel=BROWSE_TRACKS;
    browseTrackScroll=0;
    drawBrowser(); return;
  }
  if (playerState!=STATE_STOPPED) stopTrack(true);
  startPlayingFromPlaylistIndex(browseTrackIndices[idx]);
  screenMode=SCREEN_PLAYER;
  drawPlayer();
}

static void handlePlayerTouch() {
  int16_t tx,ty;
  if (!getTouchXY(tx,ty)) return;
  noteUserActivity();
  unsigned long now=millis();
  if (now-lastTouchTime<TOUCH_DEBOUNCE_MS) return;
  lastTouchTime=now;
  { unsigned long rel=millis();
    while (ts.touched()){ audioPumpDecode(60); if (millis()-rel>500) break; delay(1);} }
  // INV button
  if (ty>=PL_INV_BTN_Y && ty<PL_INV_BTN_Y+PL_INV_BTN_H &&
      tx>=PL_INV_BTN_X && tx<PL_INV_BTN_X+PL_INV_BTN_W) {
    toggleInvert(); drawPlayer(); return;
  }
  // list icon -> browser (playback continues)
  if (ty>=PL_BACK_BTN_Y && ty<PL_BACK_BTN_Y+PL_BACK_BTN_H &&
      tx>=PL_BACK_BTN_X && tx<PL_BACK_BTN_X+PL_BACK_BTN_W) {
    screenMode=SCREEN_BROWSER;
    drawBrowser();
    audioPumpDecode(256);
    return;
  }
  // volume row
  if (ty>=PL_VOLUME_Y && ty<=PL_VOLUME_Y+32) {
    if (tx<68) {
      // down: ...20 -> 10 -> 5 -> 4 -> 3 -> 2 -> 1 -> 0
      if      (volumePercent > 10)        volumePercent -= 10;
      else if (volumePercent == 10)       volumePercent = 5;
      else if (volumePercent > 0)         volumePercent -= 1;
      applyVolumePercent(); drawVolumeControls(); prefs.begin(NS_CFG,false); prefs.putInt("vol",volumePercent); prefs.end(); return;
    }
    else if (tx>=172) {
      // up: 0 -> 1 -> 2 -> 3 -> 4 -> 5 -> 10 -> 20 ...
      if      (volumePercent < 5)         volumePercent += 1;
      else if (volumePercent < 10)        volumePercent = 10;   // 5..9 -> 10 (boot is 9)
      else                                volumePercent = min(100, volumePercent + 10);
      applyVolumePercent(); drawVolumeControls(); prefs.begin(NS_CFG,false); prefs.putInt("vol",volumePercent); prefs.end(); return;
    }
  }
  // transport
  int y=PL_TRANSPORT_Y;
  if (ty>=y && ty<=y+44) {
    if (tx<68) prevTrack();
    else if (tx<172) togglePause();
    else nextTrack();
    drawPlayer();
  }
}

static void handleTouch() {
  if (!displayBacklightOn) return;
  switch (screenMode) {
    case SCREEN_BROWSER: handleBrowserTouch(); break;
    case SCREEN_PLAYER: handlePlayerTouch(); break;
    case SCREEN_SETTINGS: handleSettingsTouch(); break;
    case SCREEN_WIFI_LIST: handleWifiListTouch(); break;
    case SCREEN_KEYBOARD: handleKeyboardTouch(); break;
    case SCREEN_CONNECTING: break;  // wait for connect result (updateWifiConnecting handles it)
    case SCREEN_CLOCK: {
      // any touch wakes back to the screen that was showing before
      int16_t tx,ty;
      if (!getTouchXY(tx,ty)) return;
      noteUserActivity();
      screenMode = wakeBackScreen;
      redrawCurrentScreen();
      break;
    }
  }
}

// ── Boot button + backlight ────────────────────────────────
static void pollBootButton() {
  bool down=(digitalRead(BOOT_BUTTON_PIN)==LOW);
  unsigned long now=millis();
  if (bootBtnPhase==0){ if(down){ bootBtnPhase=1; bootBtnMs=now; } }
  else if (bootBtnPhase==1){
    if(!down) bootBtnPhase=0;
    else if (now-bootBtnMs>=45){
      if (screenClockActive) {
        // wake back to the screen that was showing before idle clock
        noteUserActivity();
        screenMode = wakeBackScreen;
        redrawCurrentScreen();
      }
      else if (displayBacklightOn) {
        displaySetOn(false);
      } else {
        displaySetOn(true);
        redrawCurrentScreen();
      }
      bootBtnPhase=2;
    }
  } else { if(!down) bootBtnPhase=0; }
}

static void updateDisplayTimeout() {
  if (!displayBacklightOn) return;
  if (screenClockActive) return;  // clock stays until touched
  // only idle into clock from main screens; never interrupt settings/wifi/keyboard
  // BT mode is deliberately NOT in this list (Phúc 12/9): it has no network, so
  // the clock would show a guessed time - the BT screensavers handle its idle.
  if (screenMode != SCREEN_BROWSER && screenMode != SCREEN_PLAYER) return;
  if (millis()-lastUserActivityMs >= DISPLAY_IDLE_OFF_MS) {
    enterClockScreen();
    lastUserActivityMs = millis();
  }
}

static void redrawCurrentScreen() {
  switch (screenMode) {
    case SCREEN_BROWSER: drawBrowser(); break;
    case SCREEN_PLAYER: drawPlayer(); break;
    case SCREEN_SETTINGS: drawSettings(); break;
    case SCREEN_BT: drawBtScreen(); break;
    case SCREEN_WIFI_LIST: drawWifiList(); break;
    case SCREEN_KEYBOARD: drawKeyboard(); break;
    case SCREEN_CONNECTING: break;
    case SCREEN_CLOCK: drawClockScreen(); break;
    case SCREEN_SAVER_LIFE:   svDrawLifeFull();   break;
    case SCREEN_SAVER_MATRIX: svDrawMatrixFull(); break;
  }
}

// ── Startup screen ─────────────────────────────────────────
/** DIAGNOSTIC (temporary): name a non-power-on reset, else null. Lets the SD
 *  auto-advance reboot identify itself on screen with no serial console. */
static const char* lastResetReasonText() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return nullptr;           // clean boot: say nothing
    case ESP_RST_SW:        return "RESET: sw restart";
    case ESP_RST_PANIC:     return "RESET: PANIC (serial!)";
    case ESP_RST_INT_WDT:   return "RESET: int WDT";
    case ESP_RST_TASK_WDT:  return "RESET: task WDT";
    case ESP_RST_WDT:       return "RESET: WDT";
    case ESP_RST_BROWNOUT:  return "RESET: BROWNOUT pwr";
    case ESP_RST_DEEPSLEEP: return "RESET: deepsleep";
    default:                return "RESET: unknown";
  }
}

static void drawStartupScreen() {
  tft.fillScreen(COL_BG);
  tft.setTextSize(2);
  tft.setTextColor(COL_TEXT,COL_BG);
  const char* a="ALBUM PLAYER";
  tft.setCursor((SCR_W-12*(int)strlen(a))/2,60);
  tft.print(a);
  uint16_t note=colInfoCyan();
  tft.fillCircle(SCR_W/2-24,120,5,note);
  tft.fillCircle(SCR_W/2-6,116,5,note);
  tft.fillRect(SCR_W/2-18,96,6,30,note);
  tft.fillRect(SCR_W/2,92,6,30,note);
  tft.drawFastHLine(SCR_W/2-16,92,20,note);
  tft.setTextSize(1);
  tft.setTextColor(COL_DIM,COL_BG);
  const char* v="cyd-album-v2";
  tft.setCursor((SCR_W-6*(int)strlen(v))/2,165);
  tft.print(v);
  const char* rr = lastResetReasonText();
  if (rr) {
    tft.setTextColor(TFT_RED, COL_BG);
    tft.setCursor((SCR_W-6*(int)strlen(rr))/2,182);
    tft.print(rr);
  }
  const char* m="Loading /music ...";
  tft.setCursor((SCR_W-6*(int)strlen(m))/2,230);
  tft.print(m);
}

// ── Mode switch (clean reboot between SD player and BT-only speaker) ──
// no-PSRAM CYD: SD stack and the classic-BT stack never run in the same
// session. Switching mode stores an NVS flag and ESP.restart()s, so every
// boot is a clean single-stack boot (this is what made BT drop-free).
static void btSwitchToBt() {   // from the SD Settings -> Bluetooth row
  // BT mode always boots at full volume: the phone's volume bar then
  // becomes the direct volume control once connected.
  prefs.begin(NS_CFG, false);
  prefs.putString("bootmode", "bt");
  prefs.putInt("vol", 100);
  prefs.end();
  tft.fillScreen(COL_BG);
  tft.setTextSize(1); tft.setTextColor(TFT_GREEN, COL_BG);
  tft.setCursor(20, 150); tft.print("Reboot to Bluetooth mode ...");
  delay(700);
  ESP.restart();
}

static void btSwitchToSd() {   // from the BT-only screen back button
  // Coming back to the SD player starts at its default volume (9), not
  // the 100% that BT mode uses.
  prefs.begin(NS_CFG, false);
  prefs.putString("bootmode", "sd");
  prefs.putInt("vol", 10);
  prefs.end();
  tft.fillScreen(COL_BG);
  tft.setTextSize(1); tft.setTextColor(TFT_GREEN, COL_BG);
  tft.setCursor(20, 150); tft.print("Reboot to SD mode ...");
  delay(700);
  ESP.restart();
}

// ── BT screensavers (Phúc 12/9) ────────────────────────────────────────────
// BT mode has no network, so its clock is gone; instead, after 5 min without a
// touch the two screensavers alternate every 5 min:
//     Conway's Game of Life  <->  Matrix digital rain (katakana)
// A tap anywhere returns to the BT screen; the phone stream keeps playing
// (the audio task owns core 0, this runs on core 1).
#define SV_SWITCH_MS 300000UL          // 5 min: idle -> saver, and saver -> saver
#define LIFE_W 60
#define LIFE_H 80                      // full panel: 80 * 4 px = 320"
#define LIFE_CELL 4
#define MX_COLS 20
#define MX_ROWS 32                     // full panel: 32 * 10 px = 320
#define MX_PX 12
#define MX_PY 10
#define MX_TRAIL 12

static uint8_t lifeCur[LIFE_W*LIFE_H/8];
static uint8_t lifeNxt[LIFE_W*LIFE_H/8];
static uint8_t mxGlyph[MX_COLS][MX_ROWS];
static int8_t  mxHead[MX_COLS];
static uint8_t mxLen[MX_COLS];
static unsigned long svEnteredMs = 0;
static unsigned long svLastFrameMs = 0;
static uint8_t svNext = 1;             // 1 -> Life next, 2 -> Matrix next

static inline void lifeSetBit(uint8_t* b, int idx, bool v) {
  if (v) b[idx >> 3] |= (uint8_t)(1u << (idx & 7));
  else   b[idx >> 3] &= (uint8_t)~(1u << (idx & 7));
}
static inline bool lifeGetBit(const uint8_t* b, int x, int y) {
  if (x < 0) x += LIFE_W; else if (x >= LIFE_W) x -= LIFE_W;
  if (y < 0) y += LIFE_H; else if (y >= LIFE_H) y -= LIFE_H;
  int i = y * LIFE_W + x;
  return (b[i >> 3] >> (i & 7)) & 1;
}
static inline uint16_t svLifeCol() { return tft.color565(88,190,245); }

static void svDrawLifeFull() {
  tft.fillScreen(COL_BG);
  uint16_t col = svLifeCol();
  for (int y = 0; y < LIFE_H; y++) {
    for (int x = 0; x < LIFE_W; x++) {
      if (lifeGetBit(lifeCur, x, y))
        tft.fillRect(x*LIFE_CELL, y*LIFE_CELL, LIFE_CELL, LIFE_CELL, col);
    }
  }
}
static void svLifeSeed() {
  memset(lifeCur, 0, sizeof(lifeCur));
  for (int i = 0; i < LIFE_W*LIFE_H; i++) if (random(100) < 28) lifeSetBit(lifeCur, i, true);
}
static void svLifeStep() {
  uint16_t col = svLifeCol();
  for (int y = 0; y < LIFE_H; y++) {
    for (int x = 0; x < LIFE_W; x++) {
      int n = 0;
      for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
          if (!dx && !dy) continue;
          if (lifeGetBit(lifeCur, x+dx, y+dy)) n++;
        }
      bool alive = lifeGetBit(lifeCur, x, y);
      bool next = (n == 3) || (alive && n == 2);
      int i = y * LIFE_W + x;
      bool was = (lifeCur[i >> 3] >> (i & 7)) & 1;
      if (next != was)                      // only the cells that changed
        tft.fillRect(x*LIFE_CELL, y*LIFE_CELL, LIFE_CELL, LIFE_CELL, next ? col : COL_BG);
      lifeSetBit(lifeNxt, i, next);
    }
  }
  memcpy(lifeCur, lifeNxt, sizeof(lifeCur));
}

static void svMatrixDrawCell(int c, int r, uint8_t level) {
  uint16_t col = (level == 0) ? tft.color565(210,255,210)
               : (level == 1) ? tft.color565(60,230,110)
               : (level == 2) ? tft.color565(30,170,70)
                              : tft.color565(18,90,38);
  tft.drawBitmap(c*MX_PX + 2, r*MX_PY + 1, KATA8[mxGlyph[c][r]], 8, 8, col);
}
static void svMatrixEraseCell(int c, int r) {
  tft.drawBitmap(c*MX_PX + 2, r*MX_PY + 1, KATA8[mxGlyph[c][r]], 8, 8, COL_BG);
}
static void svDrawMatrixFull() {
  tft.fillScreen(COL_BG);
  for (int c = 0; c < MX_COLS; c++) {
    int h = mxHead[c];
    for (uint8_t k = 0; k <= mxLen[c]; k++) {
      int r = (h - (int)k + MX_ROWS) % MX_ROWS;
      svMatrixDrawCell(c, r, k == 0 ? 0 : (k == 1 ? 1 : (k == 2 ? 2 : 3)));
    }
  }
}
static void svMatrixSeed() {
  for (int c = 0; c < MX_COLS; c++) {
    for (int r = 0; r < MX_ROWS; r++) mxGlyph[c][r] = (uint8_t)random(KATA8_N);
    mxHead[c] = (int8_t)random(MX_ROWS);
    mxLen[c] = (uint8_t)(4 + random(MX_TRAIL));
  }
}
static void svMatrixStep() {
  for (int c = 0; c < MX_COLS; c++) {
    int h = (mxHead[c] + 1) % MX_ROWS;
    mxHead[c] = (int8_t)h;
    mxGlyph[c][h] = (uint8_t)random(KATA8_N);
    svMatrixDrawCell(c, h, 0);                                    // new head
    for (int k = 1; k <= 3; k++)                                  // the 3 fading steps
      svMatrixDrawCell(c, (h - k + MX_ROWS) % MX_ROWS, k);
    int e = (h - (mxLen[c] + 1) + MX_ROWS*2) % MX_ROWS;           // just fell off
    svMatrixEraseCell(c, e);
  }
}

static void svEnter(uint8_t which) {
  screenMode = (which == 1) ? SCREEN_SAVER_LIFE : SCREEN_SAVER_MATRIX;
  svEnteredMs = millis();
  svLastFrameMs = 0;
  if (which == 1) { svLifeSeed();   svDrawLifeFull(); }
  else            { svMatrixSeed(); svDrawMatrixFull(); }
}
/** Called from loopBtMode(): BT idle -> saver, then saver -> saver every 5 min. */
static void svIdleTick() {
  unsigned long now = millis();
  if (screenMode == SCREEN_BT) {
    if (now - lastUserActivityMs >= SV_SWITCH_MS) {
      svEnter(svNext);
      svNext = (svNext == 1) ? 2 : 1;
    }
    return;
  }
  if (screenMode == SCREEN_SAVER_LIFE || screenMode == SCREEN_SAVER_MATRIX) {
    if (now - svEnteredMs >= SV_SWITCH_MS) {
      svEnter((screenMode == SCREEN_SAVER_LIFE) ? 2 : 1);
      return;
    }
    unsigned long step = (screenMode == SCREEN_SAVER_LIFE) ? 220 : 90;
    if (now - svLastFrameMs >= step) {
      svLastFrameMs = now;
      if (screenMode == SCREEN_SAVER_LIFE) svLifeStep();
      else                                 svMatrixStep();
    }
  }
}

static void runBtModeSetup() {
  Serial.printf("[BT] boot bluetooth-only mode heap=%u\n", (unsigned)ESP.getFreeHeap());
  // BT mode always runs at full ESP gain: volume lives on the phone.
  volumePercent = 100;
  prefs.begin(NS_CFG, false); prefs.putInt("vol", 100); prefs.end();
  // display + touch init now lives in setup(): the boot WiFi phase needs it

  // BT mode never mounts the SD card (that ate the heap Bluedroid needs and
  // broke pairing) - it uses the PCM embedded in flash. SD files stay an
  // SD-mode-only override; see tools/make_jingles.py to swap the chimes.
  Serial.println("[JN] BT mode uses the embedded chimes");

  audioOut = new I2SOutTap();
  audioOut->SetPinout(I2S_BCLK, I2S_LRCK, I2S_DOUT);
  audioOut->SetRate(44100);
  audioOut->SetChannels(2);
  audioOut->SetUseAPLL();          // exact 44.1 kHz: no drift against the phone clock
  audioOut->begin();
  audioOut->gainNow = 0.0f;

  btSink.set_avrc_connection_state_callback(btAvrcConnCb);
  btSink.set_stream_reader(btDataCb, false);
  btSink.set_raw_stream_reader(btRawCb);     // SPEED measurement (decoded PCM)
  xTaskCreatePinnedToCore(btAudioTask, "btAudio", 4096, nullptr, 8, nullptr, 0);
  applyVolumePercent();

  // No WiFi is ever started in this mode -> the radio is free, start now.
  btSink.start(BT_DEVICE_NAME);
  btModeActive = true;        // audio task drains immediately
  delay(500);
  btConnected = btSink.is_connected();
  Serial.printf("[BT] started heap=%u\n", (unsigned)ESP.getFreeHeap());
  // startup chime (fixed 20% gain) + arm the connect/disconnect chimes
  btStartMs = millis();
  btSawConnect = false;
  requestJingle(JINGLE_BOOT);
  jinglesArmed = true;
  // The BT-only boot IS the BT screen: screenMode stayed SCREEN_BROWSER here
  // before, so drawBtSpeedLine() early-returned (no SPEED line in BT-only
  // mode) and the clock's wake-back would have redrawn the album browser.
  screenMode = SCREEN_BT;
  wakeBackScreen = SCREEN_BT;
  drawBtScreen();

  // Abnormal reset? Print the reason on screen (dim red, bottom-right, font 1)
  // so a crash/wdt/brownout is diagnosable without a serial console. Normal
  // power-on and software resets (mode switch) draw nothing.
  const char* rtxt = nullptr;
  switch ((int)esp_reset_reason()) {
    case 4:  rtxt = "PANIC";      break;
    case 5:  rtxt = "INT_WDT";    break;
    case 6:  rtxt = "TASK_WDT";   break;
    case 7:  rtxt = "WDT";        break;
    case 9:  rtxt = "BROWNOUT";   break;
    case 14: rtxt = "PWR_GLITCH"; break;
    case 15: rtxt = "CPU_LOCKUP"; break;
  }
  if (rtxt) {
    char rbuf[24];
    snprintf(rbuf, sizeof(rbuf), "RST:%s", rtxt);
    tft.setTextDatum(BR_DATUM);
    tft.setTextColor(tft.color565(150, 40, 40), COL_BG);
    tft.drawString(rbuf, 232, 314, 1);
    tft.setTextDatum(TL_DATUM);
  }
}


// ESP volume/brightness steppers - ESP volume is INDEPENDENT from the
// phone's: it drives only our own I2S gain and never notifies the phone.
static void btVolStep(bool up) {
  if (!up) {
    if (volumePercent > 10) volumePercent -= 10;
    else if (volumePercent == 10) volumePercent = 5;
    else if (volumePercent > 0) volumePercent -= 1;
  } else {
    if (volumePercent < 5) volumePercent += 1;
    else if (volumePercent == 5) volumePercent = 10;
    else volumePercent = min(100, volumePercent + 10);
  }
  applyVolumePercent();
  prefs.begin(NS_CFG, false); prefs.putInt("vol", volumePercent); prefs.end();
}

static void btBriStep(bool up) {
  int b = cfgBright + (up ? 10 : -10);
  setBrightnessPct(b);
  prefs.begin(NS_CFG, false); prefs.putInt("bright", cfgBright); prefs.end();
}

// Returns true when the tap landed on a control (volume/brightness row).
static bool btHandleCtrl(int16_t tx, int16_t ty) {
  if (ty >= BT_ROW_VOL_Y && ty < BT_ROW_VOL_Y + BT_CTRL_H) {
    if      (tx <  BT_CTRL_BAR)  btVolStep(false);
    else if (tx >= BT_CTRL_PLUS) btVolStep(true);
    else return false;
    return true;
  }
  if (ty >= BT_ROW_BRI_Y && ty < BT_ROW_BRI_Y + BT_CTRL_H) {
    if      (tx <  BT_CTRL_BAR)  btBriStep(false);
    else if (tx >= BT_CTRL_PLUS) btBriStep(true);
    else return false;
    return true;
  }
  return false;
}

static void btOnlyHandleTouch() {
  int16_t tx, ty;
  if (!getTouchXY(tx, ty)) return;
  unsigned long now = millis();
  if (now - lastTouchTime < TOUCH_DEBOUNCE_MS) return;
  lastTouchTime = now;
  while (ts.touched()) { delay(1); if (millis() - now > 400) break; }
  // Screensaver showing -> any tap returns to the BT screen.
  if (screenMode == SCREEN_SAVER_LIFE || screenMode == SCREEN_SAVER_MATRIX) {
    noteUserActivity();
    screenMode = SCREEN_BT;
    drawBtScreen();
    return;
  }
  if (screenMode == SCREEN_CLOCK) {
    noteUserActivity();
    screenMode = wakeBackScreen;
    redrawCurrentScreen();
    return;
  }
  noteUserActivity();   // taps must defer the 5-min idle clock
  // header "< SD" -> clean reboot back into the SD player
  if (ty < 30 && tx >= 150) { btSwitchToSd(); return; }
  if (btHandleCtrl(tx, ty)) drawBtScreen();
}

static void loopBtMode() {
  pollBootButton();          // BOOT button: backlight toggle
  svIdleTick();              // 5-min idle -> Life/Matrix saver (never the clock)
  static unsigned long lastPoll = 0;
  static int lastConn = -1;      // -1 = seed from the first poll (no boot chime)
  unsigned long now = millis();
  if (now - lastPoll >= 300) {
    lastPoll = now;
    bool c = btSink.is_connected();
    if (lastConn < 0) {
      lastConn = c ? 1 : 0;
      btConnected = c;
    } else if ((c ? 1 : 0) != lastConn) {
      lastConn = c ? 1 : 0;
      btConnected = c;
      if (c) {
        btSuccessUntil = millis() + 5000;
        btRateBytes = 0; btSpeedKbps = 0;
        lastUserActivityMs = millis();
      } else {
        btSuccessUntil = 0;
      }
      // safety net for the chime (requestJingle debounces the callback path)
      if (jinglesArmed) requestJingle(c ? JINGLE_CONN : JINGLE_DISC);
      Serial.printf("[BT] conn=%d heap=%u drop=%u\n", c ? 1 : 0,
                    (unsigned)ESP.getFreeHeap(), (unsigned)btDropped);
      drawBtScreen();
    }
  }
  updateBtUi(now);                 // SUCCESS splash expiry, SPEED window, stats log
  if (btUiDirty) {                 // wake requests: splash expiry, AVRCP events.
    btUiDirty = false;             // Redraw ONLY on a real state change - a full
    if (screenMode == SCREEN_BT) { // drawBtScreen() per wake request flashed the
      bool splashNow = btConnected && btSuccessUntil != 0 &&   // whole screen
                       (long)(millis() - btSuccessUntil) < 0;  // (Phúc 12/9)
      if (splashNow != btDrawnSplash || btConnected != btDrawnConn)
        drawBtScreen();
    }
  }
  static unsigned long lastTouchScan = 0;
  if (now - lastTouchScan >= 120) {
    lastTouchScan = now;
    if (ts.touched()) btOnlyHandleTouch();
  }
  delay(5);
}

// ── Setup / Loop ───────────────────────────────────────────
static void runBtModeSetup();       // Bluetooth-only clean boot (fwd decl)
static void runSdModeSetup();       // album-player boot (fwd decl)
static void loopBtMode();           // Bluetooth-only run loop
static void loopSdMode();           // album-player run loop
static bool bootTimeSync();         // shared boot step (defined below setup())
static bool bootTimeSynced = false; // set when it fetched the time

void setup() {
  Serial.begin(115200);
  delay(300);

  jingleInit();              // sine table for the boot/connect/disconnect chimes

  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);

  tft.init();
  tft.setRotation(DISP_ROTATION);   // reversed from previous build per user
  tft.fillScreen(COL_BG);
  // Backlight PWM on GPIO21 — MUST attach AFTER tft.init(): TFT_eSPI's init
  // does digitalWrite(TFT_BL,HIGH) which would override and kill LEDC PWM.
  ledcAttach(TFT_BL, 5000, 8);
  ledcWrite(TFT_BL, 255);           // full brightness until cfg applied below

  // load NVS config
  prefs.begin(NS_CFG, true);
  cfgInvert = prefs.getBool("invert", false);
  cfgBright = prefs.getInt("bright", 60);
  volumePercent = prefs.getInt("vol", 10);   // SD default 10 (Phúc, 12/9 - the "9" was the boot chime)
  bootModeBt = (prefs.getString("bootmode", "sd") == "bt");
  String s = prefs.getString("ssid", "");
  String p = prefs.getString("pass", "");
  if (s.length()>0){ strncpy(cfgSSID,s.c_str(),32); cfgSSID[32]='\0'; }
  if (p.length()>0){ strncpy(cfgPass,p.c_str(),64); cfgPass[64]='\0'; }
  prefs.end();

  applyInvert();
  setBrightnessPct(cfgBright);

  // Display + touch are common to BOTH modes, and the boot-phase WiFi UI uses
  // them before any mode is chosen (moved up from the mode setups).
  SPI.begin();
  touchSPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  ts.begin(touchSPI);
  ts.setRotation(0);
  loadCal();
  if (!calDone) runCalibration();

  // Two clean boot modes (no PSRAM: SD stack and BT stack never coexist).
  // Switching mode = save NVS flag + ESP.restart() into a clean boot.
  Serial.printf("[BOOT] reason=%d heap=%u\n", (int)esp_reset_reason(), (unsigned)ESP.getFreeHeap());
  // BT mode NEVER touches WiFi (Phúc's call, 12/9): WiFi and BT share the same
  // 2.4GHz controller on this board, and a BT stack started in a session that
  // used WiFi is not discoverable (the phone just times out). Only SD boots
  // fetch the time; BT reuses the epoch persisted by the last SD session, so
  // its clock can drift a little - the accepted trade for a working BT.
  if (!bootModeBt) bootTimeSynced = bootTimeSync();
  else             Serial.println("[BT] bluetooth-only boot: WiFi untouched, clock from NVS");
  if (bootModeBt) runBtModeSetup();
  else            runSdModeSetup();
}

// ── Shared boot step: get the real time BEFORE the mode starts ─────────────
// Phúc (12/9): every boot fetches the time once, so the clock is right in SD
// *and* BT mode. With no saved network the SD WiFi UI (list + keyboard) runs
// right here, in the boot phase, so a fresh device is configured without having
// to enter SD mode first.
static bool bootWifiConnectSaved() {
  drawStartupScreen();
  tft.setTextSize(1);
  tft.setTextColor(colInfoCyan(), COL_BG);
  char line[52];
  snprintf(line, sizeof(line), "WiFi: %.20s", cfgSSID);
  tft.setCursor((SCR_W - 6*(int)strlen(line))/2, 208);
  tft.print(line);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(cfgSSID, cfgPass);
  unsigned long t0 = millis();
  bool connected = false;
  while (millis() - t0 < 12000) {
    if (WiFi.status() == WL_CONNECTED) { connected = true; break; }
    delay(40);
  }
  tft.fillRect(0, 224, SCR_W, 14, COL_BG);
  const char* st = connected ? "Fetching time (NTP) ..." : "WiFi unreachable";
  tft.setTextColor(connected ? colInfoCyan() : TFT_RED, COL_BG);
  tft.setCursor((SCR_W - 6*(int)strlen(st))/2, 226);
  tft.print(st);
  if (connected) {
    bool synced = ntpSyncThenRadioOff(9000);
    tft.fillRect(0, 224, SCR_W, 14, COL_BG);
    const char* r = synced ? "Time OK (GMT+7)" : "WiFi ok - no NTP reply";
    tft.setTextColor(synced ? colInfoCyan() : TFT_RED, COL_BG);
    tft.setCursor((SCR_W - 6*(int)strlen(r))/2, 226);
    tft.print(r);
    delay(900);                    // release the radio before any BT start
    return true;
  }
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  return false;
}

/** Boot-phase WiFi setup: the SD list/keyboard, driven from setup(). Returns
 *  true once the link is up and the time has been fetched. */
static bool bootWifiWizard() {
  bootWifiSkip = false;
  screenMode = SCREEN_WIFI_LIST;
  wakeBackScreen = SCREEN_WIFI_LIST;
  wifiScroll = 0;
  startWifiScan();
  drawWifiList();
  unsigned long started = millis();
  while (millis() - started < 180000UL) {          // never hang a boot
    updateWifiScanResult();
    if (WiFi.status() == WL_CONNECTED) {
      drawStartupScreen();
      tft.setTextSize(1);
      tft.setTextColor(colInfoCyan(), COL_BG);
      char l2[52];
      snprintf(l2, sizeof(l2), "WiFi: %.20s", cfgSSID);
      tft.setCursor((SCR_W - 6*(int)strlen(l2))/2, 208); tft.print(l2);
      wifiConnecting = false;
      const char* st2 = "Fetching time (NTP) ...";
      tft.setCursor((SCR_W - 6*(int)strlen(st2))/2, 226); tft.print(st2);
      bool synced2 = ntpSyncThenRadioOff(9000);
      tft.fillRect(0, 224, SCR_W, 14, COL_BG);
      const char* r2 = synced2 ? "Time OK (GMT+7)" : "WiFi ok - no NTP reply";
      tft.setTextColor(synced2 ? colInfoCyan() : TFT_RED, COL_BG);
      tft.setCursor((SCR_W - 6*(int)strlen(r2))/2, 226);
      tft.print(r2);
      delay(900);
      return true;
    }
    if (wifiConnecting && millis() - wifiConnStart > 12000) {
      wifiConnecting = false;
      WiFi.disconnect();
      screenMode = SCREEN_WIFI_LIST;
      startWifiScan();
      drawWifiList();
    }
    if (ts.touched()) {
      if (screenMode == SCREEN_WIFI_LIST) handleWifiListTouch();
      else if (screenMode == SCREEN_KEYBOARD) handleKeyboardTouch();
      if (bootWifiSkip) return false;
    }
    delay(12);
  }
  return WiFi.status() == WL_CONNECTED;
}

static bool bootTimeSync() {
  bool connected;
  if (cfgSSID[0]) {
    connected = bootWifiConnectSaved();
  } else {
    bootWifiPhase = true;
    connected = bootWifiWizard();
    bootWifiPhase = false;
  }
  Serial.printf("[BOOT] wifi=%d epoch=%ld\n", connected ? 1 : 0, (long)time(nullptr));
  return connected;
}

static void runSdModeSetup() {
  // SD mode always boots at its default volume (9, Phúc 12/9), never the
  // BT 100%.
  volumePercent = 10;
  prefs.begin(NS_CFG, false); prefs.putInt("vol", 10); prefs.end();
  // display + touch init now lives in setup(): the boot WiFi phase needs it

  // audio out (I2S -> MAX98357A)
  audioOut = new I2SOutTap();
  audioOut->SetPinout(I2S_BCLK, I2S_LRCK, I2S_DOUT);
  audioOut->SetRate(44100);
  audioOut->SetChannels(2);
  // SetUseAPLL() REMOVED for SD playback (12/9): it was the only functional
  // change on the SD path when "one track ends -> board reboots" appeared,
  // so this build bisects it. Re-add only once the reboot is proven
  // unrelated AND exact 44.1 kHz is wanted here.
  audioOut->begin();
  audioOut->gainNow = 0.0f;

  // Bluetooth sink callbacks - registered once, the stack is only started
  // (never restarted) the first time the user opens Bluetooth mode.
  btSink.set_avrc_connection_state_callback(btAvrcConnCb);
  btSink.set_stream_reader(btDataCb, false);
  btSink.set_raw_stream_reader(btRawCb);     // SPEED measurement (decoded PCM)

  // dedicated BT->I2S writer on core 0 (proven drop-free in the standalone
  // test): keeps the BT stream fed regardless of what core 1 is doing
  xTaskCreatePinnedToCore(btAudioTask, "btAudio", 4096, nullptr, 8, nullptr, 0);
  applyVolumePercent();

  // SD must be mounted before the startup chime: /sounds/boot.wav is the
  // preferred source, the built-in procedural chime is the fallback.
  if (!SD.begin(SD_CS)) {
    tft.setTextColor(TFT_RED,COL_BG);
    tft.setTextSize(2);
    tft.setCursor(20,90);
    tft.print("SD Failed!");
    while (1) { delay(1000); pollBootButton(); }
  }
  sdReady = true;                    // /sounds/*.wav may be used from here on
  playJingleBlocking(JINGLE_BOOT);   // startup chime (fixed 20% gain)

  drawStartupScreen();

  // WiFi: ALWAYS try the saved network at boot (radio state in NVS is
  // ignored here). On success the time is fetched then the radio turns
  // itself off. If there is no saved network or it fails, the scan list
  // opens so the user can pick & type a network.
  bool haveSaved = cfgSSID[0];
  bool connected = bootTimeSynced;   // shared boot step already did it
  if (haveSaved && !bootTimeSynced) {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(cfgSSID, cfgPass);
    unsigned long t0 = millis();
    while (millis()-t0 < 8000) {
      if (WiFi.status()==WL_CONNECTED) { connected=true; break; }
      delay(40);
    }
    if (!connected) WiFi.disconnect();
  } else if (!haveSaved) {
    // No saved network: init the radio here for the scan list below.
    // NOTE: this branch must be gated on !haveSaved too. With the boot-phase
    // sync in place, `haveSaved && bootTimeSynced` also lands in the else, and
    // WiFi.mode(WIFI_STA)+setSleep(false) then re-awakened the radio the boot
    // phase had just powered off - the radio stayed awake through SD playback
    // (extra current on a board that browns out easily, plus RF bursts over the
    // SD card and I2S), which showed up as "SD mode won't play + screen
    // flickering". The radio must stay OFF after the boot sync.
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
  }
  if (connected && !bootTimeSynced) {
    ntpSyncThenRadioOff(6000);   // get time + timezone, then radio off
  }
  if (bootTimeSynced) {
    // Belt & braces before playback: the boot phase already fetched the time,
    // so the radio MUST be down here (never awake while the amp + SD card run).
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    wifiScanDone = true;
  }

  scanSD();
  if (trackCount == 0) {
    tft.setTextColor(TFT_YELLOW,COL_BG);
    tft.setTextSize(2);
    tft.setCursor(20,120);
    tft.print("No music");
    while (1) { delay(500); pollBootButton(); }
  }
  scanAlbums();
  browseLevel=BROWSE_ALBUMS;
  browseTrackScroll=0;
  albumScroll=0;
  if (connected) {
    // time synced + radio auto-off -> open the standby clock first
    // (tap anywhere to wake into the album browser)
    screenMode = SCREEN_BROWSER;   // enterClockScreen remembers this as wake-back
    enterClockScreen();
  } else {
    // No saved network or connect failed: open the scan list so the user can
    // pick a network and type its password (time syncs after connect, then
    // the radio turns itself off).
    screenMode=SCREEN_WIFI_LIST;
    wifiScroll=0;
    wakeBackScreen=SCREEN_BROWSER;
    startWifiScan();     // radio already initialised above -> scan should succeed
    drawWifiList();
  }

  lastUserActivityMs=millis();
}

static void loopSdMode() {
  pollBootButton();
  updateDisplayTimeout();
  updateWifiScanResult();
  updateWifiConnecting();

  if (clockForceRedraw && screenMode==SCREEN_CLOCK) drawClockScreen();

  // periodic UI updates on player
  if (displayBacklightOn && screenMode==SCREEN_PLAYER &&
      (playerState==STATE_PLAYING || playerState==STATE_PAUSED)) {
    unsigned long now=millis();
    if (now-lastProgressUiMs>=450) {
      lastProgressUiMs=now;
      drawPlayerProgressArea();
      audioPumpDecode(64);
    }
    updateVisualizerAnimation();
  }
  // idle clock: repaint seconds each second; full redraw once a minute
  if (screenMode==SCREEN_CLOCK) {
    static unsigned long lastClockTick=0;
    static unsigned long lastClockMin=0;
    unsigned long now=millis();
    if (now-lastClockTick>=1000) { lastClockTick=now; updateClockSeconds(); }
    if (now-lastClockMin>=60000) { lastClockMin=now; clockForceRedraw=true; }
  }
  // persist epoch every ~60s so clock survives reboot without NTP
  {
    static unsigned long lastEpochSave=0;
    unsigned long now=millis();
    if (now-lastEpochSave>=60000) { lastEpochSave=now; clockPersist(); }
  }

  // decode while playing
  if (playerState==STATE_PLAYING) {
    bool alive=false;
    if (currentType==AUDIO_MP3 && mp3) alive=mp3->isRunning();
    else if (currentType==AUDIO_WAV && wav) alive=wav->isRunning();
    if (alive) {
      bool ended=false;
      int loops=0;
      while (loops<256) {
        bool ok=false;
        if (currentType==AUDIO_MP3 && mp3 && mp3->isRunning()) ok=mp3->loop();
        else if (currentType==AUDIO_WAV && wav && wav->isRunning()) ok=wav->loop();
        if (!ok) { ended=true; break; }   // loop()==false = EOF/error reached
        loops++;
      }
      if (ended) {
        // ESP8266Audio: loop() returns false at EOF but isRunning() stays true
        // until stop() is called — without this auto-advance never triggers.
        if (mp3 && mp3->isRunning()) mp3->stop();
        if (wav && wav->isRunning()) wav->stop();
      }
    }
    bool runningNow=false;
    if (currentType==AUDIO_MP3 && mp3) runningNow=mp3->isRunning();
    else if (currentType==AUDIO_WAV && wav) runningNow=wav->isRunning();
    if (!runningNow) {
      // decoder finished -> advance after short silent gap (pop-safe)
      if (trackCount>0 && allowAutoAdvance()) {
        Serial.printf("[ADV] eof track=%d heap=%u\n", currentTrack, (unsigned)ESP.getFreeHeap());
        pumpSilence(32);
        nextTrack(true);
        if (screenMode==SCREEN_PLAYER && displayBacklightOn) drawPlayer();
      } else {
        // corrupt files or end of list: stop gracefully, keep silence fed
        stopTrack(true);
        pumpSilence(16);
        if (screenMode==SCREEN_PLAYER && displayBacklightOn) drawPlayer();
      }
    }
  } else if (playerState==STATE_PAUSED && !btModeActive) {
    // keep I2S fed with silence to avoid pops
    pumpSilence(24);
  } else if (playerState==STATE_STOPPED && !btModeActive) {
    pumpSilence(8);
  }

  handleTouch();
}

void loop() {
  if (bootModeBt) loopBtMode();
  else            loopSdMode();
}
