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
static bool wifiRadioOn = true;   // user can power the radio off after NTP sync
static bool wifiNeedNtp = false;  // re-sync time after radio is turned back on
static unsigned long wifiBgStartMs = 0;  // when background reconnect began

// ── Bluetooth A2DP Sink state (phone -> CYD-32-BP -> MAX98357A) ──
static BtSink btSink;
static bool bootModeBt = false;    // NVS "bootmode"=="bt": clean BT-only boot
static bool btModeActive = false;   // BT status screen visible
static bool btStackStarted = false; // BT stack started once (never re-start!)
static bool btConnected  = false;   // A2DP link up
static volatile bool btStreaming = false; // PCM frames in the ring
static bool btUiDirty    = false;   // redraw BT screen from main loop
static char btPeerName[33]  = "";
static int  btPhonePct = -1;   // last phone AVRCP volume in %, -1 = unknown yet
#define BT_ROW_VOL_Y 150       // volume control row
#define BT_ROW_BRI_Y 210       // brightness control row
#define BT_CTRL_L 18           // minus button x0
#define BT_CTRL_LW 44
#define BT_CTRL_BAR 70         // % display x0
#define BT_CTRL_BARW 100
#define BT_CTRL_PLUS 176       // plus button x0
#define BT_CTRL_H 44
static unsigned long lastBtDataMs = 0;

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
static bool mainUiReady = false;

static void noteUserActivity() {
  lastUserActivityMs = millis();
  if (screenClockActive) { screenClockActive = false; clockForceRedraw = true; }
}

static void pollBootButton();
static void redrawCurrentScreen();
static void enterClockScreen();
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

  bool ConsumeSample(int16_t sample[2]) override {
    // fade toward target
    if (fading) {
      float target = mutePending ? 0.0f : vol;
      gainNow += (target - gainNow) * 0.25f;
      if (fabsf(target - gainNow) < 0.002f) {
        gainNow = target;
        fading = false;
      }
    }
    // mono mix (MAX98357A is a mono amp; send same mix to both I2S channels)
    int32_t m = (int32_t)(((int32_t)sample[0] + (int32_t)sample[1]) * gainNow * 0.5f);
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
static int browseAlbumIdx = -1;
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

static void freePlaylist() {
  for (int i = 0; i < trackCount; i++) { free(playlist[i]); playlist[i] = nullptr; }
  trackCount = 0;
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
                  SCREEN_BT };
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
  if (ep >= 1600000000) {
    time_t est = (time_t)ep + (time_t)((millis() - savedMs) / 1000ul);
    if (est >= 1600000000) return est;
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
static const int SET_ROW_X=8, SET_ROW_W=224, SET_ROW_H=38, SET_ROW_Y0=40, SET_ROW_GAP=5;
static int settingsScroll = 0;   // not used, single page
static void drawSettings() {
  tft.fillScreen(COL_BG);
  tft.fillRect(0,0,SCR_W,30,COL_BTN);
  tft.setTextSize(2);
  tft.setTextColor(COL_TEXT,COL_BTN);
  tft.setCursor(10,6);
  tft.print("Settings");
  tft.setTextSize(1);
  tft.setTextColor(colInfoCyan(),COL_BTN);
  tft.setCursor(170,10);
  tft.print("< back");

  int y=SET_ROW_Y0;
  // Invert row
  tft.fillRoundRect(SET_ROW_X,y,SET_ROW_W,SET_ROW_H,8,COL_BTN);
  tft.setTextSize(1); tft.setTextColor(COL_TEXT,COL_BTN);
  tft.setCursor(18,y+8); tft.print("Display invert");
  tft.fillRoundRect(150,y+10,66,24,12,cfgInvert?tft.color565(88,190,245):tft.color565(70,70,70));
  tft.setTextColor(cfgInvert?COL_BG:COL_DIM,cfgInvert?tft.color565(88,190,245):tft.color565(70,70,70));
  tft.setCursor(166,y+16); tft.print(cfgInvert?"ON":"OFF");
  y+=SET_ROW_H+SET_ROW_GAP;
  // Bluetooth mode row (auto SD / enter BT status screen)
  tft.fillRoundRect(SET_ROW_X,y,SET_ROW_W,SET_ROW_H,8,COL_BTN);
  tft.setTextSize(1); tft.setTextColor(COL_TEXT,COL_BTN);
  tft.setCursor(18,y+8); tft.print("Bluetooth mode");
  tft.setTextColor(btConnected?tft.color565(60,200,90):COL_DIM,COL_BTN);
  tft.setCursor(118,y+8); tft.print(btConnected?"connected":"SD mode");
  tft.setTextColor(COL_DIM,COL_BTN); tft.setCursor(204,y+11); tft.print(">");
  y+=SET_ROW_H+SET_ROW_GAP;
  // Brightness row
  tft.fillRoundRect(SET_ROW_X,y,SET_ROW_W,SET_ROW_H,8,COL_BTN);
  tft.setTextColor(COL_TEXT,COL_BTN); tft.setCursor(18,y+8); tft.print("Brightness");
  tft.fillRoundRect(158,y+7,26,30,6,tft.color565(45,45,52));
  tft.setTextColor(COL_TEXT,tft.color565(45,45,52)); tft.setCursor(165,y+13); tft.print("-");
  tft.setTextColor(COL_TEXT,COL_BTN);
  char bbuf[8]; snprintf(bbuf,sizeof(bbuf),"%d%%",cfgBright);
  tft.setCursor(SCR_W/2-9,y+8); tft.print(bbuf);
  tft.fillRoundRect(206,y+7,26,30,6,tft.color565(45,45,52));
  tft.setTextColor(COL_TEXT,tft.color565(45,45,52)); tft.setCursor(213,y+13); tft.print("+");
  y+=SET_ROW_H+SET_ROW_GAP;
  // WiFi row: left = status/change network, right = radio ON/OFF toggle
  tft.fillRoundRect(SET_ROW_X,y,SET_ROW_W,SET_ROW_H,8,COL_BTN);
  tft.setTextColor(COL_TEXT,COL_BTN); tft.setCursor(18,y+8); tft.print("WiFi");
  if (!wifiRadioOn) {
    tft.setTextColor(COL_DIM,COL_BTN);
    tft.setCursor(60,y+8); tft.print("Radio off");
  } else if (WiFi.status()==WL_CONNECTED && cfgSSID[0]) {
    tft.setTextColor(tft.color565(60,200,90),COL_BTN);
    tft.setCursor(60,y+8);
    char ss[14]; strncpy(ss,cfgSSID,13); ss[13]='\0';
    tft.print(ss);
  } else {
    tft.setTextColor(COL_DIM,COL_BTN);
    tft.setCursor(60,y+8); tft.print("Connecting..");
  }
  // toggle pill
  uint16_t onCol = wifiRadioOn ? tft.color565(88,190,245) : tft.color565(70,70,70);
  tft.fillRoundRect(150,y+10,66,24,12,onCol);
  tft.setTextColor(wifiRadioOn?COL_BG:COL_DIM, onCol);
  tft.setCursor(166,y+16); tft.print(wifiRadioOn?"ON":"OFF");
  y+=SET_ROW_H+SET_ROW_GAP;
  // Recalibrate touch row
  tft.fillRoundRect(SET_ROW_X,y,SET_ROW_W,SET_ROW_H,8,COL_BTN);
  tft.setTextColor(COL_TEXT,COL_BTN); tft.setCursor(18,y+8); tft.print("Recalibrate touch");
  tft.setTextColor(COL_DIM,COL_BTN); tft.setCursor(150,y+16); tft.print("run 4 taps");
  y+=SET_ROW_H+SET_ROW_GAP;
  // Clock view row (tap to preview the idle clock screen)
  tft.fillRoundRect(SET_ROW_X,y,SET_ROW_W,SET_ROW_H,8,COL_BTN);
  tft.setTextColor(COL_TEXT,COL_BTN); tft.setCursor(18,y+8); tft.print("Clock");
  tft.setTextColor(COL_DIM,COL_BTN); tft.setCursor(150,y+16); tft.print("view now");
  y+=SET_ROW_H+SET_ROW_GAP;
  // Clock note
  tft.setTextSize(1);
  tft.setTextColor(COL_DIM,COL_BG);
  tft.setCursor(12,y+2);
  tft.print("Idle: 5 min no touch -> clock");
}

// ---- WiFi list ----
#define WIFI_MAX 14
static char wifiNames[WIFI_MAX][33];
static int wifiRssi[WIFI_MAX];
static bool wifiLocked[WIFI_MAX];
static int wifiCount=0, wifiScroll=0;
static bool wifiScanDone=false;
static int wifiScanRetries=0;
static unsigned long wifiScanStartedAt=0;
static int kbTargetSSIDIdx = -1;

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
  tft.setCursor(176,10); tft.print("< back");
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
  int vis=8, y0=42, itemH=30;
  for (int i=0;i<vis;i++){
    int idx=wifiScroll+i;
    if (idx>=wifiCount) break;
    int y=y0+i*itemH;
    tft.fillRoundRect(6,y,228,itemH-4,6,COL_DIR);
    tft.setTextSize(1);
    char line[40];
    snprintf(line,sizeof(line),"%.24s",wifiNames[idx]);
    tft.setTextColor(COL_TEXT,COL_DIR);
    tft.setCursor(14,y+10); tft.print(line);
    if (wifiLocked[idx]) {
      tft.setTextColor(colInfoCyan(),COL_DIR);
      tft.setCursor(202,y+10); tft.print("*");
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
  // header
  tft.fillRect(0,0,SCR_W,82,COL_BTN);
  tft.setTextSize(1);
  tft.setTextColor(colInfoCyan(),COL_BTN);
  tft.setCursor(10,4); tft.print(kbSSIDName);
  tft.setTextColor(COL_DIM,COL_BTN);
  tft.setCursor(10,22); tft.print("Password:");
  tft.setTextColor(COL_YEL,COL_BTN);
  char dots[34]; int n=kbCur; if (n>32) n=32;
  for (int i=0;i<n;i++) dots[i]='*';
  dots[n]='\0';
  tft.setCursor(10,40); tft.print(dots);
  tft.setTextColor(COL_DIM,COL_BTN);
  tft.setCursor(10,58); tft.print(kbPass);
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
static char lastErrMsg[40]="";

static void saveWifiCreds(const char* ssid, const char* pass) {
  strncpy(cfgSSID,ssid,32); cfgSSID[32]='\0';
  strncpy(cfgPass,pass,64); cfgPass[64]='\0';
  prefs.begin(NS_CFG,false);
  prefs.putString("ssid",cfgSSID);
  prefs.putString("pass",cfgPass);
  prefs.end();
}

static bool tryConnectSavedWifi() {
  if (!cfgSSID[0]) return false;
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfgSSID, cfgPass);
  unsigned long t0=millis();
  while (millis()-t0 < 9000) {
    if (WiFi.status()==WL_CONNECTED) return true;
    audioPumpDecode(32);
    delay(50);
    tft.setTextSize(1); tft.setTextColor(COL_DIM,COL_BG);
    tft.setCursor(20,150); tft.print("Connecting WiFi...");
    yield();
  }
  WiFi.disconnect();
  return false;
}

static void startNtp() {
  configTime(7*3600, 0, "pool.ntp.org", "time.nist.gov");
}

// Boot rule: WiFi is only used to fetch the time. Once NTP has answered
// (or a timeout passes) the radio is powered fully off again.
static void ntpSyncThenRadioOff(unsigned long maxWaitMs) {
  startNtp();
  unsigned long t0 = millis();
  time_t now = time(nullptr);
  while (millis()-t0 < maxWaitMs && now < 1700000000L) { delay(40); now = time(nullptr); }
  delay(120);                       // let configTime settle one tick
  if (now >= 1700000000L) clockPersist();  // save epoch so clock survives reboot
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  wifiRadioOn = false;
  wifiConnecting = false;
  wifiScanDone = true;
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
    strncpy(lastErrMsg,"WiFi failed",sizeof(lastErrMsg)-1);
    screenMode = SCREEN_WIFI_LIST;
    startWifiScan();
    drawWifiList();
    return;
  }
}

// ── WiFi radio power (user toggle: off saves power/interference after NTP) ──
static void setWifiRadio(bool on) {
  wifiRadioOn = on;
  prefs.begin(NS_CFG, false);
  prefs.putBool("wifiOn", on);
  prefs.end();
  if (on) {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    if (cfgSSID[0]) {
      // saved network: reconnect quietly (no screen jump if we were in settings)
      wifiNeedNtp = true;
      wifiBgStartMs = millis();
      WiFi.begin(cfgSSID, cfgPass);
    } else {
      // no saved network -> open scan list
      wakeBackScreen = SCREEN_SETTINGS;
      screenMode = SCREEN_WIFI_LIST;
      wifiScroll = 0;
      startWifiScan();
      drawWifiList();
    }
  } else {
    // power the radio fully off
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    wifiConnecting = false;
    wifiScanDone = true;
  }
}

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
static volatile uint32_t btTotal = 0;
static int16_t btChunk[1024 * 2];    // audio task transfers in big chunks

static void drawBtScreen();
static void handleBtTouch();
static void exitBtMode();
static bool btHandleCtrl(int16_t tx, int16_t ty);

static void btUiWake() { btUiDirty = true; }

static void btAvrcConnCb(bool connected) {
  if (!connected) { btPeerName[0] = 0; btPhonePct = -1; }
  btUiWake();
}

// Phone volume event (0..127) from AVRCP. The lib already applied the
// phone's own scaling to the stream (volume_set_by_controller runs before
// this callback) - so the phone side is INDEPENDENT: we only read its level
// for the on-screen readout and never touch the ESP gain from here.
static void btVolumeCb(int v) {
  btPhonePct = (constrain(v,0,127) * 100 + 63) / 127;
  btUiWake();
}

static void btDataCb(const uint8_t* data, uint32_t len) {
  if (!btModeActive || !data) return;
  lastBtDataMs = millis();
  if (!btStreaming) btStreaming = true;
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
  btTotal += n;
}

// Dedicated BT audio writer: pinned to core 0 with high priority so the
// phone stream never starves against the SD/UI loop on core 1. Volume is
// applied by the shared I2SOutTap (same volumePercent as SD mode).
static void btAudioTask(void*) {
  while (true) {
    if (!btModeActive) { vTaskDelay(20); continue; }
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

static void drawBtScreen() {
  btUiDirty = false;
  tft.fillScreen(COL_BG);
  // header
  tft.fillRect(0,0,SCR_W,30,colTopBarBg());
  tft.drawFastHLine(0,29,SCR_W,tft.color565(40,40,48));
  tft.setTextSize(2); tft.setTextColor(COL_TEXT,colTopBarBg());
  tft.setCursor(10,6); tft.print("Bluetooth mode");
  tft.setTextSize(1); tft.setTextColor(colInfoCyan(),colTopBarBg());
  tft.setCursor(196,12); tft.print("< SD");

  // status zone (waiting or connected)
  bool conn = btConnected;
  drawBtRune(120, 58, conn ? tft.color565(60,200,90) : colInfoCyan());
  const char* nm = (conn && btPeerName[0]) ? btPeerName : "CYD-32-BP";
  btCenterLine(nm, 84, 2, COL_TEXT, COL_BG);
  btCenterLine(conn ? "connected" : "waiting for phone ...", 118, 1,
               conn ? tft.color565(60,200,90) : COL_DIM, COL_BG);
  if (conn) {
    char sline[48];
    snprintf(sline, sizeof(sline), "device: %s", btPeerName[0] ? btPeerName : "Phone");
    btCenterLine(sline, 132, 1, COL_DIM, COL_BG);
  }

  // volume control row (works on the ESP, syncs with the phone)
  tft.fillRoundRect(BT_CTRL_L, BT_ROW_VOL_Y, BT_CTRL_LW, BT_CTRL_H, 7, COL_BTN);
  tft.setTextSize(2); tft.setTextColor(COL_TEXT, COL_BTN);
  tft.setCursor(BT_CTRL_L + BT_CTRL_LW/2 - 6, BT_ROW_VOL_Y + 14); tft.print("-");
  tft.fillRoundRect(BT_CTRL_BAR, BT_ROW_VOL_Y, BT_CTRL_BARW, BT_CTRL_H, 7, COL_BTN);
  char vbuf[8]; snprintf(vbuf,sizeof(vbuf),"%d%%",volumePercent);
  tft.setTextColor(colInfoCyan(), COL_BTN);
  tft.setCursor(BT_CTRL_BAR + BT_CTRL_BARW/2 - (int)strlen(vbuf)*6, BT_ROW_VOL_Y + 14);
  tft.print(vbuf);
  tft.fillRoundRect(BT_CTRL_PLUS, BT_ROW_VOL_Y, BT_CTRL_LW, BT_CTRL_H, 7, COL_BTN);
  tft.setTextColor(COL_TEXT, COL_BTN);
  tft.setCursor(BT_CTRL_PLUS + BT_CTRL_LW/2 - 6, BT_ROW_VOL_Y + 14); tft.print("+");
  tft.setTextSize(1);
  if (conn && btPhonePct >= 0) {
    char pl[40]; snprintf(pl, sizeof(pl), "ESP %d%%   Phone %d%%", volumePercent, btPhonePct);
    btCenterLine(pl, BT_ROW_VOL_Y + BT_CTRL_H + 8, 1, COL_DIM, COL_BG);
  } else if (conn) {
    btCenterLine("ESP volume (phone sync chua nhan)", BT_ROW_VOL_Y + BT_CTRL_H + 8, 1, COL_DIM, COL_BG);
  } else {
    btCenterLine("Volume ESP (chua ket noi)", BT_ROW_VOL_Y + BT_CTRL_H + 8, 1, COL_DIM, COL_BG);
  }

  // brightness control row
  tft.fillRoundRect(BT_CTRL_L, BT_ROW_BRI_Y, BT_CTRL_LW, BT_CTRL_H, 7, COL_BTN);
  tft.setTextSize(2); tft.setTextColor(COL_TEXT, COL_BTN);
  tft.setCursor(BT_CTRL_L + BT_CTRL_LW/2 - 6, BT_ROW_BRI_Y + 14); tft.print("-");
  tft.fillRoundRect(BT_CTRL_BAR, BT_ROW_BRI_Y, BT_CTRL_BARW, BT_CTRL_H, 7, COL_BTN);
  char bbuf[8]; snprintf(bbuf,sizeof(bbuf),"%d%%",cfgBright);
  tft.setTextColor(colInfoCyan(), COL_BTN);
  tft.setCursor(BT_CTRL_BAR + BT_CTRL_BARW/2 - (int)strlen(bbuf)*6, BT_ROW_BRI_Y + 14);
  tft.print(bbuf);
  tft.fillRoundRect(BT_CTRL_PLUS, BT_ROW_BRI_Y, BT_CTRL_LW, BT_CTRL_H, 7, COL_BTN);
  tft.setTextColor(COL_TEXT, COL_BTN);
  tft.setCursor(BT_CTRL_PLUS + BT_CTRL_LW/2 - 6, BT_ROW_BRI_Y + 14); tft.print("+");
  tft.setTextSize(1);
  btCenterLine("Brightness", BT_ROW_BRI_Y + BT_CTRL_H + 8, 1, COL_DIM, COL_BG);
}

static void handleBtTouch() {
  int16_t tx,ty;
  if (!getTouchXY(tx,ty)) return;
  noteUserActivity();
  unsigned long now=millis();
  if (now-lastTouchTime<TOUCH_DEBOUNCE_MS) return;
  lastTouchTime=now;
  while (ts.touched()) { delay(1); if (millis()-now>400) break; }
  // back to SD
  if (ty<30 && tx>=150) { exitBtMode(); return; }
  if (btHandleCtrl(tx,ty)) drawBtScreen();
}

static void updateBtPoll() {
  if (!btModeActive) return;
  static unsigned long lastPoll=0;
  unsigned long now=millis();
  if (now-lastPoll<400) return;
  lastPoll=now;
  bool c = btSink.is_connected();
  if (c != btConnected) {
    btConnected = c;
    btPhonePct = -1;                 // unknown until the phone reports volume
    Serial.printf("[BT] conn=%d heap=%u drop=%u\n", c?1:0,
                  (unsigned)ESP.getFreeHeap(), (unsigned)btDropped);
    if (!c) btStreaming = false;
    btUiWake();
  }
  if (btConnected && now-lastBtDataMs>350) btStreaming = false;
  static bool lastStream=false;
  bool s=btStreaming;
  if (s!=lastStream) { lastStream=s; btUiWake(); }   // redraw on start/stop of audio
}

static void enterBtMode() {
  if (btModeActive) return;
  // stop SD playback (SD and BT share the same I2S output)
  if (playerState != STATE_STOPPED) stopTrack(true);
  // Bluetooth owns the 2.4GHz radio -> WiFi off, then let the controller
  // actually release it before the BT stack starts (avoids "can't pair").
  setWifiRadio(false);
  delay(600);
  // volume = our own gain, same percentage as SD mode
  audioOut->requestFade(false);
  applyVolumePercent();
  // start the BT stack exactly once - never end()/restart it afterwards.
  // WiFi MUST be fully off first: the 2.4G controller is shared and a
  // still-awake WiFi radio can stop the stack from becoming discoverable.
  if (!btStackStarted) {
    Serial.printf("[BT] start %s heap=%u\n", BT_DEVICE_NAME, (unsigned)ESP.getFreeHeap());
    btSink.start(BT_DEVICE_NAME);
    btStackStarted = true;
    delay(400);          // let it advertise before the UI says "waiting"
  }
  btModeActive = true;
  btConnected = btSink.is_connected();
  btPeerName[0] = 0;
  btUiDirty = true;
  screenMode = SCREEN_BT;
  wakeBackScreen = SCREEN_BROWSER;
  drawBtScreen();
}

static void exitBtMode() {
  if (!btModeActive) return;
  btModeActive = false;
  // disconnect the phone but KEEP the stack running (restarting crashes)
  btSink.disconnect();
  btConnected = false;
  btStreaming = false;
  audioOut->requestFade(false);
  applyVolumePercent();
  screenMode = SCREEN_BROWSER;
  wakeBackScreen = SCREEN_BROWSER;
  drawBrowser();
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
  int y=SET_ROW_Y0;
  // invert row
  if (ty>=y && ty<y+SET_ROW_H) { toggleInvert(); drawSettings(); return; }
  y+=SET_ROW_H+SET_ROW_GAP;
  // Bluetooth mode row -> reboot cleanly into Bluetooth-only mode
  if (ty>=y && ty<y+SET_ROW_H) { btSwitchToBt(); return; }
  y+=SET_ROW_H+SET_ROW_GAP;
  // brightness row: - / + / tap center steps
  if (ty>=y && ty<y+SET_ROW_H) {
    if (tx>=158 && tx<184) { setBrightnessPct(cfgBright-10); prefs.begin(NS_CFG,false); prefs.putInt("bright",cfgBright); prefs.end(); }
    else if (tx>=206 && tx<232) { setBrightnessPct(cfgBright+10); prefs.begin(NS_CFG,false); prefs.putInt("bright",cfgBright); prefs.end(); }
    drawSettings(); return;
  }
  y+=SET_ROW_H+SET_ROW_GAP;
  // wifi row: right side = radio toggle, left side = open scan list
  if (ty>=y && ty<y+SET_ROW_H) {
    if (tx>=150) {
      setWifiRadio(!wifiRadioOn);
      drawSettings();
      return;
    }
    if (!wifiRadioOn) {
      setWifiRadio(true);       // tapping status with radio off turns it on
      drawSettings();
      return;
    }
    wakeBackScreen=SCREEN_SETTINGS;   // back from list returns here
    screenMode=SCREEN_WIFI_LIST;
    wifiScroll=0;
    startWifiScan();
    drawWifiList();
    return;
  }
  y+=SET_ROW_H+SET_ROW_GAP;
  // recalibrate touch row
  if (ty>=y && ty<y+SET_ROW_H) {
    prefs.begin("cal2", false);
    prefs.clear();
    prefs.end();
    runCalibration();
    screenMode=SCREEN_SETTINGS;
    drawSettings();
    return;
  }
  y+=SET_ROW_H+SET_ROW_GAP;
  // clock row -> show idle clock screen (tap wakes back to settings)
  if (ty>=y && ty<y+SET_ROW_H) {
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
    screenMode = (wakeBackScreen==SCREEN_BROWSER || wakeBackScreen==SCREEN_PLAYER)
                 ? wakeBackScreen : SCREEN_SETTINGS;
    redrawCurrentScreen();
    return;
  }
  if (!wifiScanDone) return;
  if (ty>=290) {
    if (tx<80) { startWifiScan(); drawWifiList(); return; }       // rescan
    if (tx>=180 && (wifiScroll+8)<wifiCount) { wifiScroll+=8; drawWifiList(); return; } // more
    return;
  }
  int idx=wifiScroll+(ty-42)/30;
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
  kbCur=0; kbPass[0]='\0'; kbShift=false;
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
    if (browseLevel==BROWSE_TRACKS && tx<64) { browseLevel=BROWSE_ALBUMS; browseAlbumIdx=-1; browseTrackScroll=0; drawBrowser(); return; }
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
    browseAlbumIdx=idx;
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
      else if (volumePercent == 5)        volumePercent = 10;
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
    case SCREEN_BT: handleBtTouch(); break;
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
  }
}

// ── Startup screen ─────────────────────────────────────────
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
  // Coming back to the SD player starts at its default volume (10), not
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

static void runBtModeSetup() {
  Serial.printf("[BT] boot bluetooth-only mode heap=%u\n", (unsigned)ESP.getFreeHeap());
  // BT mode always runs at full ESP gain: volume lives on the phone.
  volumePercent = 100;
  prefs.begin(NS_CFG, false); prefs.putInt("vol", 100); prefs.end();
  SPI.begin();
  touchSPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  ts.begin(touchSPI);
  ts.setRotation(0);
  loadCal();
  if (!calDone) runCalibration();

  audioOut = new I2SOutTap();
  audioOut->SetPinout(I2S_BCLK, I2S_LRCK, I2S_DOUT);
  audioOut->SetRate(44100);
  audioOut->SetChannels(2);
  audioOut->begin();
  audioOut->gainNow = 0.0f;

  btSink.set_avrc_connection_state_callback(btAvrcConnCb);
  btSink.set_avrc_rn_volumechange(btVolumeCb);
  btSink.set_stream_reader(btDataCb, false);
  xTaskCreatePinnedToCore(btAudioTask, "btAudio", 4096, nullptr, 8, nullptr, 0);
  applyVolumePercent();

  // No WiFi is ever started in this mode -> the radio is free, start now.
  btSink.start(BT_DEVICE_NAME);
  btStackStarted = true;
  btModeActive = true;        // audio task drains immediately
  delay(500);
  btConnected = btSink.is_connected();
  Serial.printf("[BT] started heap=%u\n", (unsigned)ESP.getFreeHeap());
  drawBtScreen();
}

// ESP volume buttons -> phone (AVRCP absolute volume, when the phone
// supports it: Android/Windows yes, iOS usually keeps its own level).
static void btNotifyPhoneVolume() {
  if (!btStackStarted || !btConnected) return;
  uint8_t v = (uint8_t)constrain((volumePercent * 127 + 50) / 100, 0, 127);
  btSink.notifyVolumeToPhone(v);
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
  // header "< SD" -> clean reboot back into the SD player
  if (ty < 30 && tx >= 150) { btSwitchToSd(); return; }
  if (btHandleCtrl(tx, ty)) drawBtScreen();
}

static void loopBtMode() {
  static unsigned long lastPoll = 0, lastConn = 999;
  unsigned long now = millis();
  if (now - lastPoll >= 300) {
    lastPoll = now;
    bool c = btSink.is_connected();
    if (c != lastConn) {
      lastConn = c;
      btConnected = c;
      if (!c) btStreaming = false;
      Serial.printf("[BT] conn=%d heap=%u drop=%u\n", c ? 1 : 0,
                    (unsigned)ESP.getFreeHeap(), (unsigned)btDropped);
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

void setup() {
  Serial.begin(115200);
  delay(300);

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
  volumePercent = prefs.getInt("vol", 10);   // SD default is 10
  wifiRadioOn = prefs.getBool("wifiOn", true);
  bootModeBt = (prefs.getString("bootmode", "sd") == "bt");
  String s = prefs.getString("ssid", "");
  String p = prefs.getString("pass", "");
  if (s.length()>0){ strncpy(cfgSSID,s.c_str(),32); cfgSSID[32]='\0'; }
  if (p.length()>0){ strncpy(cfgPass,p.c_str(),64); cfgPass[64]='\0'; }
  prefs.end();

  applyInvert();
  setBrightnessPct(cfgBright);

  // Two clean boot modes (no PSRAM: SD stack and BT stack never coexist).
  // Switching mode = save NVS flag + ESP.restart() into a clean boot.
  if (bootModeBt) runBtModeSetup();
  else            runSdModeSetup();
}

static void runSdModeSetup() {
  // SD mode always boots at its default volume (10), never the BT 100%.
  volumePercent = 10;
  prefs.begin(NS_CFG, false); prefs.putInt("vol", 10); prefs.end();
  SPI.begin();
  touchSPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  ts.begin(touchSPI);
  ts.setRotation(0);

  // Touch calibration (4-point affine, persisted). Runs once until user taps 4 targets.
  loadCal();
  if (!calDone) runCalibration();

  // audio out (I2S -> MAX98357A)
  audioOut = new I2SOutTap();
  audioOut->SetPinout(I2S_BCLK, I2S_LRCK, I2S_DOUT);
  audioOut->SetRate(44100);
  audioOut->SetChannels(2);
  audioOut->begin();
  audioOut->gainNow = 0.0f;

  // Bluetooth sink callbacks - registered once, the stack is only started
  // (never restarted) the first time the user opens Bluetooth mode.
  btSink.set_avrc_connection_state_callback(btAvrcConnCb);
  btSink.set_avrc_rn_volumechange(btVolumeCb);
  btSink.set_stream_reader(btDataCb, false);

  // dedicated BT->I2S writer on core 0 (proven drop-free in the standalone
  // test): keeps the BT stream fed regardless of what core 1 is doing
  xTaskCreatePinnedToCore(btAudioTask, "btAudio", 4096, nullptr, 8, nullptr, 0);
  applyVolumePercent();

  drawStartupScreen();

  // WiFi: ALWAYS try the saved network at boot (radio state in NVS is
  // ignored here). On success the time is fetched then the radio turns
  // itself off. If there is no saved network or it fails, the scan list
  // opens so the user can pick & type a network.
  bool haveSaved = cfgSSID[0];
  bool connected = false;
  if (haveSaved) {
    wifiRadioOn = true;
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(cfgSSID, cfgPass);
    unsigned long t0 = millis();
    while (millis()-t0 < 8000) {
      if (WiFi.status()==WL_CONNECTED) { connected=true; break; }
      delay(40);
    }
    if (!connected) WiFi.disconnect();
  } else {
    // radio init now; the scan list below does the actual scan
    wifiRadioOn = true;
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
  }
  if (connected) {
    ntpSyncThenRadioOff(6000);   // get time + timezone, then radio off
  }

  if (!SD.begin(SD_CS)) {
    tft.setTextColor(TFT_RED,COL_BG);
    tft.setTextSize(2);
    tft.setCursor(20,90);
    tft.print("SD Failed!");
    while (1) { delay(1000); pollBootButton(); }
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
  browseAlbumIdx=-1;
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
  mainUiReady=true;
}

static void loopSdMode() {
  pollBootButton();
  updateDisplayTimeout();
  updateWifiScanResult();
  updateWifiConnecting();
  // radio turned back on with saved network: re-sync clock quietly once connected
  if (wifiNeedNtp && WiFi.status()==WL_CONNECTED) {
    wifiNeedNtp = false;
    startNtp();
  }

  // Bluetooth mode: poll link state, redraw on change (audio is drained by
  // the dedicated core-0 btAudioTask started in setup())
  if (btModeActive) {
    updateBtPoll();
    if (btUiDirty) drawBtScreen();
  }

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
