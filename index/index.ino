
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <SPI.h>
#include <SD_MMC.h>
#include <FS.h>
#include <Update.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <math.h>
#include "hal/gpio_ll.h"

// PxMatrix only recognizes this lowercase name (or "double_buffer"). The old
// uppercase PxMATRIX_DOUBLE_BUFFER was silently ignored, so every redraw was
// drawn straight into the buffer being shown and the panel flickered.
#define PxMATRIX_double_buffer true
#include <PxMatrix.h>
#include "esp_system.h"
#include <Fonts/TomThumb.h>
#include <glcdfont.c>

// =========================
// Log (Serial tee)
// =========================
// Everything printed through Log goes to Serial and is also kept in a RAM ring
// buffer, so the web "Estado" page can show the same messages as the Serial
// Monitor (/api/log). Guarded by a spinlock: the weather task logs from core 0.
constexpr int LOG_LINES = 80;
constexpr int LOG_LINE_LEN = 120;
struct LogLine { uint32_t seq; uint32_t ms; char text[LOG_LINE_LEN]; };
portMUX_TYPE logMux = portMUX_INITIALIZER_UNLOCKED;

class LogTee : public Print {
 public:
  size_t write(uint8_t c) override {
    Serial.write(c);
    if (c == '\r') return 1;
    portENTER_CRITICAL(&logMux);
    if (c == '\n') commitLocked();
    else if (len < LOG_LINE_LEN - 1) cur[len++] = (char)c;
    portEXIT_CRITICAL(&logMux);
    return 1;
  }
  size_t write(const uint8_t* b, size_t n) override { for (size_t i = 0; i < n; i++) write(b[i]); return n; }
  uint32_t nextSeq() { portENTER_CRITICAL(&logMux); uint32_t v = seq; portEXIT_CRITICAL(&logMux); return v; }
  // Copies lines with seq >= since (oldest first) into out; returns how many.
  int snapshot(uint32_t since, LogLine* out, int max) {
    int n = 0;
    portENTER_CRITICAL(&logMux);
    uint32_t first = seq > (uint32_t)LOG_LINES ? seq - LOG_LINES : 0;
    if (since < first) since = first;
    for (uint32_t s = since; s < seq && n < max; s++) out[n++] = ring[s % LOG_LINES];
    portEXIT_CRITICAL(&logMux);
    return n;
  }
 private:
  void commitLocked() {
    LogLine& l = ring[seq % LOG_LINES];
    l.seq = seq; l.ms = millis();
    memcpy(l.text, cur, len); l.text[len] = 0;
    seq++; len = 0;
  }
  LogLine ring[LOG_LINES];
  char cur[LOG_LINE_LEN];
  int len = 0;
  uint32_t seq = 0;
};
LogTee Log;

// =========================
// WIFI
// =========================
const char* WIFI_SSID = "";
const char* WIFI_PASSWORD = "";
const char* MDNS_HOST = "matrix";

// =========================
// HUB75 / PxMatrix
// =========================
#define P_LAT   4
#define P_OE    22
#define P_A     23
#define P_B     19
#define P_C     21
#define P_D     5
#define P_E     33
#define P_CLK   18
#define P_MOSI  25
#define P_MISO  34
#define P_SS    26

constexpr uint16_t MATRIX_W = 64;
constexpr uint16_t MATRIX_H = 64;
constexpr size_t FRAME_BYTES = MATRIX_W * MATRIX_H * 2;

PxMATRIX display(MATRIX_W, MATRIX_H, P_LAT, P_OE, P_A, P_B, P_C, P_D, P_E);
WebServer server(80);

// >>> FACE TYPES
// =========================
// Clock faces: types
// =========================
// Declared before any function so the Arduino prototype generator can use them.
// A face draws into faceFb (64x64 RGB565). Its colors and options arrive as
// generic slots (FaceSettings.p[] / .o[]); the web UI knows what each slot
// means for each face (FACES list in app.js).
struct Paint { uint16_t a; uint16_t b; uint8_t dir; };  // dir: 0 solid, 1 vertical, 2 horizontal, 3 diagonal, 4 rainbow
constexpr int FACE_PAINTS = 10;
constexpr int FACE_OPTS = 8;
struct FaceSettings {
  char face[24];
  bool h24;
  uint8_t lang;   // 0 = es, 1 = en
  bool blink;     // blinking colon
  Paint p[FACE_PAINTS];
  uint8_t o[FACE_OPTS];
};
struct WxDay { bool valid; int code; float tmax; float tmin; float hum; };
struct FaceEnv {
  struct tm t;
  bool timeValid;
  bool night;
  bool wxValid;
  float temp;
  float hum;
  int code;
  WxDay d[4];     // 0 = today
  bool wifi;
  int rssi;
  const uint16_t* slot[3];  // 18x18 RGB565 pictures for the "tablero" face, may be null
};
struct Sprite { uint8_t w; uint8_t h; const char* keys; const uint16_t* pal; const char* px; };
// <<< FACE TYPES
// Declared by hand: the Arduino prototype generator stops at the embedded web
// assets (raw strings), so functions used before their definition need this.
void faceDefaults(FaceSettings& s);
bool loadClockCfg(FaceSettings& s);
void saveClockCfg();
void loadFaceSlots();
void saveLocation();
String jsonEsc(const String& in);

// Refresco del HUB75 en una tarea FreeRTOS dedicada.
// No usamos una ISR porque PxMatrix usa SPI internamente y ese camino
// no es seguro dentro de una interrupción en Arduino-ESP32 3.x.
TaskHandle_t matrixRefreshTaskHandle = nullptr;

constexpr uint32_t MATRIX_REFRESH_MS = 2;
constexpr uint8_t MATRIX_DRAW_TIME = 20;

void matrixRefreshTask(void* parameter) {
  TickType_t lastWake = xTaskGetTickCount();
  const TickType_t period = pdMS_TO_TICKS(MATRIX_REFRESH_MS);

  for (;;) {
    display.display(MATRIX_DRAW_TIME);

    vTaskDelayUntil(
      &lastWake,
      period > 0 ? period : 1
    );
  }
}

bool startMatrixRefreshTask() {
  if (matrixRefreshTaskHandle != nullptr) {
    return true;
  }

  BaseType_t result = xTaskCreatePinnedToCore(
    matrixRefreshTask,
    "matrixRefresh",
    4096,
    nullptr,
    3,
    &matrixRefreshTaskHandle,
    1
  );

  if (result != pdPASS) {
    matrixRefreshTaskHandle = nullptr;
    Log.println("ERROR: no se pudo crear tarea de refresco HUB75");
    return false;
  }

  Log.println("Refresh HUB75: tarea FreeRTOS cada 2 ms");
  return true;
}

uint8_t frameBuffer[FRAME_BYTES];

const char* WWW_DIR = "/www";
const char* IMAGE_DIR = "/gallery/images";
const char* ANIM_DIR = "/gallery/animations";
const char* GIF_DIR = "/gallery/gifs";
const char* REMOTE_DIR = "/gallery/remote";

File uploadFile;
size_t uploadBytes = 0;
bool uploadOK = true;

bool mdnsReady = false;
uint32_t lastWifiCheck = 0;

// animation
File animFile;
bool animationPlaying = false;
uint16_t animFrames = 0;
uint16_t animIndex = 0;
uint16_t animDelay = 100;
bool animPmaV2 = false;
uint32_t nextAnimAt = 0;

// OTA
bool otaSuccess = false;
String otaError;
uint32_t lastRenderMs = 0;


// clock / weather
struct ClockConfig {
  bool enabled = false;
  uint8_t mode = 0;
  bool hour24 = true;
  bool showSeconds = false;
  bool showDate = true;
  bool showTemp = true;
  bool showHumidity = true;
  bool showWeather = true;
  uint8_t brightness = 20;
  uint16_t bg = 0x0000;
  uint16_t primary = 0xFFFF;
  uint16_t secondary = 0x06BF;
  uint16_t accent = 0xFB66;
  uint16_t weatherColor = 0x7E7F;
};
ClockConfig clockCfg;
float weatherTempC = 0;
float weatherHumidityPct = 0;
float weatherFeelsC = 0;
int weatherCode = -1;
volatile bool weatherValid = false;
volatile bool forceWeatherRefresh = true;
TaskHandle_t weatherTaskHandle = nullptr;
// Weather forecast and location. Written by the weather task (core 0) and read
// by the clock (core 1): always copy them under wxMux.
struct LocationCfg { char name[48]; float lat; float lon; int32_t utcOffset; };
LocationCfg location = {"Puebla", 19.0414f, -98.2063f, -21600};
const char* LOCATION_PATH = "/config/location.json";
const char* LOCATION_TMP = "/config/location.tmp";
WxDay wxDays[4];
volatile bool weatherIsDay = true;
int wxSunriseMin = -1;             // today, minutes after local midnight
int wxSunsetMin = -1;
volatile bool utcOffsetPending = false;
int32_t pendingUtcOffset = -21600;
portMUX_TYPE wxMux = portMUX_INITIALIZER_UNLOCKED;

// Clock faces: the active face and its settings (saved in /config/clock.json).
FaceSettings faceCfg;
uint16_t faceSlots[3][18 * 18];    // user pictures of the "tablero" face
bool faceSlotOk[3] = {false, false, false};
time_t lastClockSecond = 0;
uint32_t previewHoldUntil = 0;     // while set, the panel shows an editor preview
const char* CLOCK_PATH = "/config/clock.json";
const char* CLOCK_TMP = "/config/clock.tmp";
const char* const LEGACY_FACES[5] = {"clasico-digital", "clasico-fecha", "clasico-clima", "clasico-analogico", "clasico-hibrido"};

// =========================
// Embedded bootstrap web
// =========================
const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="es">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
  <meta name="color-scheme" content="dark">
  <title>Matrix Studio 64</title>
  <link rel="stylesheet" href="/tailwind.css?v=57">
</head>
<body>
  <div id="app"></div>
  <div id="transferOverlay" class="overlay hidden" aria-live="polite">
    <div class="transfer-card">
      <div class="spinner"></div>
      <div><div id="transferTitle" class="font-bold">Transfiriendo…</div><div id="transferDetail" class="muted mt-1">Preparando</div></div>
      <div class="progress-track"><div id="transferProgress" class="progress-bar"></div></div>
    </div>
  </div>
  <div id="toastHost" class="toast-host"></div>
  <script src="/app.js?v=57"></script>
</body>
</html>)HTML";

const char TAILWIND_CSS[] PROGMEM = R"CSS(
:root{
  --bg:#050811;--panel:#0c1424;--panel2:#111c30;--panel3:#17243b;
  --border:#243552;--text:#f8fafc;--muted:#91a0b8;--accent:#ff6b35;
  --accent2:#ff8a5b;--green:#22c55e;--red:#ef4444;--cyan:#22d3ee;
  --shadow:0 24px 70px rgba(0,0,0,.34)
}
*{box-sizing:border-box}
html{background:var(--bg)}
body{margin:0;min-height:100vh;background:
radial-gradient(circle at 15% 0%,rgba(34,211,238,.08),transparent 28rem),
radial-gradient(circle at 90% 5%,rgba(255,107,53,.08),transparent 26rem),
var(--bg);color:var(--text);font-family:Inter,-apple-system,BlinkMacSystemFont,"Segoe UI",Arial,sans-serif}
button,a,[role=button],label[for],select,input[type=file]::file-selector-button{cursor:pointer}
button,a,.btn,.nav-btn,.file-button,.library-card{transition:transform .15s ease,background .15s ease,border-color .15s ease,color .15s ease,box-shadow .15s ease}
button:hover,.btn:hover,.file-button:hover{transform:translateY(-1px);filter:brightness(1.08)}
a:hover{color:#fff}
button:disabled{cursor:not-allowed;opacity:.45;transform:none}
.hidden{display:none!important}.font-bold{font-weight:800}.font-semibold{font-weight:650}.muted{color:var(--muted)}.text-sm{font-size:13px}.text-xs{font-size:12px}.mt-1{margin-top:4px}.mt-2{margin-top:8px}.mt-3{margin-top:12px}.mt-4{margin-top:16px}.mb-2{margin-bottom:8px}.mb-4{margin-bottom:16px}
.shell{max-width:1480px;margin:auto;padding:24px}
.topbar{display:flex;align-items:center;justify-content:space-between;gap:18px;margin-bottom:18px}
.brand h1{font-size:30px;margin:3px 0 0}.eyebrow{font-size:11px;letter-spacing:.12em;text-transform:uppercase;color:var(--muted)}
.pill{display:inline-flex;align-items:center;gap:8px;padding:8px 11px;border-radius:999px;background:#0d1728;border:1px solid var(--border);font-size:12px}
.dot{width:8px;height:8px;border-radius:50%;background:var(--green);box-shadow:0 0 12px var(--green)}
.workspace{display:grid;grid-template-columns:220px minmax(0,1fr);gap:20px}
.sidebar{position:sticky;top:16px;height:max-content;background:rgba(12,20,36,.78);backdrop-filter:blur(14px);border:1px solid var(--border);border-radius:20px;padding:10px;box-shadow:var(--shadow)}
.nav-btn{width:100%;border:0;background:transparent;color:#9aa9bf;padding:12px 13px;border-radius:12px;text-align:left;font-weight:750;margin:2px 0}
.nav-btn:hover{background:#13213a;color:#fff}.nav-btn.active{background:linear-gradient(135deg,#ff6b35,#ff864f);color:#07101d;box-shadow:0 10px 25px rgba(255,107,53,.16)}
.content{min-width:0}.page{display:none}.page.active{display:block}
.page-head{display:flex;justify-content:space-between;align-items:flex-end;gap:14px;margin-bottom:16px}.page-head h2{margin:0;font-size:24px}.page-head p{margin:4px 0 0;color:var(--muted);font-size:13px}
.grid-2{display:grid;grid-template-columns:minmax(0,1.08fr) minmax(320px,.92fr);gap:16px}
.grid-cards{display:grid;grid-template-columns:repeat(auto-fill,minmax(230px,1fr));gap:14px}
.card{background:linear-gradient(180deg,rgba(17,28,48,.95),rgba(10,18,33,.95));border:1px solid var(--border);border-radius:20px;padding:18px;box-shadow:var(--shadow)}
.card-title{font-size:17px;font-weight:800;margin-bottom:13px}.card-sub{font-size:12px;color:var(--muted);margin-top:-7px;margin-bottom:13px}
.input,.select,.textarea{width:100%;background:#050a14;color:#fff;border:1px solid #2d4162;border-radius:12px;padding:11px 12px;outline:none}
.input:focus,.select:focus,.textarea:focus,.rich-editor:focus{border-color:#ff7d4b;box-shadow:0 0 0 3px rgba(255,107,53,.12)}
label.field{display:block;font-size:12px;font-weight:750;color:#c7d3e4}
label.field>.input,label.field>.select,label.field>.textarea{margin-top:7px}
.row{display:flex;align-items:center;gap:10px}.row-wrap{display:flex;align-items:center;gap:9px;flex-wrap:wrap}.split{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.btn{border:1px solid #304563;background:#18263d;color:#fff;padding:10px 13px;border-radius:11px;font-weight:760}
.btn-primary{background:linear-gradient(135deg,var(--accent),var(--accent2));border-color:transparent;color:#07101d}.btn-green{background:#14532d;border-color:#166534}.btn-danger{background:#3f1820;border-color:#6f2530;color:#ffb6bf}.btn-ghost{background:transparent}
.file-native{position:absolute;opacity:0;pointer-events:none;width:1px;height:1px}.file-picker{display:flex;align-items:center;gap:12px;border:1px dashed #38506f;background:#08111f;padding:13px;border-radius:14px}.file-button{display:inline-flex;align-items:center;gap:7px;padding:10px 13px;border-radius:11px;background:#1c2b45;border:1px solid #344c70;font-weight:800}.file-name{min-width:0;color:#b9c6d8;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.canvas-wrap{display:flex;justify-content:center;align-items:center;min-height:420px;padding:14px;background:repeating-conic-gradient(#08101d 0 25%,#0b1423 0 50%) 50%/18px 18px;border:1px solid #253754;border-radius:16px}
.canvas-wrap canvas{width:min(600px,100%);aspect-ratio:1;image-rendering:pixelated;background:#000;border:1px solid #38516f;border-radius:12px;box-shadow:0 18px 60px rgba(0,0,0,.38)}
.pixel-canvas{touch-action:none}
.color-control{display:grid;grid-template-columns:72px 1fr;gap:8px;align-items:center}.color-control input[type=color]{width:72px;height:52px;border:1px solid #38516f;background:#060b14;border-radius:12px;padding:4px}
.toolbar{display:flex;align-items:center;gap:8px;flex-wrap:wrap}.selection-badge{padding:6px 9px;border-radius:999px;background:#102039;color:#a9bdd6;font-size:11px}
.rich-editor{min-height:180px;max-height:330px;overflow:auto;white-space:pre-wrap;word-break:break-word;background:#050a14;border:1px solid #2d4162;border-radius:14px;padding:14px;line-height:1.45;outline:none;font-family:monospace}
.rich-editor:empty:before{content:attr(data-placeholder);color:#5f718c}
.preview-panel{display:flex;flex-direction:column;gap:12px}
.library-card{border:1px solid var(--border);border-radius:16px;background:#0b1527;padding:12px}.library-card:hover{border-color:#48648a;background:#0f1d34;transform:translateY(-2px)}.library-card.active{border-color:#ff6b35;box-shadow:0 0 0 2px rgba(255,107,53,.45)}
.giphy-scroll{position:relative;max-height:360px;overflow-y:auto;overscroll-behavior:contain;border:1px solid var(--border);border-radius:12px;padding:8px;background:#070d18}.giphy-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(88px,1fr));gap:8px}.giphy-grid .muted{grid-column:1/-1;padding:8px}.giphy-more{display:flex;justify-content:center;padding-top:10px}.giphy-more .btn:empty,.giphy-more .hidden{display:none}.giphy-grid img{width:100%;aspect-ratio:1;object-fit:cover;border-radius:10px;border:2px solid transparent;background:#000;cursor:pointer}.giphy-grid img:hover{border-color:#48648a}.giphy-grid img.active{border-color:#ff6b35}.giphy-attr{margin-left:auto;font-size:11px;font-weight:800;letter-spacing:.06em;color:#9aa9bf}
.library-preview{width:100%;aspect-ratio:1;background:#000;border-radius:10px;image-rendering:pixelated}
.table{width:100%;border-collapse:collapse}.table th,.table td{text-align:left;padding:9px;border-bottom:1px solid #21324e;font-size:12px}.table th{color:#9eacc2}
dialog{border:1px solid var(--border);border-radius:18px;background:#0d1729;color:#fff;width:min(620px,calc(100% - 28px));padding:0;box-shadow:0 35px 120px rgba(0,0,0,.7)}dialog::backdrop{background:rgba(0,0,0,.68);backdrop-filter:blur(5px)}.dialog-head,.dialog-body,.dialog-foot{padding:16px 18px}.dialog-head{border-bottom:1px solid var(--border);font-weight:850;font-size:18px}.dialog-foot{border-top:1px solid var(--border);display:flex;justify-content:flex-end;gap:8px}
.toast-host{position:fixed;right:20px;bottom:20px;z-index:100;display:flex;flex-direction:column;gap:8px}.toast{background:#101c30;border:1px solid #2d4263;border-radius:12px;padding:11px 13px;box-shadow:var(--shadow);min-width:240px;animation:toastin .18s ease}.toast.ok{border-color:#1e6c42}.toast.err{border-color:#7f2935}@keyframes toastin{from{transform:translateY(8px);opacity:0}to{transform:none;opacity:1}}
.overlay{position:fixed;inset:0;z-index:200;background:rgba(2,6,15,.74);backdrop-filter:blur(6px);display:grid;place-items:center}.transfer-card{width:min(430px,calc(100% - 30px));background:#0d1729;border:1px solid #324969;border-radius:18px;padding:18px;display:grid;grid-template-columns:auto 1fr;gap:14px;box-shadow:0 35px 110px rgba(0,0,0,.7)}.spinner{width:34px;height:34px;border:4px solid #2b3d57;border-top-color:#ff7441;border-radius:50%;animation:spin .7s linear infinite}.progress-track{grid-column:1/-1;height:7px;background:#16243a;border-radius:999px;overflow:hidden}.progress-bar{height:100%;width:0;background:linear-gradient(90deg,#ff6b35,#22d3ee);transition:width .12s ease}@keyframes spin{to{transform:rotate(360deg)}}
.stats{display:grid;grid-template-columns:repeat(4,1fr);gap:10px}.stat{background:#08111f;border:1px solid #233754;border-radius:14px;padding:12px}.stat .v{font-size:19px;font-weight:850}.stat .k{font-size:11px;color:var(--muted);margin-top:3px}
.note{padding:11px 12px;border-radius:12px;background:#0a1c27;border:1px solid #17465b;color:#b7dbe9;font-size:12px;line-height:1.5}
@media(max-width:980px){.workspace{grid-template-columns:1fr}.sidebar{position:static;display:flex;overflow:auto}.nav-btn{white-space:nowrap;width:auto}.grid-2{grid-template-columns:1fr}.canvas-wrap{min-height:320px}}
@media(max-width:620px){.shell{padding:13px}.topbar{align-items:flex-start}.brand h1{font-size:24px}.sidebar{border-radius:14px}.card{border-radius:15px;padding:14px}.split{grid-template-columns:1fr}.stats{grid-template-columns:1fr 1fr}.canvas-wrap{min-height:260px}}

.switch-row{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:10px 0;border-bottom:1px solid #1d2d47}.switch-row:last-child{border-bottom:0}.switch{position:relative;width:46px;height:26px}.switch input{opacity:0;width:0;height:0}.switch-track{position:absolute;inset:0;background:#253750;border-radius:999px;transition:.18s}.switch-track:before{content:"";position:absolute;width:20px;height:20px;left:3px;top:3px;background:#fff;border-radius:50%;transition:.18s}.switch input:checked+.switch-track{background:#ff6b35}.switch input:checked+.switch-track:before{transform:translateX(20px)}
.clock-layouts{display:grid;grid-template-columns:repeat(5,minmax(105px,1fr));gap:9px}.clock-layout{border:1px solid #2b4060;background:#091323;border-radius:13px;padding:10px;cursor:pointer;text-align:center}.clock-layout:hover{border-color:#526d94;background:#0d1b31}.clock-layout.active{border-color:#ff7745;box-shadow:0 0 0 2px rgba(255,107,53,.13);background:#162137}.clock-layout .mini{width:64px;height:64px;margin:auto;background:#000;border-radius:7px;display:grid;place-items:center;font-family:monospace;font-size:9px;color:#fff;overflow:hidden}.clock-layout .label{font-size:11px;font-weight:800;margin-top:7px}
.color-grid{display:grid;grid-template-columns:repeat(2,1fr);gap:10px}.clock-color{display:grid;grid-template-columns:54px 1fr;gap:8px;align-items:center}.clock-color input[type=color]{width:54px;height:46px;border-radius:10px;border:1px solid #38516f;background:#060b14;padding:3px}
.clock-info{display:grid;grid-template-columns:repeat(3,1fr);gap:8px}.clock-info .box{background:#08111f;border:1px solid #253956;border-radius:12px;padding:10px}.clock-info .big{font-size:18px;font-weight:850}.clock-info .small{font-size:10px;color:#91a0b8;margin-top:3px}
.checkbox-line{display:flex;align-items:center;gap:8px;font-size:12px;color:#c7d3e4}.checkbox-line input{width:17px;height:17px;accent-color:#ff6b35}
.quick-range{display:grid;grid-template-columns:1fr 44px;gap:9px;align-items:center}.quick-range input[type=range]{width:100%;accent-color:#ff6b35}
@media(max-width:760px){.clock-layouts{grid-template-columns:repeat(2,1fr)}.color-grid{grid-template-columns:1fr}.clock-info{grid-template-columns:1fr 1fr}}
.brand-row{display:flex;align-items:center;gap:13px}.logo{display:grid;grid-template-columns:repeat(2,10px);gap:3px;flex:none}.logo i{width:10px;height:10px;border-radius:3px;background:#ef4444}.logo i:nth-child(2){background:#22c55e}.logo i:nth-child(3){background:#3b82f6}.logo i:nth-child(4){background:#f8fafc}
.nav-group{font-size:10.5px;letter-spacing:.14em;text-transform:uppercase;color:#5d6f8c;font-weight:800;padding:14px 13px 4px}.nav-group:first-child{padding-top:4px}
.nav-btn{display:flex;align-items:center;gap:11px}.nav-btn svg{width:18px;height:18px;flex:none}
.nav-dot{width:7px;height:7px;border-radius:50%;background:var(--green);box-shadow:0 0 8px var(--green);margin-left:auto;flex:none}.nav-dot.off,.dot.off{background:var(--red);box-shadow:0 0 8px var(--red)}
.page.active{animation:pagein .22s ease}@keyframes pagein{from{opacity:0;transform:translateY(6px)}to{opacity:1;transform:none}}
.tabbar,.sheet,.sheet-backdrop{display:none}
.tab{flex:1;min-width:0;display:flex;flex-direction:column;align-items:center;gap:3px;border:0;background:transparent;color:#8494ad;font:inherit;font-size:11px;font-weight:700;padding:3px 0;cursor:pointer;-webkit-tap-highlight-color:transparent}
.tab .ic{position:relative;width:58px;height:30px;display:grid;place-items:center;border-radius:999px;transition:background .2s,color .2s,transform .12s}
.tab svg{width:22px;height:22px}.tab.active{color:#fff}.tab.active .ic{background:rgba(255,107,53,.18);color:var(--accent)}.tab:active .ic{transform:scale(.92)}
.tab-badge{position:absolute;top:3px;right:15px;width:9px;height:9px;border-radius:50%;background:var(--red);border:2px solid #0a111f}
.sheet-backdrop{position:fixed;inset:0;z-index:70;background:rgba(2,6,15,.62);backdrop-filter:blur(3px);-webkit-backdrop-filter:blur(3px);opacity:0;pointer-events:none;transition:opacity .22s}.sheet-backdrop.open{opacity:1;pointer-events:auto}
.sheet{position:fixed;left:0;right:0;bottom:0;z-index:71;max-height:86vh;overflow-y:auto;overscroll-behavior:contain;background:#0c1424;border:1px solid var(--border);border-bottom:0;border-radius:24px 24px 0 0;padding:6px 16px calc(20px + env(safe-area-inset-bottom));transform:translateY(105%);transition:transform .28s cubic-bezier(.2,.8,.2,1);box-shadow:0 -24px 70px rgba(0,0,0,.55)}.sheet.open{transform:none}
.sheet-handle{width:44px;height:5px;border-radius:9px;background:#34486d;margin:6px auto 10px}
.sheet-head{display:flex;align-items:center;justify-content:space-between;margin:2px 2px 6px}.sheet-head h3{margin:0;font-size:17px}
.sheet-close{border:0;background:#13213a;color:#c9d4e5;width:34px;height:34px;border-radius:50%;font-size:20px;line-height:1;cursor:pointer}
.sheet-bright{background:#08111f;border:1px solid var(--border);border-radius:16px;padding:12px 14px;margin-top:8px}
.sheet-group{font-size:10.5px;letter-spacing:.14em;text-transform:uppercase;color:#5d6f8c;font-weight:800;margin:16px 2px 8px}
.sheet-grid{display:grid;grid-template-columns:repeat(4,1fr);gap:8px}
.tile{position:relative;display:flex;flex-direction:column;align-items:center;justify-content:center;gap:7px;min-height:78px;padding:10px 4px;border-radius:16px;border:1px solid var(--border);background:#0a1322;color:#c9d4e5;font:inherit;font-size:11.5px;font-weight:700;text-align:center;line-height:1.15;cursor:pointer;-webkit-tap-highlight-color:transparent}
.tile svg{width:24px;height:24px;color:#9fb0c9}.tile.active{border-color:var(--accent);background:rgba(255,107,53,.12);color:#fff}.tile.active svg{color:var(--accent)}.tile:active{transform:scale(.97)}.tile .nav-dot{position:absolute;top:9px;right:9px;margin:0}
body.no-scroll{overflow:hidden}
.page-head .pill{white-space:nowrap;flex:none}
.status-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(270px,1fr));gap:14px}
.status-title{display:flex;align-items:center;gap:9px;font-weight:850;margin-bottom:8px}.status-title svg{width:18px;height:18px;color:var(--accent)}
.kv{display:flex;justify-content:space-between;gap:12px;padding:7px 0;border-bottom:1px solid rgba(36,53,82,.55);font-size:13px}.kv:last-child{border-bottom:0}.kv span{color:var(--muted);flex:none}.kv b{font-weight:750;text-align:right;word-break:break-word}
.meter{height:6px;background:#16243a;border-radius:99px;overflow:hidden;margin:2px 0 6px}.meter span{display:block;height:100%;background:linear-gradient(90deg,#22d3ee,#ff6b35);border-radius:99px}
.monitor-head{display:flex;align-items:center;justify-content:space-between;gap:10px;flex-wrap:wrap}
.monitor{height:360px;overflow:auto;background:#03060d;border:1px solid #1c2a42;border-radius:14px;padding:12px;font:12px/1.55 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;color:#cbd5e1;white-space:pre-wrap;word-break:break-word}
.monitor .ts{color:#4b5d7a;margin-right:10px}.monitor .ok{color:#86efac}.monitor .warn{color:#fcd34d}.monitor .err{color:#fca5a5}
@media(max-width:980px){
  .shell{padding:0 14px calc(96px + env(safe-area-inset-bottom))}
  .topbar{position:sticky;top:0;z-index:40;margin:0 -14px 16px;padding:calc(12px + env(safe-area-inset-top)) 18px 12px;background:rgba(5,8,17,.82);backdrop-filter:blur(16px);-webkit-backdrop-filter:blur(16px);border-bottom:1px solid rgba(36,53,82,.7)}
  .brand .eyebrow{font-size:9.5px}.brand h1{font-size:20px;margin:2px 0 0}.logo{grid-template-columns:repeat(2,8px);gap:3px}.logo i{width:8px;height:8px}
  .workspace{display:block}.sidebar{display:none}
  .tabbar{display:flex;position:fixed;left:0;right:0;bottom:0;z-index:60;padding:6px 6px calc(6px + env(safe-area-inset-bottom));background:rgba(8,13,25,.92);backdrop-filter:blur(18px);-webkit-backdrop-filter:blur(18px);border-top:1px solid var(--border)}
  .sheet,.sheet-backdrop{display:block}
  .toast-host{left:14px;right:14px;bottom:calc(88px + env(safe-area-inset-bottom))}
  .monitor{height:300px;font-size:11px;padding:10px}.monitor .ts{margin-right:7px}
}
@media(max-width:360px){.sheet-grid{grid-template-columns:repeat(3,1fr)}}
.clock-grid{display:grid;grid-template-columns:minmax(0,1fr) 330px;gap:16px;align-items:start}.clock-main{min-width:0;order:1}.clock-side{order:2;position:sticky;top:16px}
.clock-preview-card{display:grid;gap:12px}.clock-preview-card .card-title{margin:0}
.clock-preview-wrap{display:flex;justify-content:center;align-items:center;padding:12px;background:repeating-conic-gradient(#08101d 0 25%,#0b1423 0 50%) 50%/18px 18px;border:1px solid #253754;border-radius:14px}
.clock-preview-wrap canvas{width:256px;max-width:100%}
.live-row{border-bottom:0;padding:2px 0 0}
.seg-control{display:flex;background:#08111f;border:1px solid var(--border);border-radius:12px;padding:3px;gap:3px}
.seg{flex:1;border:0;background:transparent;color:#9aa9bf;font:inherit;font-size:13px;font-weight:750;padding:8px 6px;border-radius:9px;transition:background .15s,color .15s}.seg.active{background:#1c2b45;color:#fff}
.face-group{font-size:10.5px;letter-spacing:.14em;text-transform:uppercase;color:#5d6f8c;font-weight:800;margin:14px 2px 8px}.face-group:first-child{margin-top:0}
.face-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(112px,1fr));gap:10px}
.face-card{position:relative;display:flex;flex-direction:column;align-items:center;gap:8px;padding:10px;border-radius:14px;border:1px solid var(--border);background:#0a1322;color:#c9d4e5;font:inherit;font-size:12px;font-weight:750;text-align:center;transition:border-color .15s,background .15s,color .15s,transform .12s}
.face-card canvas{width:100%;max-width:120px;border-radius:8px;border:1px solid #1d2b44;box-shadow:none}
.face-card.active{border-color:var(--accent);background:rgba(255,107,53,.1);color:#fff;box-shadow:0 0 0 2px rgba(255,107,53,.35)}
.face-live{position:absolute;top:6px;left:6px;font-style:normal;font-size:9.5px;font-weight:850;letter-spacing:.04em;background:var(--green);color:#052e16;border-radius:999px;padding:2px 7px}
.param-sec{border-top:1px solid #1d2d47;padding:6px 0}.param-sec:first-child{border-top:0}
.param-sec>summary{list-style:none;font-size:11px;letter-spacing:.14em;text-transform:uppercase;color:#7b8dab;font-weight:850;padding:8px 0}.param-sec>summary::-webkit-details-marker{display:none}.param-sec>summary::after{content:'▾';float:right;color:#5d6f8c}.param-sec:not([open])>summary::after{content:'▸'}
.param-block{border:1px solid #1d2d47;border-radius:14px;padding:4px 12px;margin:8px 0;background:#08111f}
.param-row{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:8px 0;min-height:44px}
.param-label{font-weight:750;font-size:14px}.param-sub{font-size:13px;color:var(--muted)}
.color-pick{display:grid;grid-template-columns:44px 92px;gap:8px;align-items:center}
.color-pick input[type=color]{width:44px;height:36px;padding:2px;border:1px solid #344c70;border-radius:10px;background:#0a1322}
.color-pick .hex{padding:8px 9px;font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:13px}
.select-sm{width:auto;min-width:140px;padding:8px 10px}
.chips{display:flex;flex-wrap:wrap;gap:6px;padding:2px 0 10px}
.chip{border:1px solid rgba(255,255,255,.18);background:linear-gradient(90deg,var(--c1),var(--c2));color:#07101d;font:inherit;font-size:12px;font-weight:850;padding:7px 11px;border-radius:999px;transition:filter .15s,transform .12s}
.slot-grid{display:grid;grid-template-columns:repeat(3,1fr);gap:10px}
.slot{display:flex;flex-direction:column;gap:8px;align-items:center;border:1px solid #1d2d47;border-radius:14px;padding:10px;background:#08111f}
.slot-canvas{width:72px;height:72px;aspect-ratio:1;border-radius:8px;box-shadow:none}.slot-canvas.empty{background:repeating-conic-gradient(#0d1a2c 0 25%,#12223a 0 50%) 50%/12px 12px}
.slot-actions{display:flex;flex-wrap:wrap;gap:6px;justify-content:center}
.btn-sm{padding:7px 10px;font-size:12px;border-radius:9px}
.city-results{display:grid;gap:6px}.city-results:empty{display:none}
.city-item{display:flex;flex-direction:column;align-items:flex-start;gap:2px;text-align:left;border:1px solid var(--border);background:#0a1322;color:#e5e7eb;font:inherit;padding:10px 12px;border-radius:12px;transition:border-color .15s,background .15s}
.city-item span{font-size:12px;color:var(--muted)}
.city-pin{font-size:12px}
.wx-now{font-size:15px}.wx-days{display:grid;grid-template-columns:repeat(4,1fr);gap:8px}
.wx-day{display:flex;flex-direction:column;gap:3px;border:1px solid #1d2d47;border-radius:12px;padding:8px 6px;text-align:center;background:#08111f;font-size:13px}.wx-day b{font-size:12px;color:#c9d4e5}.wx-day small{font-size:10.5px;color:var(--muted)}
.t-max{color:#fdba74;font-weight:800}.t-min{color:#60a5fa;font-weight:800}
.pick-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(88px,1fr));gap:8px;max-height:60vh;overflow:auto}
.pick{display:flex;flex-direction:column;gap:4px;align-items:center;border:1px solid var(--border);background:#0a1322;color:#c9d4e5;font:inherit;font-size:11px;padding:6px;border-radius:10px;transition:border-color .15s,background .15s}.pick canvas{width:100%;box-shadow:none;border-radius:6px}
button,a,summary,label.switch,.file-picker,.file-button,.nav-btn,.tab,.tile,.library-card,.face-card,.chip,.seg,.city-item,.pick,.gif-tab,.giphy-grid img,select,input[type=color],input[type=range],input[type=checkbox],label.btn{cursor:pointer}
button:disabled,.btn:disabled{cursor:not-allowed;opacity:.5}
@media(hover:hover){
.btn:hover:not(:disabled){background:#22344f;border-color:#4b6a93;color:#fff}
.btn-primary:hover:not(:disabled){background:linear-gradient(135deg,#ff8a5b,#ffb08a);border-color:transparent;color:#1a0d05}
.btn-green:hover:not(:disabled){background:#166534;border-color:#22c55e;color:#dcfce7}
.btn-danger:hover:not(:disabled){background:#5a1f2a;border-color:#9f3040;color:#ffd5da}
.file-picker:hover{border-color:#5b7aa6;background:#0b1628}.file-picker:hover .file-button{background:#27405f;color:#fff}
a:hover{color:#ffb08a}
.tab:hover{color:#fff}.tab:hover .ic{background:rgba(255,255,255,.07)}.tab.active:hover .ic{background:rgba(255,107,53,.26)}
.tile:hover{border-color:#4b6a93;background:#122038;color:#fff}.tile:hover svg{color:#fff}
.face-card:hover{border-color:#4b6a93;background:#122038;color:#fff}.face-card.active:hover{border-color:var(--accent2)}
.chip:hover{filter:brightness(1.18);transform:translateY(-1px)}
.seg:hover:not(.active){background:#13213a;color:#fff}
.city-item:hover,.pick:hover:not(:disabled){border-color:#4b6a93;background:#122038;color:#fff}
.param-sec>summary:hover{color:#fff}
.sheet-close:hover{background:#22344f;color:#fff}
.giphy-grid img:hover{border-color:#4b6a93}
}
.btn:active:not(:disabled),.face-card:active,.chip:active,.seg:active,.city-item:active,.tile:active,.pick:active:not(:disabled){transform:scale(.97)}
:focus-visible{outline:2px solid #22d3ee;outline-offset:2px}
@media(max-width:980px){
.clock-grid{grid-template-columns:1fr;gap:12px}
.clock-side{order:0;position:sticky;top:calc(58px + env(safe-area-inset-top));z-index:30}
.clock-preview-card{grid-template-columns:104px minmax(0,1fr);gap:8px 12px;align-items:center;padding:10px 12px;background:rgba(12,20,36,.97);backdrop-filter:blur(14px);-webkit-backdrop-filter:blur(14px)}
.clock-preview-card .card-title{grid-column:2;font-size:15px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.clock-preview-wrap{grid-row:1 / span 3;padding:4px;border-radius:10px}.clock-preview-wrap canvas{width:94px}
.clock-preview-card .seg-control,.clock-preview-card .toolbar{grid-column:2}
.clock-preview-card .seg{padding:6px 4px;font-size:12px}
.clock-preview-card .toolbar{gap:6px}.clock-preview-card .toolbar .btn{padding:8px 9px;font-size:12px}
.clock-preview-card .live-row{grid-column:1 / -1;padding:2px 0 0}.clock-preview-card .live-row .text-xs{display:none}
.face-grid{grid-template-columns:repeat(3,1fr);gap:8px}.face-card{padding:8px 6px;font-size:11px}
.color-pick{grid-template-columns:44px 84px}
.wx-days{gap:6px}
}
)CSS";

const char APP_JS[] PROGMEM = R"JS(const $=q=>document.querySelector(q), $$=q=>[...document.querySelectorAll(q)];
const host=$('#app');

function toast(msg,type='ok',ms=2400){const el=document.createElement('div');el.className='toast '+type;el.textContent=msg;$('#toastHost').appendChild(el);setTimeout(()=>el.remove(),ms)}
function showLoader(title='Transfiriendo…',detail='Preparando',pct=4){$('#transferTitle').textContent=title;$('#transferDetail').textContent=detail;$('#transferProgress').style.width=pct+'%';$('#transferOverlay').classList.remove('hidden')}
function updateLoader(detail,pct){$('#transferDetail').textContent=detail;$('#transferProgress').style.width=Math.max(2,Math.min(100,pct))+'%'}
function hideLoader(){$('#transferOverlay').classList.add('hidden')}
function modal(title,body,ok='Aceptar',cancel='Cancelar'){return new Promise(resolve=>{const d=document.createElement('dialog');d.innerHTML=`<div class="dialog-head">${title}</div><div class="dialog-body">${body}</div><div class="dialog-foot">${cancel?`<button class="btn cancel">${cancel}</button>`:''}<button class="btn btn-primary ok">${ok}</button></div>`;document.body.appendChild(d);d.showModal();if(cancel)d.querySelector('.cancel').onclick=()=>{d.close();d.remove();resolve(null)};d.querySelector('.ok').onclick=()=>{let vals={};d.querySelectorAll('[name]').forEach(x=>vals[x.name]=x.type==='checkbox'?x.checked:x.value);d.close();d.remove();resolve(vals)}})}

host.innerHTML=`
<div class="shell">
 <header class="topbar">
  <div class="brand-row"><div class="logo" aria-hidden="true"><i></i><i></i><i></i><i></i></div><div class="brand"><div class="eyebrow">ESP32-WROVER · HUB75 · 64×64</div><h1>Matrix Studio</h1></div></div>
 </header>
 <div class="workspace">
  <aside class="sidebar">
   <nav id="sideNav" aria-label="Secciones"></nav>
   <div class="card mt-3" style="padding:12px;box-shadow:none">
    <div class="text-xs muted">Brillo global</div>
    <div class="quick-range mt-2"><input id="globalBrightness" type="range" min="1" max="255" value="20"><span id="globalBrightnessValue">20</span></div>
   </div>
  </aside>
  <main class="content">
   <section class="page active" id="page-image">
    <div class="page-head"><div><h2>Imagen</h2><p>Recorta, rota y convierte a 64×64 antes de enviar.</p></div></div>
    <div class="grid-2">
     <div class="card">
      <div class="card-title">Archivo</div>
      <label class="file-picker" for="imgFile"><span class="file-button">Elegir imagen</span><span class="file-name" id="imgFileName">Ningún archivo seleccionado</span></label>
      <input class="file-native" id="imgFile" type="file" accept="image/*">
      <div class="split mt-3">
       <label class="field">Ajuste<select class="select" id="fit"><option value="cover">Llenar / recortar</option><option value="contain">Imagen completa</option></select></label>
       <label class="field">Rotación<select class="select" id="rot"><option>0</option><option>90</option><option>180</option><option>270</option></select></label>
      </div>
      <div class="switch-row mt-2"><div><div class="font-semibold text-sm">Suavizado al reducir</div><div class="text-xs muted">Mejor para fotografías; desactívalo para pixel art.</div></div><label class="switch"><input id="smoothSend" type="checkbox" checked><span class="switch-track"></span></label></div>
      <div class="row-wrap mt-4"><button class="btn btn-primary" id="sendImg">Enviar al panel</button><button class="btn" id="saveImg">Guardar en SD</button></div>
     </div>
     <div class="card"><div class="card-title">Vista previa</div><div class="canvas-wrap"><canvas id="imgCanvas" width="64" height="64"></canvas></div></div>
    </div>
   </section>

   <section class="page" id="page-pixel">
    <div class="page-head"><div><h2>Pixel Art</h2><p>Editor nativo 64×64 con herramientas de precisión.</p></div></div>
    <div class="grid-2"><div class="card"><div class="card-title">Herramientas</div><div class="toolbar"><select class="select" style="width:auto" id="tool"><option value="pencil">Lápiz</option><option value="eraser">Goma</option></select><select class="select" style="width:auto" id="brush"><option value="1">1 px</option><option value="2">2 px</option><option value="3">3 px</option></select></div><div class="color-control mt-3"><input id="pixColor" type="color" value="#ff3b30"><input class="input" id="pixHex" value="#ff3b30"></div><div class="toolbar mt-4"><button class="btn" id="undo">Deshacer</button><button class="btn" id="clearPix">Limpiar</button><button class="btn btn-primary" id="sendPix">Enviar</button><button class="btn" id="savePix">Guardar</button></div></div><div class="card"><div class="canvas-wrap"><canvas id="pixCanvas" class="pixel-canvas" width="64" height="64"></canvas></div></div></div>
   </section>

   <section class="page" id="page-text">
    <div class="page-head"><div><h2>Texto / Marquesina</h2><p>Multilínea, wrap automático y estilos por selección.</p></div></div>
    <div class="grid-2"><div class="card"><div class="card-title">Editor enriquecido</div><div id="richEditor" class="rich-editor" contenteditable="true" data-placeholder="Escribe aquí varias líneas…">HOLA<br>MUNDO</div><div class="mt-3 row-wrap"><span class="selection-badge" id="selectionInfo">Sin selección</span></div><div class="split mt-3"><div><label class="field">Color de selección</label><div class="color-control mt-2"><input id="txtColor" type="color" value="#00ffff"><input class="input" id="txtHex" value="#00ffff"></div></div><label class="field">Tamaño<select class="select" id="txtSize"><option>8</option><option selected>10</option><option>12</option><option>14</option><option>16</option><option>20</option></select></label></div><label class="field mt-3">Fuente<select class="select" id="txtFont"><option value="monospace">Monospace</option><option value="Arial">Arial</option><option value="sans-serif">Sans</option><option value="serif">Serif</option></select></label><div class="switch-row mt-2"><div><div class="font-semibold text-sm">Texto nítido (sin suavizado)</div><div class="text-xs muted">Píxeles sólidos, sin bordes grises. Recomendado para el panel RGB.</div></div><label class="switch"><input id="txtCrisp" type="checkbox" checked><span class="switch-track"></span></label></div><div class="toolbar mt-4"><button class="btn btn-primary" id="applySelectionStyle">Aplicar a selección</button><button class="btn" id="clearTextStyles">Quitar estilos</button><button class="btn" id="renderText">Actualizar preview</button></div><div class="note mt-4">Selecciona exactamente una letra, palabra o frase dentro del editor y luego aplica color, tamaño o fuente. El estilo sólo afecta ese rango.</div><div class="toolbar mt-4"><button class="btn btn-primary" id="sendText">Enviar al panel</button></div></div><div class="card preview-panel"><div class="card-title">Preview 64×64</div><div class="canvas-wrap"><canvas id="textCanvas" width="64" height="64"></canvas></div></div></div>
   </section>

   <section class="page" id="page-clock">
    <div class="page-head"><div><h2>Modo reloj</h2><p>Elige una carátula, personalízala y mándala al panel.</p></div><span class="pill"><span class="dot off" id="clockDot"></span><span id="clockState">Inactivo</span></span></div>
    <div class="clock-grid">
     <div class="clock-side">
      <div class="card clock-preview-card">
       <div class="card-title" id="clockFaceTitle">Vista previa</div>
       <div class="clock-preview-wrap"><canvas id="clockCanvas" width="64" height="64"></canvas></div>
       <div class="seg-control" id="clockSim" role="group" aria-label="Simular"><button class="seg active" data-sim="auto">Ahora</button><button class="seg" data-sim="day">Día</button><button class="seg" data-sim="night">Noche</button></div>
       <div class="toolbar"><button class="btn btn-primary" id="activateClock">Aplicar al panel</button><button class="btn" id="resetFace">Restablecer</button><button class="btn btn-danger" id="stopClock">Desactivar</button></div>
       <div class="switch-row live-row"><div><div class="font-semibold text-sm">Ver en el panel mientras edito</div><div class="text-xs muted">En el LED los colores no se ven igual que en pantalla.</div></div><label class="switch"><input id="clockLive" type="checkbox"><span class="switch-track"></span></label></div>
      </div>
     </div>
     <div class="clock-main">
      <div class="card"><div class="card-title">Carátulas</div><div id="faceGallery"></div></div>
      <div class="card mt-3"><div class="card-title" id="faceEditorTitle">Personalizar</div><p class="muted text-sm" id="faceEditorDesc"></p><div id="faceEditor"></div></div>
      <div class="card mt-3">
       <div class="card-title">Formato</div>
       <div class="switch-row"><span>Formato 24 horas</span><label class="switch"><input id="clock24" type="checkbox" checked><span class="switch-track"></span></label></div>
       <div class="switch-row"><span>Parpadeo de los dos puntos</span><label class="switch"><input id="clockBlink" type="checkbox"><span class="switch-track"></span></label></div>
       <label class="field mt-3">Idioma de las fechas<select class="select mt-2" id="clockLang"><option value="0">Español (LUN, ENE)</option><option value="1">English (MON, JAN)</option></select></label>
       <label class="field mt-3">Brillo<div class="quick-range mt-2"><input id="clockBrightness" type="range" min="1" max="255" value="20"><span id="clockBrightnessValue">20</span></div></label>
      </div>
      <div class="card mt-3">
       <div class="card-title">Ubicación y clima</div>
       <div class="row-wrap"><span class="pill"><span class="city-pin">📍</span><span id="cityName">Puebla</span></span><button class="btn" id="refreshWeather">Actualizar clima</button></div>
       <div class="row-wrap mt-3"><input class="input" style="flex:1;min-width:0" id="citySearch" placeholder="Buscar ciudad…" autocomplete="off"><button class="btn btn-primary" id="citySearchBtn">Buscar</button></div>
       <div class="city-results mt-2" id="cityResults"></div>
       <div class="wx-now mt-3" id="wxNow"></div>
       <div class="wx-days mt-2" id="wxDays"></div>
       <div class="note mt-3">La hora se sincroniza por Internet y la zona horaria se ajusta sola según la ciudad. El clima viene de Open-Meteo y se actualiza cada 15 minutos.</div>
      </div>
     </div>
    </div>
   </section>

   <section class="page" id="page-library"><div class="page-head"><div><h2>Biblioteca Pixel Art</h2><p>Contenido local + importación experimental desde enlaces públicos de Pixilart.</p></div></div><div class="card"><div class="card-title">Colección local</div><div id="builtInLibrary" class="grid-cards"></div></div><div class="card"><div class="card-title">Pixilart</div><div class="note">Pixilart no publica una API oficial/documentada para consultar su galería. Matrix Studio usa únicamente páginas públicas y el metadato og:image. Respeta los derechos/licencia del autor.</div><div class="row-wrap mt-4"><a class="btn" target="_blank" rel="noopener" href="https://www.pixilart.com/gallery/tags/64x64">Abrir Pixilart #64x64</a></div><label class="field mt-4">URL pública de una obra<input class="input" id="pixilartUrl" placeholder="https://www.pixilart.com/art/..."></label><button class="btn btn-primary mt-3" id="importPixilart">Importar obra</button></div></section>
   <section class="page" id="page-gallery"><div class="page-head"><div><h2>Galería</h2><p>Contenido persistente guardado en la microSD.</p></div><button class="btn" id="reloadGallery">Actualizar</button></div><div class="grid-cards" id="galleryList"></div></section>
   <section class="page" id="page-gifs">
    <div class="page-head"><div><h2>GIF's</h2><p>Sube GIFs animados: el navegador los convierte a 64×64 y se guardan en la microSD.</p></div><button class="btn" id="gifReload">Actualizar</button></div>
    <div class="grid-2">
     <div class="card">
      <div class="card-title">Nuevo GIF</div>
      <div class="toolbar"><button class="btn btn-primary gif-tab" data-tab="upload">Subir GIF</button><button class="btn gif-tab" data-tab="giphy">Buscar en Giphy</button></div>
      <div class="mt-3" id="gifTabUpload">
       <label class="file-picker" for="gifFile"><span class="file-button">Elegir GIF</span><span class="file-name" id="gifFileName">Ningún archivo</span></label>
       <input class="file-native" id="gifFile" type="file" accept="image/gif">
      </div>
      <div class="mt-3 hidden" id="gifTabGiphy">
       <div class="row-wrap"><input class="input" style="flex:1;min-width:0" id="giphyQuery" placeholder="Buscar GIFs…"><button class="btn btn-primary" id="giphySearch">Buscar</button></div>
       <div class="giphy-scroll mt-3" id="giphyScroll"><div class="giphy-grid" id="giphyGrid"></div><div class="giphy-more"><button class="btn hidden" id="giphyMore">Cargar más</button></div></div>
       <div class="row-wrap mt-2"><span class="giphy-attr">Powered by GIPHY</span></div>
      </div>
      <div class="split mt-3">
       <label class="field">Ajuste 64×64<select class="select" id="gifFit"><option value="contain">Contener (completo)</option><option value="cover">Recortar (llenar)</option><option value="stretch">Estirar</option></select></label>
       <label class="field">Nombre<input class="input" id="gifName" placeholder="mi-gif"></label>
      </div>
      <div class="note mt-3" id="gifInfo">Elige un GIF para previsualizarlo. No se envía al panel hasta que lo agregues y lo cargues.</div>
      <div class="row-wrap mt-4"><button class="btn btn-primary" id="gifAdd" disabled>Agregar al historial</button></div>
     </div>
     <div class="card"><div class="card-title">Vista previa 64×64</div><div class="canvas-wrap"><canvas id="gifCanvas" width="64" height="64"></canvas></div></div>
    </div>
    <div class="card mt-3">
     <div class="card-title">Historial en la microSD</div>
     <div class="grid-cards" id="gifGrid"></div>
     <div class="toolbar mt-4"><button class="btn btn-primary" id="gifLoad" disabled>Cargar GIF al panel</button><button class="btn btn-danger" id="gifDelete" disabled>Borrar GIF</button></div>
    </div>
   </section>
   <section class="page" id="page-admin"><div class="page-head"><div><h2>Admin SD</h2><p>Gestiona archivos sin desmontar el display.</p></div></div><div class="card"><div class="row-wrap"><input class="input" style="max-width:420px" id="adminPath" value="/www"><button class="btn" id="listFiles">Listar</button><button class="btn" id="mkdir">Crear carpeta</button></div><label class="file-picker mt-3" for="adminUpload"><span class="file-button">Elegir archivos</span><span class="file-name" id="adminFileName">Ningún archivo</span></label><input class="file-native" id="adminUpload" type="file" multiple><button class="btn btn-green mt-3" id="uploadFiles">Subir a carpeta actual</button><div class="mt-4" id="fileList"></div></div></section>
   <section class="page" id="page-firmware"><div class="page-head"><div><h2>Firmware OTA</h2><p>Actualiza el ESP32 desde el navegador.</p></div></div><div class="card"><label class="file-picker" for="fwFile"><span class="file-button">Elegir .bin</span><span class="file-name" id="fwFileName">Ningún firmware</span></label><input class="file-native" id="fwFile" type="file" accept=".bin,application/octet-stream"><button class="btn btn-primary mt-4" id="fwUpload">Instalar firmware</button></div></section>
   <section class="page" id="page-panel"><div class="page-head"><div><h2>Panel</h2><p>Ajustes del panel LED.</p></div></div><div class="card"><label class="field">Brillo<div class="quick-range mt-2"><input id="brightness" type="range" min="1" max="255" value="20"><span id="brightnessValue">20</span></div></label></div></section>
   <section class="page" id="page-status">
    <div class="page-head"><div><h2>Estado</h2><p>Salud del ESP32 en vivo y monitor de eventos: lo mismo que ves en el Monitor Serie.</p></div><span class="pill"><span class="dot" id="statusDot"></span><span id="statusConn">Conectando…</span></span></div>
    <div class="status-grid" id="statusGrid"><div class="muted">Cargando…</div></div>
    <div class="card mt-3">
     <div class="monitor-head"><div class="card-title" style="margin:0">Monitor serie</div><div class="toolbar"><button class="btn" id="logPause">Pausar</button><button class="btn" id="logClear">Limpiar</button><button class="btn" id="logCopy">Copiar</button></div></div>
     <div class="monitor mt-3" id="logBox"></div>
    </div>
   </section>
  </main>
 </div>
 <nav class="tabbar" id="tabbar" aria-label="Secciones"></nav>
 <div class="sheet-backdrop" id="sheetBackdrop"></div>
 <div class="sheet" id="sheet" role="dialog" aria-modal="true" aria-label="Todas las secciones">
  <div class="sheet-handle"></div>
  <div class="sheet-head"><h3>Secciones</h3><button class="sheet-close" id="sheetClose" aria-label="Cerrar">×</button></div>
  <div class="sheet-bright"><div class="text-xs muted">Brillo del panel</div><div class="quick-range mt-2"><input id="sheetBrightness" type="range" min="1" max="255" value="20"><span id="sheetBrightnessValue">20</span></div></div>
  <div id="sheetBody"></div>
 </div>
</div>`;

// ---- Navigation ----
// One NAV list drives the desktop sidebar, the mobile bottom tab bar and the
// "Más" bottom sheet. Sections are reachable by URL hash, so the phone's back
// button moves between sections (and closes the sheet first when it is open).
const ICONS={
  image:'<rect x="3" y="3" width="18" height="18" rx="2"/><circle cx="9" cy="9" r="2"/><path d="m21 15-3.1-3.1a2 2 0 0 0-2.8 0L6 21"/>',
  pixel:'<path d="M12 20h9"/><path d="M16.5 3.5a2.1 2.1 0 0 1 3 3L7 19l-4 1 1-4Z"/>',
  text:'<path d="M4 7V4h16v3"/><path d="M9 20h6"/><path d="M12 4v16"/>',
  gifs:'<rect x="2" y="5" width="20" height="14" rx="2"/><path d="m10 9 5 3-5 3z"/>',
  clock:'<circle cx="12" cy="12" r="9"/><path d="M12 7v5l3 2"/>',
  gallery:'<rect x="7" y="7" width="14" height="14" rx="2"/><path d="M3 17V5a2 2 0 0 1 2-2h12"/>',
  library:'<path d="M4 19.5A2.5 2.5 0 0 1 6.5 17H20"/><path d="M6.5 2H20v20H6.5A2.5 2.5 0 0 1 4 19.5v-15A2.5 2.5 0 0 1 6.5 2z"/>',
  status:'<path d="M22 12h-4l-3 9L9 3l-3 9H2"/>',
  panel:'<path d="M4 21v-7M4 10V3M12 21v-9M12 8V3M20 21v-5M20 12V3M1 14h6M9 8h6M17 16h6"/>',
  admin:'<path d="M7 2h8l4 4v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2z"/><path d="M9 6v3M12 6v3M15 7v2"/>',
  firmware:'<path d="M12 15V3"/><path d="m7 8 5-5 5 5"/><path d="M5 21h14"/>',
  more:'<rect x="3" y="3" width="7" height="7" rx="1.5"/><rect x="14" y="3" width="7" height="7" rx="1.5"/><rect x="3" y="14" width="7" height="7" rx="1.5"/><rect x="14" y="14" width="7" height="7" rx="1.5"/>'
};
const icon=n=>`<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${ICONS[n]}</svg>`;
const NAV=[
  {id:'image',label:'Imagen',group:'Crear'},
  {id:'pixel',label:'Pixel Art',group:'Crear'},
  {id:'text',label:'Texto',group:'Crear'},
  {id:'gifs',label:"GIF's",group:'Crear'},
  {id:'clock',label:'Modo reloj',short:'Reloj',group:'Mostrar'},
  {id:'gallery',label:'Galería',group:'Mostrar'},
  {id:'library',label:'Biblioteca',group:'Mostrar'},
  {id:'status',label:'Estado',group:'Sistema'},
  {id:'panel',label:'Panel',group:'Sistema'},
  {id:'admin',label:'Admin SD',group:'Sistema'},
  {id:'firmware',label:'Firmware',group:'Sistema'}
];
const TABS=['image','text','gifs','clock'],GROUPS=['Crear','Mostrar','Sistema'];
const navDot=id=>id==='status'?'<span class="nav-dot"></span>':'';
$('#sideNav').innerHTML=GROUPS.map(g=>`<div class="nav-group">${g}</div>`+NAV.filter(n=>n.group===g).map(n=>`<button class="nav-btn" data-page="${n.id}">${icon(n.id)}<span>${n.label}</span>${navDot(n.id)}</button>`).join('')).join('');
$('#tabbar').innerHTML=TABS.map(id=>{const n=NAV.find(x=>x.id===id);return `<button class="tab" data-page="${id}"><span class="ic">${icon(id)}</span><span>${n.short||n.label}</span></button>`}).join('')+`<button class="tab" id="tabMore" aria-haspopup="dialog"><span class="ic">${icon('more')}<span class="tab-badge hidden" id="moreBadge"></span></span><span>Más</span></button>`;
$('#sheetBody').innerHTML=GROUPS.map(g=>`<div class="sheet-group">${g}</div><div class="sheet-grid">`+NAV.filter(n=>n.group===g).map(n=>`<button class="tile" data-page="${n.id}">${icon(n.id)}<span>${n.label}</span>${navDot(n.id)}</button>`).join('')+'</div>').join('');

let currentPage=null,sheetOpen=false,sheetOpenedAt=0;
const PAGE_HOOKS={gallery:()=>loadGallery(),gifs:()=>loadGifs(),admin:()=>listFiles(),clock:()=>clockEnter(),status:()=>statusStart()};
function go(page){
  if(!$('#page-'+page))page='image';
  const changed=page!==currentPage;currentPage=page;
  $$('.page').forEach(x=>x.classList.toggle('active',x.id==='page-'+page));
  $$('.nav-btn,.tab,.tile').forEach(x=>{const on=x.dataset.page===page;x.classList.toggle('active',on);if(on)x.setAttribute('aria-current','page');else x.removeAttribute('aria-current')});
  $('#tabMore').classList.toggle('active',!TABS.includes(page));
  if(page!=='status')statusStop();
  if(page!=='clock')clockLeave();
  if(changed)window.scrollTo(0,0);
  try{localStorage.setItem('ms.page',page)}catch(e){}
  const h=PAGE_HOOKS[page];if(h)h();
}
function nav(page){
  if(sheetOpen){hideSheet();history.replaceState(null,'','#'+page)}
  else if(page!==currentPage)history.pushState(null,'','#'+page);
  go(page);
}
function showSheet(){if(sheetOpen)return;sheetOpen=true;sheetOpenedAt=Date.now();$('#sheet').classList.add('open');$('#sheetBackdrop').classList.add('open');document.body.classList.add('no-scroll');history.pushState({sheet:1},'',location.href)}
function hideSheet(){sheetOpen=false;const sh=$('#sheet');sh.classList.remove('open');sh.style.transform='';$('#sheetBackdrop').classList.remove('open');document.body.classList.remove('no-scroll')}
// Ignores the ghost click a tap can fire right after opening the sheet.
function closeSheet(){if(!sheetOpen||Date.now()-sheetOpenedAt<400)return;hideSheet();if(history.state&&history.state.sheet)history.back()}
document.addEventListener('click',e=>{const b=e.target.closest('.nav-btn,.tab[data-page],.tile');if(b&&b.dataset.page)nav(b.dataset.page)});
$('#tabMore').onclick=showSheet;
$('#sheetBackdrop').onclick=closeSheet;$('#sheetClose').onclick=closeSheet;
document.addEventListener('keydown',e=>{if(e.key==='Escape')closeSheet()});
window.addEventListener('popstate',()=>{if(sheetOpen){hideSheet();return}const p=location.hash.slice(1)||'image';if(p!==currentPage)go(p)});
(()=>{const sh=$('#sheet');let y0=null,dy=0;
  sh.addEventListener('touchstart',e=>{if(sh.scrollTop>0){y0=null;return}y0=e.touches[0].clientY;dy=0;sh.style.transition='none'},{passive:true});
  sh.addEventListener('touchmove',e=>{if(y0===null)return;dy=Math.max(0,e.touches[0].clientY-y0);sh.style.transform=`translateY(${dy}px)`},{passive:true});
  sh.addEventListener('touchend',()=>{if(y0===null)return;sh.style.transition='';y0=null;if(dy>90)closeSheet();else sh.style.transform=''});
})();

function black(ctx){ctx.fillStyle='#000';ctx.fillRect(0,0,64,64)}
function to565(canvas){const d=canvas.getContext('2d',{willReadFrequently:true}).getImageData(0,0,64,64).data,o=new Uint8Array(8192);for(let p=0;p<4096;p++){let i=p*4,v=((d[i]&248)<<8)|((d[i+1]&252)<<3)|(d[i+2]>>3);o[p*2]=(v>>8)&255;o[p*2+1]=v&255}return o}
function uploadXHR(url,field,blob,name,title='Transfiriendo'){return new Promise((resolve,reject)=>{const fd=new FormData();fd.append(field,blob,name);const x=new XMLHttpRequest();x.open('POST',url,true);showLoader(title,'Subiendo…',5);x.upload.onprogress=e=>{if(e.lengthComputable)updateLoader(`Subiendo ${Math.round(e.loaded/1024)} / ${Math.round(e.total/1024)} KB`,Math.round(e.loaded/e.total*88)+5)};x.upload.onload=()=>updateLoader('Renderizando en el panel…',94);x.onload=()=>{hideLoader();if(x.status>=200&&x.status<300){let data;try{data=JSON.parse(x.responseText)}catch{data=x.responseText}resolve(data)}else reject(new Error(x.responseText||'Error '+x.status))};x.onerror=()=>{hideLoader();reject(new Error('Error de red'))};x.send(fd)})}
async function sendCanvas(canvas){const b=to565(canvas);const data=await uploadXHR('/api/frame','frame',new Blob([b],{type:'application/octet-stream'}),'frame.rgb565','Enviando al panel');if(data?.renderMs!==undefined)toast(`Listo · render ${data.renderMs} ms`);else toast('Enviado al panel');return b}
async function sendRawFrame(bytes){return uploadXHR('/api/frame','frame',new Blob([bytes]),'frame.rgb565','Enviando al panel')}
async function saveFrame(bytes){await sendRawFrame(bytes);const v=await modal('Guardar en microSD','<label class="field">Nombre<input class="input" name="name" value="imagen"></label>','Guardar');if(!v)return;showLoader('Guardando','Escribiendo en microSD…',70);const r=await fetch('/api/gallery/save-image?name='+encodeURIComponent(v.name),{method:'POST'});hideLoader();if(!r.ok)throw Error(await r.text());toast('Guardado en microSD')}

// BRIGHTNESS
const BRIGHT_IDS=['globalBrightness','brightness','clockBrightness','sheetBrightness'];
let brightnessTimer=null;function setBrightness(v){BRIGHT_IDS.forEach(id=>{$('#'+id).value=v;$('#'+id+'Value').textContent=v});clearTimeout(brightnessTimer);brightnessTimer=setTimeout(()=>fetch('/api/brightness?v='+v,{method:'POST'}),100)}
BRIGHT_IDS.forEach(id=>$('#'+id).oninput=e=>setBrightness(e.target.value));

// IMAGE
const ic=$('#imgCanvas'),ix=ic.getContext('2d');black(ix);let img=null;
$('#imgFile').onchange=e=>{const f=e.target.files[0];$('#imgFileName').textContent=f?f.name:'Ningún archivo seleccionado';if(!f)return;const u=URL.createObjectURL(f),im=new Image();im.onload=()=>{img=im;drawImage();URL.revokeObjectURL(u)};im.src=u};
function drawImage(){if(!img)return;black(ix);const sw=img.naturalWidth,sh=img.naturalHeight,rot=+$('#rot').value,rr=rot===90||rot===270,rw=rr?sh:sw,rh=rr?sw:sh,s=$('#fit').value==='cover'?Math.max(64/rw,64/rh):Math.min(64/rw,64/rh);ix.save();ix.translate(32,32);ix.rotate(rot*Math.PI/180);ix.imageSmoothingEnabled=$('#smoothSend').checked;ix.drawImage(img,-sw*s/2,-sh*s/2,sw*s,sh*s);ix.restore()}
$('#fit').onchange=drawImage;$('#rot').onchange=drawImage;$('#smoothSend').onchange=drawImage;$('#sendImg').onclick=()=>sendCanvas(ic).catch(e=>toast(e.message,'err'));$('#saveImg').onclick=async()=>{try{const b=await sendCanvas(ic);await saveFrame(b)}catch(e){toast(e.message,'err')}};

// PIXEL
const pc=$('#pixCanvas'),px=pc.getContext('2d',{willReadFrequently:true});black(px);let hist=[],painting=false;
$('#pixColor').oninput=e=>$('#pixHex').value=e.target.value;$('#pixHex').onchange=e=>{if(/^#[0-9a-f]{6}$/i.test(e.target.value))$('#pixColor').value=e.target.value};
function ppos(e){const r=pc.getBoundingClientRect(),t=e.touches?.[0]||e;return{x:Math.max(0,Math.min(63,Math.floor((t.clientX-r.left)*64/r.width))),y:Math.max(0,Math.min(63,Math.floor((t.clientY-r.top)*64/r.height)))}}
function pushHist(){hist.push(px.getImageData(0,0,64,64));if(hist.length>30)hist.shift()}function paintAt(x,y){px.fillStyle=$('#tool').value==='eraser'?'#000':$('#pixColor').value;let s=+$('#brush').value;px.fillRect(x,y,s,s)}
pc.onmousedown=e=>{pushHist();painting=true;let p=ppos(e);paintAt(p.x,p.y)};pc.onmousemove=e=>{if(painting){let p=ppos(e);paintAt(p.x,p.y)}};window.addEventListener('mouseup',()=>painting=false);pc.ontouchstart=e=>{e.preventDefault();pushHist();painting=true;let p=ppos(e);paintAt(p.x,p.y)};pc.ontouchmove=e=>{e.preventDefault();if(painting){let p=ppos(e);paintAt(p.x,p.y)}};window.addEventListener('touchend',()=>painting=false);$('#undo').onclick=()=>{let i=hist.pop();if(i)px.putImageData(i,0,0)};$('#clearPix').onclick=()=>{pushHist();black(px)};$('#sendPix').onclick=()=>sendCanvas(pc).catch(e=>toast(e.message,'err'));$('#savePix').onclick=async()=>{try{const b=await sendCanvas(pc);await saveFrame(b)}catch(e){toast(e.message,'err')}};

// RICH TEXT
const editor=$('#richEditor'),tc=$('#textCanvas'),tx=tc.getContext('2d',{willReadFrequently:true});let savedRange=null;
function selectionInsideEditor(){const s=window.getSelection();if(!s||!s.rangeCount)return false;const r=s.getRangeAt(0);return editor.contains(r.commonAncestorContainer)}
document.addEventListener('selectionchange',()=>{if(selectionInsideEditor()){const s=window.getSelection(),r=s.getRangeAt(0);savedRange=r.cloneRange();const t=s.toString();$('#selectionInfo').textContent=t?`Selección: ${t.length} caracteres`:'Cursor sin selección'}});$('#txtColor').oninput=e=>$('#txtHex').value=e.target.value;$('#txtHex').onchange=e=>{if(/^#[0-9a-f]{6}$/i.test(e.target.value))$('#txtColor').value=e.target.value};
function applySelectionStyle(){if(!savedRange||savedRange.collapsed||!editor.contains(savedRange.commonAncestorContainer)){toast('Selecciona una letra, palabra o frase','err');return}const span=document.createElement('span');span.style.color=$('#txtColor').value;span.style.fontSize=$('#txtSize').value+'px';span.style.fontFamily=$('#txtFont').value;try{span.appendChild(savedRange.extractContents());savedRange.insertNode(span);savedRange.selectNodeContents(span);const s=window.getSelection();s.removeAllRanges();s.addRange(savedRange);savedRange=savedRange.cloneRange();renderRichText();toast('Estilo aplicado sólo a la selección')}catch(e){toast('No se pudo aplicar el estilo: '+e.message,'err')}}
$('#applySelectionStyle').onclick=applySelectionStyle;$('#clearTextStyles').onclick=()=>{const txt=editor.innerText;editor.innerHTML='';txt.split('\n').forEach((line,i)=>{if(i)editor.appendChild(document.createElement('br'));editor.appendChild(document.createTextNode(line))});renderRichText()};
function collectRuns(node,style={color:'#ffffff',size:10,font:'monospace'},out=[]){if(node.nodeType===Node.TEXT_NODE){if(node.nodeValue)out.push({text:node.nodeValue,style:{...style}});return out}if(node.nodeType!==Node.ELEMENT_NODE)return out;if(node.tagName==='BR'){out.push({text:'\n',style:{...style}});return out}const cs=getComputedStyle(node),next={...style,color:node.style.color||style.color,size:parseFloat(cs.fontSize)||style.size,font:cs.fontFamily||style.font};const block=['DIV','P'].includes(node.tagName);if(block&&out.length&&out[out.length-1].text!=='\n')out.push({text:'\n',style:{...next}});[...node.childNodes].forEach(ch=>collectRuns(ch,next,out));if(block&&out.length&&out[out.length-1].text!=='\n')out.push({text:'\n',style:{...next}});return out}
// Crisp mode draws each glyph as a white mask, thresholds its alpha and paints
// the lit pixels with the exact run color: every pixel is fully on or off, with
// no gray antialiasing fringe. Normal mode keeps the browser's smooth text.
function textRGB(c,mc){mc.fillStyle='#000';mc.fillStyle=c;const h=mc.fillStyle;if(h[0]==='#')return[parseInt(h.slice(1,3),16),parseInt(h.slice(3,5),16),parseInt(h.slice(5,7),16)];const t=(h.match(/[\d.]+/g)||[255,255,255]).map(Number);return[t[0],t[1],t[2]]}
function renderRichText(){
  const crisp=$('#txtCrisp').checked,off=document.createElement('canvas');off.width=64;off.height=64;
  const o=off.getContext('2d',{willReadFrequently:true}),out=crisp?o.createImageData(64,64):null;
  const mk=crisp?document.createElement('canvas'):null;if(mk){mk.width=64;mk.height=64}
  const mc=mk?mk.getContext('2d',{willReadFrequently:true}):null;
  const draw=(ch,x,y,st)=>{
    if(!crisp){o.fillStyle=st.color;o.textBaseline='alphabetic';o.fillText(ch,x,y);return}
    mc.clearRect(0,0,64,64);mc.font=o.font;mc.textBaseline='alphabetic';mc.fillStyle='#fff';mc.fillText(ch,x,y);
    const [r,g,b]=textRGB(st.color,mc),d=mc.getImageData(0,0,64,64).data,od=out.data;
    for(let i=3;i<d.length;i+=4)if(d[i]>=120){od[i-3]=r;od[i-2]=g;od[i-1]=b;od[i]=255}
  };
  (()=>{const runs=collectRuns(editor);let x=1,y=9,lineH=10;for(const run of runs){for(const ch of run.text){if(ch==='\n'){x=1;y+=lineH;lineH=10;if(y>63)return;continue}o.font=`${run.style.size}px ${run.style.font}`;const w=Math.max(1,Math.ceil(o.measureText(ch).width));lineH=Math.max(lineH,run.style.size+2);if(x+w>63){x=1;y+=lineH;lineH=run.style.size+2;if(y>63)return}draw(ch,x,y,run.style);x+=w}}})();
  if(crisp)o.putImageData(out,0,0);
  black(tx);tx.drawImage(off,0,0);
}
$('#txtCrisp').onchange=renderRichText;
editor.addEventListener('input',renderRichText);$('#renderText').onclick=renderRichText;$('#sendText').onclick=()=>{renderRichText();sendCanvas(tc).catch(e=>toast(e.message,'err'))};renderRichText();

// CLOCK UI
// ---- Modo reloj (caratulas) ----
// The ESP32 draws every face; the editor asks it for a preview of the current
// (unsaved) settings via /api/clock/preview and paints the returned pixels.
// Each face uses generic slots: a/b/d = paint colors + gradient direction,
// o = options. FACES says what every slot means.
const FACES=[{"id":"clasico-digital","name":"Digital","cat":"Clásicos","desc":"La hora grande, con segundos y fecha opcionales.","params":[{"t":"color","s":0,"l":"Fondo","a":"#000000"},{"t":"color","s":1,"l":"Hora / agujas","a":"#ffffff"},{"t":"color","s":2,"l":"Fecha / segundos","a":"#22d3ee"},{"t":"bool","o":0,"l":"Mostrar segundos","v":0},{"t":"bool","o":1,"l":"Mostrar fecha","v":1}],"def":{"a":["#000000","#ffffff","#22d3ee","#ff6b35","#7dd3fc","#000000","#000000","#000000","#000000","#000000"],"b":["#000000","#ffffff","#22d3ee","#ff6b35","#7dd3fc","#000000","#000000","#000000","#000000","#000000"],"d":[0,0,0,0,0,0,0,0,0,0],"o":[0,1,1,1,1,0,0,0]}},{"id":"clasico-fecha","name":"Digital + fecha","cat":"Clásicos","desc":"Hora arriba y fecha debajo.","params":[{"t":"color","s":0,"l":"Fondo","a":"#000000"},{"t":"color","s":1,"l":"Hora / agujas","a":"#ffffff"},{"t":"color","s":2,"l":"Fecha / segundos","a":"#22d3ee"},{"t":"bool","o":0,"l":"Mostrar segundos","v":0},{"t":"bool","o":1,"l":"Mostrar fecha","v":1}],"def":{"a":["#000000","#ffffff","#22d3ee","#ff6b35","#7dd3fc","#000000","#000000","#000000","#000000","#000000"],"b":["#000000","#ffffff","#22d3ee","#ff6b35","#7dd3fc","#000000","#000000","#000000","#000000","#000000"],"d":[0,0,0,0,0,0,0,0,0,0],"o":[0,1,1,1,1,0,0,0]}},{"id":"clasico-clima","name":"Clima","cat":"Clásicos","desc":"Hora, fecha y el clima actual abajo.","params":[{"t":"color","s":0,"l":"Fondo","a":"#000000"},{"t":"color","s":1,"l":"Hora / agujas","a":"#ffffff"},{"t":"color","s":2,"l":"Fecha / segundos","a":"#22d3ee"},{"t":"color","s":3,"l":"Acento","a":"#ff6b35"},{"t":"color","s":4,"l":"Clima","a":"#7dd3fc"},{"t":"bool","o":0,"l":"Mostrar segundos","v":0},{"t":"bool","o":1,"l":"Mostrar fecha","v":1},{"t":"bool","o":2,"l":"Temperatura","v":1},{"t":"bool","o":3,"l":"Humedad","v":1},{"t":"bool","o":4,"l":"Estado del clima","v":1}],"def":{"a":["#000000","#ffffff","#22d3ee","#ff6b35","#7dd3fc","#000000","#000000","#000000","#000000","#000000"],"b":["#000000","#ffffff","#22d3ee","#ff6b35","#7dd3fc","#000000","#000000","#000000","#000000","#000000"],"d":[0,0,0,0,0,0,0,0,0,0],"o":[0,1,1,1,1,0,0,0]}},{"id":"clasico-analogico","name":"Analógico","cat":"Clásicos","desc":"Reloj de manecillas con fecha.","params":[{"t":"color","s":0,"l":"Fondo","a":"#000000"},{"t":"color","s":1,"l":"Hora / agujas","a":"#ffffff"},{"t":"color","s":2,"l":"Fecha / segundos","a":"#22d3ee"},{"t":"color","s":3,"l":"Acento","a":"#ff6b35"},{"t":"bool","o":0,"l":"Mostrar segundos","v":0},{"t":"bool","o":1,"l":"Mostrar fecha","v":1}],"def":{"a":["#000000","#ffffff","#22d3ee","#ff6b35","#7dd3fc","#000000","#000000","#000000","#000000","#000000"],"b":["#000000","#ffffff","#22d3ee","#ff6b35","#7dd3fc","#000000","#000000","#000000","#000000","#000000"],"d":[0,0,0,0,0,0,0,0,0,0],"o":[0,1,1,1,1,0,0,0]}},{"id":"clasico-hibrido","name":"Híbrido","cat":"Clásicos","desc":"Manecillas a la izquierda, hora y clima a la derecha.","params":[{"t":"color","s":0,"l":"Fondo","a":"#000000"},{"t":"color","s":1,"l":"Hora / agujas","a":"#ffffff"},{"t":"color","s":2,"l":"Fecha / segundos","a":"#22d3ee"},{"t":"color","s":3,"l":"Acento","a":"#ff6b35"},{"t":"color","s":4,"l":"Clima","a":"#7dd3fc"},{"t":"bool","o":0,"l":"Mostrar segundos","v":0},{"t":"bool","o":1,"l":"Mostrar fecha","v":1},{"t":"bool","o":2,"l":"Temperatura","v":1},{"t":"bool","o":3,"l":"Humedad","v":1},{"t":"bool","o":4,"l":"Estado del clima","v":1}],"def":{"a":["#000000","#ffffff","#22d3ee","#ff6b35","#7dd3fc","#000000","#000000","#000000","#000000","#000000"],"b":["#000000","#ffffff","#22d3ee","#ff6b35","#7dd3fc","#000000","#000000","#000000","#000000","#000000"],"d":[0,0,0,0,0,0,0,0,0,0],"o":[0,1,1,1,1,0,0,0]}},{"id":"minimal-apilado","name":"Minimal apilado","cat":"Simples","desc":"Horas sobre minutos en dígitos grandes, con fecha opcional.","params":[{"t":"paint","s":0,"l":"Dígitos","a":"#b388ff","b":"#22d3ee","d":1,"rb":false},{"t":"color","s":1,"l":"Fecha","a":"#7dd3fc"},{"t":"color","s":2,"l":"Fondo","a":"#000000"},{"t":"bool","o":0,"l":"Mostrar fecha","v":1},{"t":"select","o":1,"l":"Estilo de dígitos","opts":[[0,"7 segmentos"],[1,"Bloque"],[2,"Fino"]],"v":0}],"def":{"a":["#b388ff","#7dd3fc","#000000","#000000","#000000","#000000","#000000","#000000","#000000","#000000"],"b":["#22d3ee","#7dd3fc","#000000","#000000","#000000","#000000","#000000","#000000","#000000","#000000"],"d":[1,0,0,0,0,0,0,0,0,0],"o":[1,0,0,0,0,0,0,0]}},{"id":"marco-arcoiris","name":"Marco arcoíris","cat":"Simples","desc":"Dígitos apilados dentro de un marco en degradado.","params":[{"t":"paint","s":0,"l":"Dígitos","a":"#ffffff","b":"#ffffff","d":0,"rb":false},{"t":"paint","s":1,"l":"Marco","a":"#ff4d6d","b":"#4dabf7","d":4,"rb":true},{"t":"color","s":2,"l":"Fondo","a":"#000000"},{"t":"range","o":0,"l":"Grosor del marco","min":1,"max":3,"v":2},{"t":"select","o":1,"l":"Estilo de dígitos","opts":[[0,"7 segmentos"],[1,"Bloque"],[2,"Fino"]],"v":1}],"def":{"a":["#ffffff","#ff4d6d","#000000","#000000","#000000","#000000","#000000","#000000","#000000","#000000"],"b":["#ffffff","#4dabf7","#000000","#000000","#000000","#000000","#000000","#000000","#000000","#000000"],"d":[0,4,0,0,0,0,0,0,0,0],"o":[2,1,0,0,0,0,0,0]}},{"id":"segmentos-xl","name":"Segmentos XL","cat":"Simples","desc":"Hora gigante estilo LED con segmentos apagados visibles.","params":[{"t":"paint","s":0,"l":"Segmentos encendidos","a":"#4ade80","b":"#22d3ee","d":0,"rb":false},{"t":"color","s":1,"l":"Segmentos apagados","a":"#16225c"},{"t":"paint","s":2,"l":"Textos","a":"#4ade80","b":"#4ade80","d":0,"rb":false},{"t":"color","s":3,"l":"Fondo","a":"#000000"},{"t":"paint","s":4,"l":"Temperatura","a":"#4ade80","b":"#4ade80","d":0,"rb":false},{"t":"bool","o":0,"l":"Ícono Wi-Fi","v":1},{"t":"bool","o":1,"l":"Segmentos fantasma","v":1}],"def":{"a":["#4ade80","#16225c","#4ade80","#000000","#4ade80","#000000","#000000","#000000","#000000","#000000"],"b":["#22d3ee","#16225c","#4ade80","#000000","#4ade80","#000000","#000000","#000000","#000000","#000000"],"d":[0,0,0,0,0,0,0,0,0,0],"o":[1,1,0,0,0,0,0,0]}},{"id":"clima-4dias","name":"Clima 4 días","cat":"Clima","desc":"Fecha, hora, clima actual y pronóstico de 4 días.","params":[{"t":"color","s":0,"l":"Fecha","a":"#9ca3af"},{"t":"color","s":1,"l":"Hora","a":"#ffffff"},{"t":"color","s":2,"l":"Día","a":"#c084fc"},{"t":"paint","s":3,"l":"Temperatura","a":"#ffffff","b":"#ffffff","d":0,"rb":false},{"t":"color","s":4,"l":"Máxima","a":"#fdba74"},{"t":"color","s":5,"l":"Mínima","a":"#60a5fa"},{"t":"color","s":6,"l":"Humedad","a":"#67e8f9"},{"t":"color","s":7,"l":"Líneas","a":"#334155"},{"t":"color","s":8,"l":"Fondo","a":"#000000"},{"t":"color","s":9,"l":"Días del pronóstico","a":"#e5e7eb"},{"t":"select","o":0,"l":"Datos por día","opts":[[0,"Máxima y mínima"],[1,"Máxima y humedad"]],"v":0}],"def":{"a":["#9ca3af","#ffffff","#c084fc","#ffffff","#fdba74","#60a5fa","#67e8f9","#334155","#000000","#e5e7eb"],"b":["#9ca3af","#ffffff","#c084fc","#ffffff","#fdba74","#60a5fa","#67e8f9","#334155","#000000","#e5e7eb"],"d":[0,0,0,0,0,0,0,0,0,0],"o":[0,0,0,0,0,0,0,0]}},{"id":"mascotas-dia","name":"Mascotas","cat":"Divertidas","desc":"Dos gatos con sol de día y luna de noche, hora y día.","params":[{"t":"paint","s":0,"l":"Cielo","a":"#8fe3f5","b":"#d8f7ff","d":0,"rb":false},{"t":"paint","s":1,"l":"Cielo de noche","a":"#15206b","b":"#2b3a9c","d":0,"rb":false},{"t":"color","s":2,"l":"Pasto","a":"#7ed957"},{"t":"color","s":3,"l":"Marco","a":"#7ed957"},{"t":"paint","s":4,"l":"Hora","a":"#ffffff","b":"#ffffff","d":0,"rb":false},{"t":"paint","s":5,"l":"Día","a":"#ffffff","b":"#ffffff","d":0,"rb":false},{"t":"color","s":6,"l":"Gato naranja","a":"#f5a524"},{"t":"color","s":7,"l":"Gato negro","a":"#4a4e6e"},{"t":"color","s":8,"l":"Fondo del panel","a":"#000000"},{"t":"bool","o":0,"l":"Versión de noche al anochecer","v":1},{"t":"bool","o":1,"l":"Destellos","v":1}],"def":{"a":["#8fe3f5","#15206b","#7ed957","#7ed957","#ffffff","#ffffff","#f5a524","#4a4e6e","#000000","#000000"],"b":["#d8f7ff","#2b3a9c","#7ed957","#7ed957","#ffffff","#ffffff","#f5a524","#4a4e6e","#000000","#000000"],"d":[0,0,0,0,0,0,0,0,0,0],"o":[1,1,0,0,0,0,0,0]}},{"id":"mascotas-noche","name":"Mascotas de noche","cat":"Divertidas","desc":"Los gatos dormidos bajo las estrellas, con muñeco de nieve o luna.","params":[{"t":"paint","s":0,"l":"Cielo","a":"#15206b","b":"#0b1340","d":0,"rb":false},{"t":"color","s":1,"l":"Pasto","a":"#2f9e5a"},{"t":"color","s":2,"l":"Marco","a":"#4ade80"},{"t":"paint","s":3,"l":"Hora","a":"#e0f2fe","b":"#e0f2fe","d":0,"rb":false},{"t":"paint","s":4,"l":"Día","a":"#e0f2fe","b":"#e0f2fe","d":0,"rb":false},{"t":"color","s":5,"l":"Gato naranja","a":"#f5a524"},{"t":"color","s":6,"l":"Gato negro","a":"#4a4e6e"},{"t":"color","s":7,"l":"Estrellas","a":"#fff1a8"},{"t":"color","s":8,"l":"Fondo del panel","a":"#000000"},{"t":"select","o":0,"l":"Compañero","opts":[[0,"Muñeco de nieve"],[1,"Luna"]],"v":0}],"def":{"a":["#15206b","#2f9e5a","#4ade80","#e0f2fe","#e0f2fe","#f5a524","#4a4e6e","#fff1a8","#000000","#000000"],"b":["#0b1340","#2f9e5a","#4ade80","#e0f2fe","#e0f2fe","#f5a524","#4a4e6e","#fff1a8","#000000","#000000"],"d":[0,0,0,0,0,0,0,0,0,0],"o":[0,0,0,0,0,0,0,0]}},{"id":"tablero","name":"Tablero","cat":"Divertidas","desc":"Hora grande, fecha, tres imágenes tuyas y el clima.","params":[{"t":"color","s":0,"l":"Fondo","a":"#0e8a8a"},{"t":"paint","s":1,"l":"Hora","a":"#ffd23f","b":"#ff9f1c","d":0,"rb":false},{"t":"color","s":2,"l":"Franja","a":"#4ade80"},{"t":"color","s":3,"l":"Texto de la franja","a":"#064e3b"},{"t":"color","s":4,"l":"Marcos","a":"#e6fffb"},{"t":"color","s":5,"l":"Texto inferior","a":"#ffd23f"},{"t":"color","s":6,"l":"Recuadros inferiores","a":"#0b5e5e"},{"t":"images","l":"Imágenes de los recuadros"}],"def":{"a":["#0e8a8a","#ffd23f","#4ade80","#064e3b","#e6fffb","#ffd23f","#0b5e5e","#000000","#000000","#000000"],"b":["#0e8a8a","#ff9f1c","#4ade80","#064e3b","#e6fffb","#ffd23f","#0b5e5e","#000000","#000000","#000000"],"d":[0,0,0,0,0,0,0,0,0,0],"o":[0,0,0,0,0,0,0,0]}}];
const CLOCK_CATS=['Clásicos','Simples','Clima','Divertidas'];
const PRESETS=[['Neón','#22d3ee','#a855f7'],['Atardecer','#ff6b35','#ffd23f'],['Hielo','#e0f2fe','#38bdf8'],['Retro','#ff4d6d','#4dabf7'],['Bosque','#4ade80','#0e7490']];
const WX_ES={0:'Despejado',1:'Poco nublado',2:'Parcialmente nublado',3:'Nublado',45:'Niebla',48:'Niebla',51:'Llovizna',53:'Llovizna',55:'Llovizna',61:'Lluvia',63:'Lluvia',65:'Lluvia fuerte',71:'Nieve',73:'Nieve',75:'Nieve',80:'Chubascos',81:'Chubascos',82:'Chubascos',95:'Tormenta',96:'Tormenta',99:'Tormenta'};
let faceVals={},currentFace=null,simMode='auto',liveOnPanel=false,previewSeq=0,previewTimer=null,thumbTimers={},clockTick=null,clockBuilt=false,activeFace=null;
const faceById=id=>FACES.find(f=>f.id===id)||FACES[0];
function valsFor(id){if(!faceVals[id]){let saved=null;try{saved=JSON.parse(localStorage.getItem('ms.face.'+id))}catch(e){}const d=faceById(id).def;faceVals[id]=saved&&Array.isArray(saved.a)&&saved.a.length===10?saved:JSON.parse(JSON.stringify(d))}return faceVals[id]}
function persistVals(id){try{localStorage.setItem('ms.face.'+id,JSON.stringify(faceVals[id]))}catch(e){}}
function faceParams(id,extra){const v=valsFor(id),q=new URLSearchParams({face:id,h24:$('#clock24').checked?1:0,lang:$('#clockLang').value,blink:$('#clockBlink').checked?1:0});for(let i=0;i<10;i++){q.set('a'+i,v.a[i]);q.set('b'+i,v.b[i]);q.set('d'+i,v.d[i])}for(let i=0;i<8;i++)q.set('o'+i,v.o[i]);if(extra)for(const k in extra)q.set(k,extra[k]);return q}
async function fetchFacePixels(id,extra){const r=await fetch('/api/clock/preview',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:faceParams(id,extra)});if(!r.ok)throw Error('Vista previa '+r.status);const b=new Uint8Array(await r.arrayBuffer());if(b.length!==8192)throw Error('Vista previa incompleta');return b}
function paintLE(canvas,bytes,w,h){const x=canvas.getContext('2d'),im=x.createImageData(w,h);for(let p=0;p<w*h;p++){const v=bytes[p*2]|(bytes[p*2+1]<<8);im.data[p*4]=((v>>11)&31)*255/31;im.data[p*4+1]=((v>>5)&63)*255/63;im.data[p*4+2]=(v&31)*255/31;im.data[p*4+3]=255}x.putImageData(im,0,0)}
function schedulePreview(){clearTimeout(previewTimer);previewTimer=setTimeout(renderClockPreview,120)}
async function renderClockPreview(){if(!currentFace)return;const seq=++previewSeq;try{const px=await fetchFacePixels(currentFace,{night:simMode,panel:liveOnPanel?1:0});if(seq===previewSeq)paintLE($('#clockCanvas'),px,64,64)}catch(e){}}
function scheduleThumb(id){clearTimeout(thumbTimers[id]);thumbTimers[id]=setTimeout(()=>drawThumb(id),700)}
async function drawThumb(id){const c=$('#thumb-'+id);if(!c)return;try{paintLE(c,await fetchFacePixels(id,{night:simMode}),64,64)}catch(e){}}
async function drawAllThumbs(){for(const f of FACES){if(!$('#page-clock').classList.contains('active'))return;await drawThumb(f.id)}}

function renderFaceGallery(){$('#faceGallery').innerHTML=CLOCK_CATS.map(cat=>`<div class="face-group">${cat}</div><div class="face-grid">`+FACES.filter(f=>f.cat===cat).map(f=>`<button class="face-card" data-face="${f.id}" title="${esc(f.desc)}"><canvas id="thumb-${f.id}" width="64" height="64"></canvas><span>${esc(f.name)}</span>${f.id===activeFace?'<i class="face-live">En el panel</i>':''}</button>`).join('')+'</div>').join('');$$('.face-card').forEach(b=>{b.classList.toggle('active',b.dataset.face===currentFace);b.onclick=()=>selectFace(b.dataset.face)})}
function selectFace(id){currentFace=id;$$('.face-card').forEach(b=>b.classList.toggle('active',b.dataset.face===id));const f=faceById(id);$('#clockFaceTitle').textContent=f.name;$('#faceEditorTitle').textContent='Personalizar · '+f.name;$('#faceEditorDesc').textContent=f.desc;renderFaceEditor();schedulePreview();try{localStorage.setItem('ms.clockFace',id)}catch(e){}}

function colorPick(i,k,val){return `<div class="color-pick"><input type="color" data-i="${i}" data-k="${k}" value="${val}" aria-label="Color"><input class="input hex" data-i="${i}" data-k="${k}hex" value="${val}" maxlength="7" spellcheck="false" aria-label="Código de color"></div>`}
function paramHtml(p,i,v){
  if(p.t==='color')return `<div class="param-row"><span class="param-label">${esc(p.l)}</span>${colorPick(i,'a',v.a[p.s])}</div>`;
  if(p.t==='paint'){const d=v.d[p.s],grad=d>0,rb=d===4;const dirs=[[1,'Vertical'],[2,'Horizontal'],[3,'Diagonal']].concat(p.rb?[[4,'Arcoíris']]:[]);
    return `<div class="param-block"><div class="param-row"><span class="param-label">${esc(p.l)}</span>${rb?'<span class="param-sub">Arcoíris</span>':colorPick(i,'a',v.a[p.s])}</div>`+
    `<div class="param-row"><span class="param-sub">Degradado</span><label class="switch"><input type="checkbox" data-i="${i}" data-k="grad" ${grad?'checked':''}><span class="switch-track"></span></label></div>`+
    (grad?`<div class="param-row"><span class="param-sub">Dirección</span><select class="select select-sm" data-i="${i}" data-k="dir">${dirs.map(([k,l])=>`<option value="${k}" ${d===k?'selected':''}>${l}</option>`).join('')}</select></div>`+
      (rb?'':`<div class="param-row"><span class="param-sub">Segundo color</span>${colorPick(i,'b',v.b[p.s])}</div><div class="chips">${PRESETS.map((c,k)=>`<button class="chip" data-i="${i}" data-preset="${k}" style="--c1:${c[1]};--c2:${c[2]}">${c[0]}</button>`).join('')}</div>`):'')+`</div>`}
  if(p.t==='bool')return `<div class="switch-row"><span>${esc(p.l)}</span><label class="switch"><input type="checkbox" data-i="${i}" data-k="bool" ${v.o[p.o]?'checked':''}><span class="switch-track"></span></label></div>`;
  if(p.t==='select')return `<label class="field mt-3">${esc(p.l)}<select class="select mt-2" data-i="${i}" data-k="sel">${p.opts.map(([k,l])=>`<option value="${k}" ${v.o[p.o]==k?'selected':''}>${esc(l)}</option>`).join('')}</select></label>`;
  if(p.t==='range')return `<label class="field mt-3">${esc(p.l)}<div class="quick-range mt-2"><input type="range" min="${p.min}" max="${p.max}" step="1" data-i="${i}" data-k="range" value="${v.o[p.o]}"><span>${v.o[p.o]}</span></div></label>`;
  if(p.t==='images')return `<div class="field mt-3">${esc(p.l)}</div><div class="slot-grid mt-2">${[0,1,2].map(k=>`<div class="slot"><canvas class="slot-canvas" id="slot-${k}" width="18" height="18"></canvas><div class="slot-actions"><label class="btn btn-sm">Subir<input type="file" accept="image/*" class="file-native" data-slot="${k}"></label><button class="btn btn-sm" data-slot-gallery="${k}">Galería</button><button class="btn btn-sm btn-danger" data-slot-clear="${k}">Quitar</button></div></div>`).join('')}</div>`;
  return ''}
function renderFaceEditor(){const f=faceById(currentFace),v=valsFor(currentFace),colors=f.params.map((p,i)=>[p,i]).filter(([p])=>p.t==='color'||p.t==='paint'),other=f.params.map((p,i)=>[p,i]).filter(([p])=>p.t!=='color'&&p.t!=='paint');
  $('#faceEditor').innerHTML=(colors.length?`<details class="param-sec" open><summary>Colores</summary>${colors.map(([p,i])=>paramHtml(p,i,v)).join('')}</details>`:'')+(other.length?`<details class="param-sec" open><summary>Contenido</summary>${other.map(([p,i])=>paramHtml(p,i,v)).join('')}</details>`:'');
  if(f.params.some(p=>p.t==='images'))loadSlotThumbs()}
function onParam(e){const t=e.target,i=+t.dataset.i;if(t.dataset.i===undefined||isNaN(i))return;const k=t.dataset.k,f=faceById(currentFace),p=f.params[i],v=valsFor(currentFace);if(!p)return;
  if((t.type==='checkbox'||t.tagName==='SELECT')&&e.type==='input')return;
  if(k==='a'||k==='b'){v[k][p.s]=t.value;const h=t.parentElement.querySelector('.hex');if(h)h.value=t.value}
  else if(k==='ahex'||k==='bhex'){if(!/^#[0-9a-f]{6}$/i.test(t.value))return;v[k[0]][p.s]=t.value.toLowerCase();const c=t.parentElement.querySelector('input[type=color]');if(c)c.value=t.value.toLowerCase()}
  else if(k==='grad'){v.d[p.s]=t.checked?1:0;persistVals(currentFace);renderFaceEditor();schedulePreview();scheduleThumb(currentFace);return}
  else if(k==='dir'){v.d[p.s]=+t.value;persistVals(currentFace);renderFaceEditor();schedulePreview();scheduleThumb(currentFace);return}
  else if(k==='bool')v.o[p.o]=t.checked?1:0;
  else if(k==='sel')v.o[p.o]=+t.value;
  else if(k==='range'){v.o[p.o]=+t.value;t.nextElementSibling.textContent=t.value}
  else return;
  persistVals(currentFace);schedulePreview();scheduleThumb(currentFace)}
function onParamClick(e){const chip=e.target.closest('.chip');if(chip){const p=faceById(currentFace).params[+chip.dataset.i],v=valsFor(currentFace),c=PRESETS[+chip.dataset.preset];v.a[p.s]=c[1];v.b[p.s]=c[2];if(!v.d[p.s]||v.d[p.s]===4)v.d[p.s]=1;persistVals(currentFace);renderFaceEditor();schedulePreview();scheduleThumb(currentFace);return}
  const g=e.target.closest('[data-slot-gallery]');if(g){pickSlotFromGallery(+g.dataset.slotGallery);return}
  const c=e.target.closest('[data-slot-clear]');if(c){clearSlot(+c.dataset.slotClear)}}
$('#faceEditor').addEventListener('input',onParam);$('#faceEditor').addEventListener('change',onParam);$('#faceEditor').addEventListener('click',onParamClick);
$('#faceEditor').addEventListener('change',e=>{const t=e.target;if(t.dataset.slot===undefined||!t.files||!t.files[0])return;const k=+t.dataset.slot,img=new Image();img.onload=()=>{uploadSlot(k,toSlotBytes(img));URL.revokeObjectURL(img.src)};img.onerror=()=>toast('No pude leer esa imagen','err');img.src=URL.createObjectURL(t.files[0]);t.value=''});

// Pictures of the "Tablero" face: 18x18 RGB565 little endian in /config/slotN.rgb565
function toSlotBytes(src){const c=document.createElement('canvas');c.width=18;c.height=18;const x=c.getContext('2d'),sc=Math.max(18/src.width,18/src.height),w=src.width*sc,h=src.height*sc;x.imageSmoothingEnabled=true;x.drawImage(src,(18-w)/2,(18-h)/2,w,h);const d=x.getImageData(0,0,18,18).data,out=new Uint8Array(648);for(let p=0;p<324;p++){const v=((d[p*4]&248)<<8)|((d[p*4+1]&252)<<3)|(d[p*4+2]>>3);out[p*2]=v&255;out[p*2+1]=v>>8}return out}
function drawSlotThumb(k,bytes){const c=$('#slot-'+k);if(!c)return;if(!bytes||bytes.length!==648){c.getContext('2d').clearRect(0,0,18,18);c.classList.add('empty');return}c.classList.remove('empty');paintLE(c,bytes,18,18)}
async function loadSlotThumbs(){for(let k=0;k<3;k++){try{const r=await fetch('/api/fs/read?path='+encodeURIComponent('/config/slot'+k+'.rgb565'),{cache:'no-store'});drawSlotThumb(k,r.ok?new Uint8Array(await r.arrayBuffer()):null)}catch(e){drawSlotThumb(k,null)}}}
async function uploadSlot(k,bytes){try{await uploadXHR('/api/fs/upload?path='+encodeURIComponent('/config/slot'+k+'.rgb565'),'file',new Blob([bytes],{type:'application/octet-stream'}),'slot'+k+'.rgb565','Guardando imagen');drawSlotThumb(k,bytes);toast('Imagen '+(k+1)+' guardada');schedulePreview();scheduleThumb('tablero')}catch(e){toast(e.message,'err')}}
async function clearSlot(k){await fetch('/api/fs/delete?path='+encodeURIComponent('/config/slot'+k+'.rgb565'),{method:'DELETE'});drawSlotThumb(k,null);schedulePreview();scheduleThumb('tablero')}
async function pickSlotFromGallery(k){let d;try{d=await (await fetch('/api/gallery')).json()}catch(e){return toast('No pude leer la galería','err')}const imgs=d.images||[];if(!imgs.length)return toast('Tu galería no tiene imágenes todavía','err');
  const dlg=document.createElement('dialog');dlg.innerHTML=`<div class="dialog-head">Imagen ${k+1} desde la galería</div><div class="dialog-body"><div class="pick-grid">${imgs.map((it,i)=>`<button class="pick" data-i="${i}" disabled><canvas width="64" height="64"></canvas><span>${esc(it.name)}</span></button>`).join('')}</div></div><div class="dialog-foot"><button class="btn cancel">Cancelar</button></div>`;
  document.body.appendChild(dlg);dlg.showModal();const close=()=>{dlg.close();dlg.remove()};dlg.querySelector('.cancel').onclick=close;
  for(const b of dlg.querySelectorAll('.pick')){const it=imgs[+b.dataset.i];try{const u=new Uint8Array(await (await fetch('/api/fs/read?path='+encodeURIComponent('/gallery/images/'+it.name))).arrayBuffer());if(u.length!==8192)continue;const c=b.querySelector('canvas'),x=c.getContext('2d'),im=x.createImageData(64,64);for(let p=0;p<4096;p++){const v=(u[p*2]<<8)|u[p*2+1];im.data[p*4]=((v>>11)&31)*255/31;im.data[p*4+1]=((v>>5)&63)*255/63;im.data[p*4+2]=(v&31)*255/31;im.data[p*4+3]=255}x.putImageData(im,0,0);b.disabled=false;b.onclick=()=>{uploadSlot(k,toSlotBytes(c));close()}}catch(e){}if(!dlg.open)return}}

// Weather + location
function renderWx(s){const box=$('#wxNow');if(!box)return;const L=$('#clockLang').value==='1'?['SU','MO','TU','WE','TH','FR','SA']:['DO','LU','MA','MI','JU','VI','SA'];
  box.innerHTML=s.weatherValid?`<b>${Math.round(s.temp)}°C</b> · ${Math.round(s.humidity)}% · ${esc(WX_ES[s.weatherCode]||'—')} <span class="muted">${s.night?'· de noche':'· de día'}</span>`:'<span class="muted">Sin datos del clima todavía</span>';
  const wd=new Date().getDay();$('#wxDays').innerHTML=(s.days||[]).map((d,i)=>`<div class="wx-day"><b>${L[(wd+i)%7]}</b>${d.valid?`<span class="t-max">${Math.round(d.max)}°</span> <span class="t-min">${Math.round(d.min)}°</span><small>${Math.round(d.hum)}% · ${esc(WX_ES[d.code]||'')}</small>`:'<small class="muted">—</small>'}</div>`).join('')}
async function loadClockStatus(){try{const s=await (await fetch('/api/clock/status',{cache:'no-store'})).json();activeFace=s.enabled?s.face:null;
  $('#clockState').textContent=s.enabled?'En el panel: '+faceById(s.face).name:'Inactivo';$('#clockDot').classList.toggle('off',!s.enabled);
  $('#clock24').checked=!!s.h24;$('#clockBlink').checked=!!s.blink;$('#clockLang').value=String(s.lang||0);
  if(s.face&&FACES.some(f=>f.id===s.face)&&Array.isArray(s.a)){faceVals[s.face]={a:s.a.slice(0,10),b:s.b.slice(0,10),d:s.d.slice(0,10),o:s.o.slice(0,8)};persistVals(s.face)}
  if(!currentFace){let last=null;try{last=localStorage.getItem('ms.clockFace')}catch(e){}currentFace=(s.enabled&&s.face)||(FACES.some(f=>f.id===last)?last:FACES[0].id)}
  if(s.brightness)setBrightness(s.brightness);$('#cityName').textContent=s.city||'—';renderWx(s);return s}catch(e){toast('No pude leer el estado del reloj','err');return null}}
async function clockEnter(){await loadClockStatus();if(!currentFace)currentFace=FACES[0].id;renderFaceGallery();selectFace(currentFace);drawAllThumbs();clearInterval(clockTick);clockTick=setInterval(()=>{if($('#page-clock').classList.contains('active'))renderClockPreview()},5000)}
function clockLeave(){clearInterval(clockTick);clockTick=null;if(liveOnPanel){liveOnPanel=false;$('#clockLive').checked=false;fetch('/api/clock/preview/end',{method:'POST'})}}
async function citySearch(){const q=$('#citySearch').value.trim(),box=$('#cityResults');if(!q)return;box.innerHTML='<div class="muted">Buscando…</div>';
  try{const d=await (await fetch('https://geocoding-api.open-meteo.com/v1/search?count=6&language=es&format=json&name='+encodeURIComponent(q))).json(),res=d.results||[];
    box.innerHTML=res.length?res.map((c,i)=>`<button class="city-item" data-i="${i}"><b>${esc(c.name)}</b><span>${esc([c.admin1,c.country].filter(Boolean).join(', '))}</span></button>`).join(''):'<div class="muted">Sin resultados.</div>';
    box.querySelectorAll('.city-item').forEach(b=>b.onclick=()=>setCity(res[+b.dataset.i]))}
  catch(e){box.innerHTML='<div class="muted">No se pudo buscar. Este dispositivo necesita Internet para buscar la ciudad.</div>'}}
async function setCity(c){const r=await fetch('/api/location',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams({name:c.name,lat:c.latitude,lon:c.longitude})});if(!r.ok)return toast(await r.text(),'err');$('#cityName').textContent=c.name;$('#cityResults').innerHTML='';$('#citySearch').value='';toast('Ubicación: '+c.name+'. Actualizando el clima…');setTimeout(loadClockStatus,6000);setTimeout(loadClockStatus,15000)}
$('#citySearchBtn').onclick=citySearch;$('#citySearch').onkeydown=e=>{if(e.key==='Enter')citySearch()};
$('#refreshWeather').onclick=async()=>{await fetch('/api/clock/weather',{method:'POST'});toast('Consultando el clima…');setTimeout(loadClockStatus,5000)};
$$('#clockSim .seg').forEach(b=>b.onclick=()=>{simMode=b.dataset.sim;$$('#clockSim .seg').forEach(x=>x.classList.toggle('active',x===b));schedulePreview()});
$('#clockLive').onchange=e=>{liveOnPanel=e.target.checked;if(liveOnPanel){schedulePreview();toast('Mostrando la vista previa en el panel')}else fetch('/api/clock/preview/end',{method:'POST'})};
['clock24','clockBlink','clockLang'].forEach(id=>$('#'+id).onchange=()=>{schedulePreview();clearTimeout(thumbTimers._all);thumbTimers._all=setTimeout(drawAllThumbs,800)});
$('#resetFace').onclick=()=>{if(!currentFace)return;faceVals[currentFace]=JSON.parse(JSON.stringify(faceById(currentFace).def));persistVals(currentFace);renderFaceEditor();schedulePreview();scheduleThumb(currentFace);toast('Colores y opciones por defecto')};
$('#activateClock').onclick=async()=>{if(!currentFace)return;showLoader('Aplicando carátula','Enviando al panel…',55);const r=await fetch('/api/clock/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:faceParams(currentFace)});hideLoader();if(!r.ok)return toast(await r.text(),'err');liveOnPanel=false;$('#clockLive').checked=false;activeFace=currentFace;$('#clockState').textContent='En el panel: '+faceById(currentFace).name;$('#clockDot').classList.remove('off');renderFaceGallery();drawAllThumbs();toast(faceById(currentFace).name+' en el panel')};
$('#stopClock').onclick=async()=>{await fetch('/api/clock/stop',{method:'POST'});activeFace=null;$('#clockState').textContent='Inactivo';$('#clockDot').classList.add('off');renderFaceGallery();drawAllThumbs();toast('Modo reloj desactivado')};

// LIBRARY
const presets=[{name:'Corazón',draw:c=>{black(c);c.fillStyle='#ef3340';[[3,1],[4,1],[2,2],[3,2],[4,2],[5,2],[1,3],[2,3],[3,3],[4,3],[5,3],[6,3],[2,4],[3,4],[4,4],[5,4],[3,5],[4,5]].forEach(([x,y])=>c.fillRect(x*8,y*8,8,8))}},{name:'Carita',draw:c=>{black(c);c.fillStyle='#ffd84d';c.fillRect(8,8,48,48);c.fillStyle='#111';c.fillRect(20,22,6,6);c.fillRect(38,22,6,6);c.fillRect(20,42,24,5);c.fillRect(16,37,5,5);c.fillRect(43,37,5,5)}},{name:'Estrella',draw:c=>{black(c);c.fillStyle='#ffe347';const pts=[[32,5],[39,24],[59,24],[43,36],[49,57],[32,44],[15,57],[21,36],[5,24],[25,24]];c.beginPath();pts.forEach((p,i)=>i?c.lineTo(...p):c.moveTo(...p));c.closePath();c.fill()}},{name:'Nebulosa',draw:c=>{black(c);for(let y=0;y<64;y+=4)for(let x=0;x<64;x+=4){c.fillStyle=`hsl(${(x*4+y*2)%280+190} 85% ${25+((x+y)%20)}%)`;c.fillRect(x,y,4,4)};c.fillStyle='#fff';[[8,9],[49,12],[22,44],[57,51],[35,26]].forEach(p=>c.fillRect(...p,2,2))}},{name:'Flor',draw:c=>{black(c);c.fillStyle='#22c55e';c.fillRect(30,30,4,30);c.fillStyle='#ff78c6';[[30,14],[22,22],[38,22],[22,30],[38,30]].forEach(([x,y])=>c.fillRect(x,y,8,8));c.fillStyle='#ffd84d';c.fillRect(30,22,8,8)}},{name:'Robot',draw:c=>{black(c);c.fillStyle='#5ee7f7';c.fillRect(12,14,40,36);c.fillStyle='#07101d';c.fillRect(20,24,8,8);c.fillRect(36,24,8,8);c.fillRect(20,39,24,4);c.fillStyle='#ff6b35';c.fillRect(29,7,6,7)}}];
function loadPreset(p){const c=document.createElement('canvas');c.width=64;c.height=64;p.draw(c.getContext('2d'));px.putImageData(c.getContext('2d').getImageData(0,0,64,64),0,0);$$('.nav-btn').find(b=>b.dataset.page==='pixel').click();toast(`${p.name} cargado en Pixel Art`)}const lib=$('#builtInLibrary');presets.forEach(p=>{const d=document.createElement('div');d.className='library-card';const c=document.createElement('canvas');c.className='library-preview';c.width=64;c.height=64;p.draw(c.getContext('2d'));d.innerHTML=`<div class="font-bold mt-2">${p.name}</div><div class="muted text-xs mt-1">64×64 · original local</div>`;d.prepend(c);d.onclick=()=>loadPreset(p);lib.appendChild(d)});
$('#importPixilart').onclick=async()=>{const url=$('#pixilartUrl').value.trim();if(!url)return toast('Pega una URL pública de Pixilart','err');showLoader('Importando Pixilart','Resolviendo metadatos públicos…',12);try{const r=await fetch('/api/pixilart/import?url='+encodeURIComponent(url),{method:'POST'});const data=await r.json().catch(()=>({}));if(!r.ok)throw Error(data.error||'No se pudo importar');updateLoader('Cargando imagen importada…',78);const im=new Image();im.crossOrigin='anonymous';im.onload=()=>{img=im;drawImage();hideLoader();$$('.nav-btn').find(b=>b.dataset.page==='image').click();toast('Obra importada a Imagen')};im.onerror=()=>{hideLoader();toast('Se guardó pero no se pudo previsualizar','err')};im.src=data.localUrl+'?t='+Date.now()}catch(e){hideLoader();toast(e.message,'err')}};

// GALLERY + ADMIN + OTA
async function loadGallery(){const box=$('#galleryList');box.innerHTML='<div class="muted">Cargando…</div>';const r=await fetch('/api/gallery');const d=await r.json();box.innerHTML='';const all=[...(d.images||[]).map(x=>({...x,type:'image'})),...(d.animations||[]).map(x=>({...x,type:'animation'}))];if(!all.length){box.innerHTML='<div class="muted">Sin contenido.</div>';return}for(const it of all){const e=document.createElement('div');e.className='library-card';e.innerHTML=`<div class="font-bold">${it.name}</div><div class="muted text-xs mt-1">${it.type} · ${it.size} B</div><div class="toolbar mt-3"><button class="btn play">${it.type==='image'?'Mostrar':'Reproducir'}</button><button class="btn btn-danger del">Eliminar</button></div>`;e.querySelector('.play').onclick=()=>fetch((it.type==='image'?'/api/gallery/show-image?name=':'/api/gallery/play-animation?name=')+encodeURIComponent(it.name),{method:'POST'});e.querySelector('.del').onclick=async()=>{const v=await modal('Eliminar',`¿Eliminar <b>${it.name}</b>?`,'Eliminar');if(v!==null){await fetch('/api/gallery/delete?type='+it.type+'&name='+encodeURIComponent(it.name),{method:'DELETE'});loadGallery()}};box.appendChild(e)}}$('#reloadGallery').onclick=loadGallery;
$('#adminUpload').onchange=e=>$('#adminFileName').textContent=[...e.target.files].map(f=>f.name).join(', ')||'Ningún archivo';async function listFiles(){const path=$('#adminPath').value||'/';const r=await fetch('/api/fs/list?path='+encodeURIComponent(path));const d=await r.json();let h='<table class="table"><tr><th>Nombre</th><th>Tipo</th><th>Tamaño</th><th></th></tr>';for(const it of d.items)h+=`<tr><td>${it.name}</td><td>${it.dir?'carpeta':'archivo'}</td><td>${it.dir?'':it.size}</td><td><button class="btn btn-danger fdel" data-path="${it.path}">Eliminar</button></td></tr>`;h+='</table>';$('#fileList').innerHTML=h;$$('.fdel').forEach(b=>b.onclick=async()=>{const v=await modal('Eliminar',`¿Eliminar <b>${b.dataset.path}</b>?`,'Eliminar');if(v!==null){await fetch('/api/fs/delete?path='+encodeURIComponent(b.dataset.path),{method:'DELETE'});listFiles()}})}$('#listFiles').onclick=listFiles;$('#mkdir').onclick=async()=>{const v=await modal('Crear carpeta','<label class="field">Nombre<input class="input" name="name"></label>','Crear');if(!v)return;const base=$('#adminPath').value.replace(/\/$/,'');await fetch('/api/fs/mkdir?path='+encodeURIComponent(base+'/'+v.name),{method:'POST'});listFiles()};$('#uploadFiles').onclick=async()=>{const files=[...$('#adminUpload').files];if(!files.length)return toast('Selecciona archivos','err');const base=$('#adminPath').value.replace(/\/$/,'');showLoader('Subiendo archivos','Preparando…',5);try{let i=0;for(const f of files){i++;await uploadXHR('/api/fs/upload?path='+encodeURIComponent(base+'/'+f.name),'file',f,f.name,`Subiendo ${i}/${files.length}`)}hideLoader();toast('Archivos subidos');listFiles()}catch(e){hideLoader();toast(e.message,'err')}};
$('#fwFile').onchange=e=>$('#fwFileName').textContent=e.target.files[0]?.name||'Ningún firmware';$('#fwUpload').onclick=async()=>{const f=$('#fwFile').files[0];if(!f)return toast('Selecciona un .bin','err');const v=await modal('Actualizar firmware',`Se instalará <b>${f.name}</b> y el ESP32 reiniciará. No desconectes alimentación.`,'Instalar');if(v===null)return;try{await uploadXHR('/api/firmware','firmware',f,f.name,'Actualizando firmware');toast('Firmware instalado; reiniciando…');setTimeout(()=>location.href='http://matrix.local',9000)}catch(e){toast(e.message,'err')}};
// ---- Estado ----
// Live status (/api/status every 3 s) and the serial monitor (/api/log every
// 1.5 s) poll only while the Estado page is open. Elsewhere a 10 s heartbeat
// keeps the online/offline dot in the navigation up to date.
let statusTimer=null,logTimer=null,logSince=0,logPaused=false,logLines=[];
const fmtBytes=b=>b>=1048576?(b/1048576).toFixed(1)+' MB':Math.round(b/1024)+' KB';
const fmtUptime=s=>{const d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60),x=s%60;return (d?d+'d ':'')+[h,m,x].map(v=>String(v).padStart(2,'0')).join(':')};
const fmtMs=ms=>fmtUptime(Math.floor(ms/1000))+'.'+String(ms%1000).padStart(3,'0');
const signal=r=>r>=-55?'Excelente':r>=-67?'Buena':r>=-75?'Regular':'Débil';
const esc=t=>String(t??'').replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
function setOnline(on){$$('.nav-dot').forEach(d=>d.classList.toggle('off',!on));$('#moreBadge').classList.toggle('hidden',on);$('#statusConn').textContent=on?'En línea':'Sin conexión';$('#statusDot').classList.toggle('off',!on)}
async function loadStatus(){try{const s=await (await fetch('/api/status',{cache:'no-store'})).json();setOnline(true);renderStatus(s);return s}catch(e){setOnline(false);return null}}
const meter=p=>`<div class="meter"><span style="width:${Math.max(0,Math.min(100,p))}%"></span></div>`;
function showingText(s){if(s.stateKind==='frame')return 'Imagen fija';if(s.stateKind==='anim')return (s.stateAnim.includes('/gifs/')?'GIF':'Animación')+' · '+s.stateAnim.split('/').pop().replace(/\.pma$/,'');if(s.stateKind==='clock')return 'Modo reloj';return 'Pantalla inicial'}
function renderStatus(s){
  if(!$('#page-status').classList.contains('active'))return;
  const row=(k,v)=>`<div class="kv"><span>${k}</span><b>${v}</b></div>`;
  const card=(t,ic,body)=>`<div class="card status-card"><div class="status-title">${icon(ic)}<span>${t}</span></div>${body}</div>`;
  const sdPct=s.sdTotalMB?s.sdUsedMB/s.sdTotalMB*100:0,heapPct=s.heapSize?(s.heapSize-s.heapFree)/s.heapSize*100:0,fwPct=s.sketchFree?s.sketchSize/s.sketchFree*100:0;
  $('#statusGrid').innerHTML=
    card('Conexión','status',row('IP',esc(s.ip))+row('Red Wi-Fi',esc(s.ssid||'—'))+row('Señal',`${s.rssi} dBm · ${signal(s.rssi)}`)+row('mDNS',esc(s.host))+row('MAC',esc(s.mac)))+
    card('Pantalla','image',row('Mostrando',esc(showingText(s)))+(s.animPlaying?row('Frames',s.animFrames):'')+row('Brillo',s.brightness)+row('Reloj',s.clockEnabled?'Activo':'Inactivo')+row('Último render',s.lastRenderMs+' ms'))+
    card('Sistema','panel',row('Encendido hace',fmtUptime(s.uptimeS))+row('Último reinicio',esc(s.resetReason))+row('RAM libre',`${fmtBytes(s.heapFree)} de ${fmtBytes(s.heapSize)}`)+meter(heapPct)+row('RAM mínima',fmtBytes(s.heapMin))+row('PSRAM libre',s.psramSize?`${fmtBytes(s.psramFree)} de ${fmtBytes(s.psramSize)}`:'No detectada')+row('Chip',`${esc(s.chip)} · ${s.cpuMHz} MHz`))+
    card('Almacenamiento','admin',row('microSD',`${s.sdUsedMB} / ${s.sdTotalMB} MB`)+meter(sdPct)+row('Firmware',`${fmtBytes(s.sketchSize)} de ${fmtBytes(s.sketchFree)} (slot OTA)`)+meter(fwPct)+row('Web assets','v'+esc(s.webVersion))+row('Compilado',esc(s.build)))+
    card('Clima','clock',row('Ciudad',esc(s.city||'—'))+(s.weatherValid?row('Temperatura',s.temp+' °C')+row('Humedad',s.humidity+'%'):row('Estado','Sin datos todavía')));
}
const logClass=m=>/ERROR|fallido|panic/i.test(m)?'err':/ADVERTENCIA|perdido|reinici/i.test(m)?'warn':/ OK|listo|correctamente|restaurado|reconectado|instalado|actualizado/i.test(m)?'ok':'';
function renderLog(){
  const box=$('#logBox'),atBottom=box.scrollHeight-box.scrollTop-box.clientHeight<40;
  box.innerHTML=logLines.length?logLines.map(l=>`<div class="ln ${l.c}">${l.t===null?'':`<span class="ts">${fmtMs(l.t)}</span>`}${esc(l.m)||'&nbsp;'}</div>`).join(''):'<div class="ts">Esperando mensajes del ESP32…</div>';
  if(atBottom)box.scrollTop=box.scrollHeight;
}
async function pollLog(){
  if(logPaused)return;
  try{
    const d=await (await fetch('/api/log?since='+logSince,{cache:'no-store'})).json();
    if(d.next<logSince){logSince=0;logLines.push({t:null,m:'— El ESP32 se reinició —',c:'warn'});renderLog();return pollLog()}
    for(const l of d.lines){logLines.push({t:l.t,m:l.m,c:logClass(l.m)});logSince=l.s+1}
    if(logLines.length>400)logLines.splice(0,logLines.length-400);
    renderLog();
  }catch(e){}
}
function statusStart(){statusStop();renderLog();loadStatus();pollLog();statusTimer=setInterval(loadStatus,3000);logTimer=setInterval(pollLog,1500)}
function statusStop(){clearInterval(statusTimer);clearInterval(logTimer);statusTimer=logTimer=null}
function copyText(t){const fallback=()=>{const ta=document.createElement('textarea');ta.value=t;ta.style.position='fixed';ta.style.opacity='0';document.body.appendChild(ta);ta.select();try{document.execCommand('copy');toast('Log copiado')}catch(e){toast('No se pudo copiar','err')}ta.remove()};if(navigator.clipboard&&window.isSecureContext)navigator.clipboard.writeText(t).then(()=>toast('Log copiado'),fallback);else fallback()}
$('#logPause').onclick=()=>{logPaused=!logPaused;$('#logPause').textContent=logPaused?'Reanudar':'Pausar';if(!logPaused)pollLog()};
$('#logClear').onclick=()=>{logLines=[];renderLog()};
$('#logCopy').onclick=()=>copyText(logLines.map(l=>(l.t===null?'':'['+fmtMs(l.t)+'] ')+l.m).join('\n'));
loadClockStatus();

// ---- GIF's ----
// GIFs are decoded in the browser with a self-contained GIF89a decoder (LZW +
// frame disposal). This works over plain HTTP; the WebCodecs ImageDecoder API
// is only available in secure contexts, so it is not used here. Each frame is
// fitted to 64x64 and packed into PMA2. The panel never decodes GIFs itself.
const GIF_MAX_FRAMES=300;
let gifFrames=null,gifFile=null,gifSelected=null,gifTimer=null;
const gifCtx=$('#gifCanvas').getContext('2d',{willReadFrequently:true});

function gifFitRect(sw,sh,mode){
  if(mode==='stretch')return{dx:0,dy:0,dw:64,dh:64};
  const s=mode==='cover'?Math.max(64/sw,64/sh):Math.min(64/sw,64/sh);
  const dw=Math.max(1,Math.round(sw*s)),dh=Math.max(1,Math.round(sh*s));
  return{dx:Math.round((64-dw)/2),dy:Math.round((64-dh)/2),dw,dh};
}
// GIF LZW: decodes one image's index stream into iw*ih palette indices.
function gifLzw(minCodeSize,data,npix){
  const clear=1<<minCodeSize,eoi=clear+1;
  const prefix=new Int16Array(4096),suffix=new Uint8Array(4096),stack=new Uint8Array(4097),out=new Uint8Array(npix);
  for(let c=0;c<clear;c++){prefix[c]=0;suffix[c]=c;}
  let codeSize=minCodeSize+1,mask=(1<<codeSize)-1,available=clear+2,old=-1,first=0;
  let datum=0,bits=0,dp=0,top=0,i=0;
  while(i<npix){
    if(top===0){
      if(bits<codeSize){if(dp>=data.length)break;datum+=data[dp++]<<bits;bits+=8;continue;}
      let code=datum&mask;datum>>=codeSize;bits-=codeSize;
      if(code>available||code===eoi)break;
      if(code===clear){codeSize=minCodeSize+1;mask=(1<<codeSize)-1;available=clear+2;old=-1;continue;}
      if(old===-1){stack[top++]=suffix[code];old=code;first=code;continue;}
      const inCode=code;
      if(code===available){stack[top++]=first;code=old;}
      while(code>clear){stack[top++]=suffix[code];code=prefix[code];}
      first=suffix[code]&0xff;stack[top++]=first;
      if(available<4096){prefix[available]=old;suffix[available]=first;available++;if((available&mask)===0&&available<4096){codeSize++;mask+=available;}}
      old=inCode;
    }
    top--;out[i++]=stack[top];
  }
  return out;
}
// Full GIF89a parse: composites frames (disposal-aware), fits each to 64x64 and
// returns [{data:RGB565(8192), delay:ms}].
async function gifDecode(file,mode){
  const b=new Uint8Array(await file.arrayBuffer());let p=0;
  if(b[0]!==71||b[1]!==73||b[2]!==70)throw new Error('Archivo GIF inválido');
  p=6;
  const rd16=()=>{const v=b[p]|(b[p+1]<<8);p+=2;return v};
  const W=rd16(),H=rd16();const scr=b[p++];p+=2;
  let gct=null;if(scr&0x80){const n=2<<(scr&7);gct=b.subarray(p,p+n*3);p+=n*3;}
  if(!W||!H)throw new Error('GIF sin dimensiones');
  const canvas=new Uint8ClampedArray(W*H*4);
  const src=document.createElement('canvas');src.width=W;src.height=H;const sctx=src.getContext('2d');
  const tmp=document.createElement('canvas');tmp.width=64;tmp.height=64;const t=tmp.getContext('2d',{willReadFrequently:true});
  const rect=gifFitRect(W,H,mode);
  let saved=null,prevDisp=0,pl=0,pt=0,pw=0,ph=0;
  let delay=0,tflag=false,tindex=0,disposal=0;
  const out=[];
  while(p<b.length){
    const blk=b[p++];
    if(blk===0x3B)break;
    if(blk===0x21){
      const label=b[p++];
      if(label===0xF9){const sz=b[p++];const pk=b[p];delay=b[p+1]|(b[p+2]<<8);tindex=b[p+3];tflag=(pk&1)===1;disposal=(pk>>2)&7;p+=sz;p++;}
      else{while(b[p]!==0)p+=b[p]+1;p++;}
    }else if(blk===0x2C){
      const left=rd16(),top2=rd16(),iw=rd16(),ih=rd16();const ip=b[p++];
      let ct=gct;if(ip&0x80){const n=2<<(ip&7);ct=b.subarray(p,p+n*3);p+=n*3;}
      const interlace=(ip&0x40)!==0;const minCode=b[p++];
      let size=0,q=p;while(b[q]!==0){size+=b[q];q+=b[q]+1;}
      const data=new Uint8Array(size);let o=0,r=p;while(b[r]!==0){const c=b[r++];for(let k=0;k<c;k++)data[o++]=b[r++];}p=r+1;
      if(!ct){tflag=false;disposal=0;delay=0;continue;}
      const idx=gifLzw(minCode,data,iw*ih);
      if(prevDisp===2){for(let y=0;y<ph;y++)for(let x=0;x<pw;x++){const o4=((pt+y)*W+(pl+x))*4;canvas[o4]=canvas[o4+1]=canvas[o4+2]=canvas[o4+3]=0;}}
      else if(prevDisp===3&&saved)canvas.set(saved);
      if(disposal===3)saved=canvas.slice();
      const rows=new Int32Array(ih);
      if(interlace){let j=0;const passes=[[0,8],[4,8],[2,4],[1,2]];for(const pa of passes)for(let y=pa[0];y<ih;y+=pa[1])rows[j++]=y;}else for(let y=0;y<ih;y++)rows[y]=y;
      for(let sy=0;sy<ih;sy++){const y=rows[sy];for(let x=0;x<iw;x++){const ci=idx[sy*iw+x];if(tflag&&ci===tindex)continue;const cy=top2+y,cx=left+x;if(cy<0||cx<0||cy>=H||cx>=W)continue;const o4=(cy*W+cx)*4,c3=ci*3;canvas[o4]=ct[c3];canvas[o4+1]=ct[c3+1];canvas[o4+2]=ct[c3+2];canvas[o4+3]=255}}
      sctx.putImageData(new ImageData(canvas,W,H),0,0);
      t.fillStyle='#000';t.fillRect(0,0,64,64);t.imageSmoothingEnabled=mode!=='stretch';t.drawImage(src,rect.dx,rect.dy,rect.dw,rect.dh);
      let ms=delay*10;if(!ms||ms<20)ms=100;if(ms>5000)ms=5000;
      out.push({data:to565(tmp),delay:ms});
      prevDisp=disposal;pl=left;pt=top2;pw=iw;ph=ih;tflag=false;disposal=0;delay=0;
      if(out.length>=GIF_MAX_FRAMES)break;
    }else break;
  }
  if(!out.length)throw new Error('El GIF no tiene frames legibles');
  return out;
}
function gifBuildPma2(frames){
  const per=2+8192,out=new Uint8Array(10+frames.length*per),dv=new DataView(out.buffer);
  out[0]=80;out[1]=77;out[2]=65;out[3]=50; // "PMA2"
  dv.setUint16(4,64,true);dv.setUint16(6,64,true);dv.setUint16(8,frames.length,true);
  let off=10;for(const f of frames){dv.setUint16(off,f.delay,true);off+=2;out.set(f.data,off);off+=8192}
  return out;
}
function gifPlayPreview(frames){
  if(gifTimer)clearTimeout(gifTimer);let i=0;
  const step=()=>{
    const f=frames[i],im=gifCtx.createImageData(64,64);
    for(let p=0;p<4096;p++){const v=(f.data[p*2]<<8)|f.data[p*2+1];im.data[p*4]=((v>>11)&31)<<3;im.data[p*4+1]=((v>>5)&63)<<2;im.data[p*4+2]=(v&31)<<3;im.data[p*4+3]=255}
    gifCtx.putImageData(im,0,0);i=(i+1)%frames.length;gifTimer=setTimeout(step,f.delay);
  };step();
}
async function gifRefresh(file){
  gifFile=file;gifFrames=null;$('#gifAdd').disabled=true;$('#gifInfo').textContent='Decodificando…';
  try{const frames=await gifDecode(file,$('#gifFit').value);gifFrames=frames;$('#gifInfo').textContent=`${frames.length} frames · listo para agregar`;$('#gifAdd').disabled=false;gifPlayPreview(frames)}
  catch(e){$('#gifInfo').textContent=e.message;toast(e.message,'err')}
}
$('#gifFile').onchange=e=>{const f=e.target.files[0];if(!f)return;$('#gifFileName').textContent=f.name;$('#gifName').value=f.name.replace(/\.gif$/i,'');gifRefresh(f)};
$('#gifFit').onchange=()=>{if(gifFile)gifRefresh(gifFile)};

// ---- Giphy search ----
// Runs in the browser (the client has Internet); the ESP32 is not involved.
// A picked GIF is downloaded as a Blob and goes through the same decode flow.
const GIPHY_API_KEY='MnOOWuodMfdUZSUKd3fOiYJNakT0yHtp';
let giphyOffset=0,giphyTotal=0,giphyLoaded=false,giphyBusy=false;
$$('.gif-tab').forEach(b=>b.onclick=()=>{
  $$('.gif-tab').forEach(x=>x.classList.toggle('btn-primary',x===b));
  $('#gifTabUpload').classList.toggle('hidden',b.dataset.tab!=='upload');
  $('#gifTabGiphy').classList.toggle('hidden',b.dataset.tab!=='giphy');
  if(b.dataset.tab==='giphy'&&!giphyLoaded)giphyLoad(true);
});
async function giphyLoad(reset){
  if(giphyBusy)return;giphyBusy=true;
  const q=$('#giphyQuery').value.trim(),grid=$('#giphyGrid'),box=$('#giphyScroll'),more=$('#giphyMore');
  if(reset){giphyOffset=0;grid.innerHTML='<div class="muted">Cargando…</div>';more.classList.add('hidden');box.scrollTop=0}
  more.disabled=true;more.textContent='Cargando…';
  const url='https://api.giphy.com/v1/gifs/'+(q?'search':'trending')+'?api_key='+GIPHY_API_KEY+'&limit=24&offset='+giphyOffset+(q?'&lang=es&q='+encodeURIComponent(q):'');
  try{
    const r=await fetch(url),d=await r.json();
    if(!r.ok)throw Error((d.meta&&d.meta.msg)||('Giphy '+r.status));
    const items=d.data||[];
    if(reset)grid.innerHTML=items.length?'':'<div class="muted">Sin resultados.</div>';
    giphyLoaded=true;
    let firstNew=null;
    for(const g of items){
      const im=g.images||{},prev=im.fixed_width_small||im.fixed_height_small||im.fixed_height;
      if(!prev)continue;
      const el=document.createElement('img');el.alt=g.title||'';el.src=prev.webp||prev.url;
      el.onclick=()=>giphyPick(g,el);grid.appendChild(el);if(!firstNew)firstNew=el;
    }
    giphyOffset+=items.length;giphyTotal=(d.pagination&&d.pagination.total_count)||0;
    more.classList.toggle('hidden',!(items.length&&giphyOffset<giphyTotal&&giphyOffset<4999));
    if(!reset&&firstNew)box.scrollTo({top:Math.max(0,firstNew.offsetTop-8),behavior:'smooth'});
  }catch(e){if(reset)grid.innerHTML='<div class="muted">No se pudo consultar Giphy.</div>';toast(e.message,'err')}
  finally{giphyBusy=false;more.disabled=false;more.textContent='Cargar más'}
}
async function giphyPick(g,el){
  $$('#giphyGrid img').forEach(x=>x.classList.remove('active'));el.classList.add('active');
  const im=g.images||{},src=(im.fixed_height&&im.fixed_height.url)||(im.downsized&&im.downsized.url)||(im.original&&im.original.url);
  if(!src)return toast('Ese GIF no tiene versión descargable','err');
  $('#gifInfo').textContent='Descargando de Giphy…';$('#gifAdd').disabled=true;
  try{
    const r=await fetch(src);if(!r.ok)throw Error('Giphy '+r.status);
    const blob=await r.blob();
    const title=((g.title||g.slug||'giphy').replace(/\s*GIF(\s+by\s+.*)?$/i,'').trim()||'giphy').slice(0,40);
    $('#gifName').value=title;$('#gifFileName').textContent='Giphy: '+title;
    await gifRefresh(blob);
  }catch(e){$('#gifInfo').textContent=e.message;toast(e.message,'err')}
}
$('#giphySearch').onclick=()=>giphyLoad(true);
$('#giphyQuery').onkeydown=e=>{if(e.key==='Enter')giphyLoad(true)};
$('#giphyMore').onclick=()=>giphyLoad(false);
$('#gifAdd').onclick=async()=>{
  if(!gifFrames||!gifFile)return toast('Elige un GIF','err');
  let base=($('#gifName').value||'gif').trim().replace(/[^a-zA-Z0-9_-]/g,'_')||'gif';
  const pma=gifBuildPma2(gifFrames);
  try{
    await uploadXHR('/api/fs/upload?path='+encodeURIComponent('/gallery/gifs/'+base+'.gif'),'file',gifFile,base+'.gif','Subiendo GIF original');
    await uploadXHR('/api/fs/upload?path='+encodeURIComponent('/gallery/gifs/'+base+'.pma'),'file',new Blob([pma],{type:'application/octet-stream'}),base+'.pma','Guardando frames');
    toast('GIF agregado al historial');loadGifs();
  }catch(e){toast(e.message,'err')}
};
async function loadGifs(){
  const box=$('#gifGrid');box.innerHTML='<div class="muted">Cargando…</div>';
  gifSelected=null;$('#gifLoad').disabled=true;$('#gifDelete').disabled=true;
  try{
    const d=await (await fetch('/api/gifs')).json();box.innerHTML='';
    if(!d.gifs||!d.gifs.length){box.innerHTML='<div class="muted">Sin GIFs todavía.</div>';return}
    for(const g of d.gifs){
      const card=document.createElement('div');card.className='library-card';
      const img=document.createElement('img');img.className='library-preview';img.loading='lazy';img.src='/gifs?name='+encodeURIComponent(g.name+'.gif')+'&t='+Date.now();
      card.appendChild(img);
      card.insertAdjacentHTML('beforeend',`<div class="font-bold mt-2">${g.name}</div><div class="muted text-xs mt-1">${Math.round(g.size/1024)} KB</div>`);
      card.onclick=()=>{$$('#gifGrid .library-card').forEach(c=>c.classList.remove('active'));card.classList.add('active');gifSelected=g.name;$('#gifLoad').disabled=false;$('#gifDelete').disabled=false};
      box.appendChild(card);
    }
  }catch(e){box.innerHTML='<div class="muted">No se pudo cargar el historial.</div>'}
}
$('#gifReload').onclick=loadGifs;
$('#gifLoad').onclick=async()=>{if(!gifSelected)return;try{const r=await fetch('/api/gifs/play?name='+encodeURIComponent(gifSelected),{method:'POST'});if(!r.ok)throw Error('No se pudo cargar');toast('GIF cargado al panel')}catch(e){toast(e.message,'err')}};
$('#gifDelete').onclick=async()=>{if(!gifSelected)return;const v=await modal('Borrar GIF',`¿Eliminar <b>${gifSelected}</b> de la microSD?`,'Borrar');if(v!==null){await fetch('/api/gifs/delete?name='+encodeURIComponent(gifSelected),{method:'DELETE'});loadGifs()}};

// ---- Start ----
go(location.hash.slice(1)||(()=>{try{return localStorage.getItem('ms.page')}catch(e){return null}})()||'image');
history.replaceState(null,'','#'+currentPage);
loadStatus();setInterval(()=>{if(currentPage!=='status')loadStatus()},10000);
)JS";

// =========================
// Helpers
// =========================
String safePath(String p) {
  p.trim();
  if (!p.startsWith("/")) p = "/" + p;
  while (p.indexOf("..") >= 0) p.replace("..", "");
  return p;
}
String safeName(String s) {
  s.trim();
  String o;
  for (char c: s) {
    if (isalnum(c) || c=='_' || c=='-' || c=='.') o += c;
    else if (c==' ') o += '_';
  }
  if (o.length()==0) o="item";
  return o;
}
bool ensureDir(const char* p){ return SD_MMC.exists(p) || SD_MMC.mkdir(p); }

bool writeText(const char* path, const char* src){
  File f=SD_MMC.open(path, FILE_WRITE);
  if(!f) return false;
  size_t len=strlen_P(src), off=0;
  char buf[512];
  while(off<len){
    size_t n=min((size_t)511,len-off);
    memcpy_P(buf,src+off,n);
    if(f.write((uint8_t*)buf,n)!=n){f.close();return false;}
    off+=n;
  }
  f.close(); return true;
}

const char* WEB_ASSET_VERSION = "5.7";

void provisionWeb(){
  ensureDir(WWW_DIR);
  ensureDir("/gallery");
  ensureDir(IMAGE_DIR);
  ensureDir(ANIM_DIR);
  ensureDir(GIF_DIR);
  ensureDir(REMOTE_DIR);

  String installedVersion = "";
  if (SD_MMC.exists("/www/.version")) {
    File vf = SD_MMC.open("/www/.version", FILE_READ);
    if (vf) {
      installedVersion = vf.readString();
      installedVersion.trim();
      vf.close();
    }
  }

  bool needsUpdate =
    installedVersion != WEB_ASSET_VERSION ||
    !SD_MMC.exists("/www/index.html") ||
    !SD_MMC.exists("/www/tailwind.css") ||
    !SD_MMC.exists("/www/app.js");

  if (!needsUpdate) {
    Log.println("Web assets OK: v" + installedVersion);
    return;
  }

  Log.println("Actualizando web assets en microSD a v" + String(WEB_ASSET_VERSION));

  SD_MMC.remove("/www/index.html");
  SD_MMC.remove("/www/app.css");
  SD_MMC.remove("/www/tailwind.css");
  SD_MMC.remove("/www/app.js");
  SD_MMC.remove("/www/.version");

  bool ok =
    writeText("/www/index.html", INDEX_HTML) &&
    writeText("/www/tailwind.css", TAILWIND_CSS) &&
    writeText("/www/app.js", APP_JS);

  if (ok) {
    File vf = SD_MMC.open("/www/.version", FILE_WRITE);
    if (vf) {
      vf.print(WEB_ASSET_VERSION);
      vf.close();
    }
    Log.println("Web assets actualizados correctamente.");
  } else {
    Log.println("ERROR actualizando web assets.");
  }
}

bool serveFile(const String& p){
  if(!SD_MMC.exists(p)) return false;
  File f=SD_MMC.open(p,FILE_READ); if(!f||f.isDirectory()) return false;
  String ct="application/octet-stream";
  if(p.endsWith(".html"))ct="text/html"; else if(p.endsWith(".css"))ct="text/css"; else if(p.endsWith(".js"))ct="application/javascript"; else if(p.endsWith(".json"))ct="application/json";
  server.streamFile(f,ct); f.close(); return true;
}

// Explicit forward declarations.
// Arduino's automatic prototype generator can miss functions in large .ino files
// containing embedded HTML/JS raw strings, so keep these declarations manual.
void stopClock();
void serviceClock();
void renderClock();

void stopAnim(){animationPlaying=false;if(animFile)animFile.close();}
void applyFrame(){
  uint32_t started = millis();
  display.clearDisplay();
  size_t i=0;
  for(int y=0;y<64;y++)for(int x=0;x<64;x++){
    uint16_t c=((uint16_t)frameBuffer[i]<<8)|frameBuffer[i+1];
    display.drawPixelRGB565(x,y,c);
    i+=2;
  }
  display.showBuffer();
  lastRenderMs = millis() - started;
}

uint16_t read16(File& f){int a=f.read(),b=f.read();return (a<0||b<0)?0:(uint16_t)a|((uint16_t)b<<8);}
// Animation playback supports two on-disk formats:
//   PMA1: "PMA1" + w + h + frames + delay        (single global delay, legacy)
//   PMA2: "PMA2" + w + h + frames, then per frame: delay + FRAME_BYTES
// PMA2 keeps each GIF frame's own timing. All fields are little-endian u16.
bool playAnim(const String& p){
  stopClock(); stopAnim(); animFile=SD_MMC.open(p,FILE_READ); if(!animFile)return false;
  char m[4]; if(animFile.read((uint8_t*)m,4)!=4){animFile.close();return false;}
  if(!memcmp(m,"PMA2",4))animPmaV2=true;
  else if(!memcmp(m,"PMA1",4))animPmaV2=false;
  else{animFile.close();return false;}
  uint16_t w=read16(animFile),h=read16(animFile);animFrames=read16(animFile);
  animDelay=animPmaV2?100:read16(animFile);
  if(w!=64||h!=64||!animFrames){animFile.close();return false;}
  animIndex=0;animationPlaying=true;nextAnimAt=millis();return true;
}
void serviceAnim(){
  if(previewHoldUntil||!animationPlaying||!animFile||millis()<nextAnimAt)return;
  if(animPmaV2){
    // Header is 10 bytes; each frame block is 2-byte delay + FRAME_BYTES pixels.
    size_t off=10+(size_t)animIndex*(2+FRAME_BYTES);
    if(!animFile.seek(off)){stopAnim();return;}
    animDelay=read16(animFile);
    if(animFile.read(frameBuffer,FRAME_BYTES)!=FRAME_BYTES){stopAnim();return;}
  }else{
    size_t off=12+(size_t)animIndex*FRAME_BYTES;
    if(!animFile.seek(off)||animFile.read(frameBuffer,FRAME_BYTES)!=FRAME_BYTES){stopAnim();return;}
  }
  applyFrame();animIndex=(animIndex+1)%animFrames;nextAnimAt=millis()+animDelay;
}


uint16_t parseHex565(String hex) {
  hex.trim();
  if (hex.startsWith("#")) hex.remove(0, 1);
  if (hex.length() != 6) return 0;
  uint32_t rgb = strtoul(hex.c_str(), nullptr, 16);
  uint8_t r = (rgb >> 16) & 0xFF;
  uint8_t g = (rgb >> 8) & 0xFF;
  uint8_t b = rgb & 0xFF;
  return display.color565(r, g, b);
}

float jsonNumber(const String& json, const char* key, float fallback) {
  String needle = String("\"") + key + "\":";
  int p = json.indexOf(needle);
  if (p < 0) return fallback;
  p += needle.length();
  while (p < (int)json.length() && (json[p] == ' ' || json[p] == '\t')) p++;
  int e = p;
  while (e < (int)json.length()) {
    char c = json[e];
    if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E') e++;
    else break;
  }
  if (e <= p) return fallback;
  return json.substring(p, e).toFloat();
}

const char* weatherShort(int code) {
  if (code == 0) return "SOL";
  if (code <= 3) return "NUB";
  if (code == 45 || code == 48) return "NIE";
  if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) return "LLU";
  if (code >= 71 && code <= 77) return "NVE";
  if (code >= 95) return "TOR";
  return "CLM";
}

// Position right after "key":<open> ('{' or '['), searching from `from`; -1 if absent.
int jsonFind(const String& s, int from, const char* key, char open) {
  if (from < 0) return -1;
  String needle = String("\"") + key + "\":" + open;
  int p = s.indexOf(needle, from);
  return p < 0 ? -1 : p + needle.length();
}

// Like jsonNumber() but starting at `from`, so a key is read from the right
// object (Open-Meteo repeats every key in the *_units objects as text).
float jsonNumberFrom(const String& s, int from, const char* key, float fallback) {
  if (from < 0) return fallback;
  String needle = String("\"") + key + "\":";
  int p = s.indexOf(needle, from);
  if (p < 0) return fallback;
  p += needle.length();
  while (p < (int)s.length() && s[p] == ' ') p++;
  int e = p;
  while (e < (int)s.length()) {
    char c = s[e];
    if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E') e++;
    else break;
  }
  return e > p ? s.substring(p, e).toFloat() : fallback;
}

// Reads up to maxN numbers of the array "key":[...] found after `from`.
// null entries become NAN. Returns how many entries were read.
int jsonNumbers(const String& s, int from, const char* key, float* out, int maxN) {
  int p = jsonFind(s, from, key, '[');
  if (p < 0) return 0;
  int n = 0;
  while (n < maxN && p < (int)s.length()) {
    while (p < (int)s.length() && s[p] == ' ') p++;
    if (p >= (int)s.length() || s[p] == ']') break;
    int e = p;
    while (e < (int)s.length() && s[e] != ',' && s[e] != ']') e++;
    String v = s.substring(p, e);
    v.trim();
    out[n++] = (v.length() == 0 || v == "null") ? NAN : v.toFloat();
    p = e;
    if (p < (int)s.length() && s[p] == ',') p++;
  }
  return n;
}

// First "YYYY-MM-DDTHH:MM" of the array "key":[...] as minutes after midnight, or -1.
int jsonFirstHHMM(const String& s, int from, const char* key) {
  int p = jsonFind(s, from, key, '[');
  if (p < 0) return -1;
  int t = s.indexOf('T', p);
  if (t < 0 || t + 5 >= (int)s.length()) return -1;
  int hh = s.substring(t + 1, t + 3).toInt(), mm = s.substring(t + 4, t + 6).toInt();
  return (hh >= 0 && hh < 24 && mm >= 0 && mm < 60) ? hh * 60 + mm : -1;
}

void fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) return;
  LocationCfg loc;
  portENTER_CRITICAL(&wxMux);
  loc = location;
  portEXIT_CRITICAL(&wxMux);
  char url[360];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,is_day"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min,relative_humidity_2m_mean,sunrise,sunset"
           "&forecast_days=4&timezone=auto",
           loc.lat, loc.lon);
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, url)) return;
  http.setConnectTimeout(5000);
  http.setTimeout(7000);
  http.setUserAgent("MatrixStudioClock/6.0");
  int code = http.GET();
  if (code < 200 || code >= 300) {
    Log.println("ERROR clima: HTTP " + String(code));
    http.end();
    return;
  }
  String body = http.getString();
  http.end();
  int cur = jsonFind(body, 0, "current", '{');
  int daily = jsonFind(body, 0, "daily", '{');
  float t = jsonNumberFrom(body, cur, "temperature_2m", NAN);
  if (isnan(t)) {
    Log.println("ERROR clima: respuesta sin datos");
    return;
  }
  float h = jsonNumberFrom(body, cur, "relative_humidity_2m", 0);
  float f = jsonNumberFrom(body, cur, "apparent_temperature", t);
  int wc = (int)jsonNumberFrom(body, cur, "weather_code", -1);
  bool isDay = jsonNumberFrom(body, cur, "is_day", 1) != 0;
  float dc[4], dmax[4], dmin[4], dhum[4];
  int nc = jsonNumbers(body, daily, "weather_code", dc, 4);
  int nx = jsonNumbers(body, daily, "temperature_2m_max", dmax, 4);
  int nn = jsonNumbers(body, daily, "temperature_2m_min", dmin, 4);
  int nh = jsonNumbers(body, daily, "relative_humidity_2m_mean", dhum, 4);
  int sunrise = jsonFirstHHMM(body, daily, "sunrise");
  int sunset = jsonFirstHHMM(body, daily, "sunset");
  int32_t offset = (int32_t)jsonNumber(body, "utc_offset_seconds", (float)loc.utcOffset);

  portENTER_CRITICAL(&wxMux);
  weatherTempC = t;
  weatherHumidityPct = h;
  weatherFeelsC = f;
  weatherCode = wc;
  weatherIsDay = isDay;
  wxSunriseMin = sunrise;
  wxSunsetMin = sunset;
  for (int i = 0; i < 4; i++) {
    bool ok = i < nc && i < nx && i < nn && !isnan(dc[i]) && !isnan(dmax[i]) && !isnan(dmin[i]);
    wxDays[i].valid = ok;
    wxDays[i].code = ok ? (int)dc[i] : -1;
    wxDays[i].tmax = ok ? dmax[i] : 0;
    wxDays[i].tmin = ok ? dmin[i] : 0;
    wxDays[i].hum = (ok && i < nh && !isnan(dhum[i])) ? dhum[i] : 0;
  }
  weatherValid = true;
  if (offset != location.utcOffset && offset > -50400 && offset < 50400) {
    pendingUtcOffset = offset;
    utcOffsetPending = true;
  }
  portEXIT_CRITICAL(&wxMux);
  Log.println("Clima actualizado: " + String(t, 1) + " C, " + String(h, 0) + "% humedad, " + String(loc.name));
}

void weatherTask(void* parameter) {
  uint32_t lastFetch = 0;
  for (;;) {
    if (WiFi.status() == WL_CONNECTED && (forceWeatherRefresh || millis() - lastFetch > 900000UL)) {
      forceWeatherRefresh = false;
      fetchWeather();
      lastFetch = millis();
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

// Applies a UTC offset (seconds) to local time, e.g. -21600 -> "<-06>6".
void applyUtcOffset(int32_t off) {
  int32_t a = off < 0 ? -off : off;
  int hh = a / 3600, mm = (a % 3600) / 60;
  char tz[32];
  char sign = off < 0 ? '-' : '+', posix = off < 0 ? '+' : '-';
  if (mm) snprintf(tz, sizeof(tz), "<%c%02d%02d>%c%d:%02d", sign, hh, mm, posix, hh, mm);
  else snprintf(tz, sizeof(tz), "<%c%02d>%c%d", sign, hh, posix, hh);
  setenv("TZ", tz, 1);
  tzset();
}

// >>> FACE ART
// Pixel art: '.' is transparent, other chars index the palette (RGB565).
static const uint16_t PAL_cat_a[] = {0xF524,0xDBA3,0x3BDF,0x38E2,0xFFFF,0x6943,0xDA8D,0x79A2,0xFBD3,0xFF99,0xFE87};
static const Sprite SPR_cat_a = {26, 30, "abcehmnopwy", PAL_cat_a,
  "......oo..............oo.."
  "......oao............oao.."
  "......opao..........oapo.."
  ".....oappao.oooooo.oappao."
  ".....oappaaoaaaaaaoaappao."
  ".....oaaaaaaaaaaaaaaaaaao."
  ".....oaaaaaaaaaaaaaaaaaao."
  ".....oaaaawwaaaaaawwaaaao."
  ".....oaaaaaaaaaaaaaaaaaao."
  ".....oaaaaheaaaaaaheaaaao."
  ".....oaaaaeeaaaaaaeeaaaao."
  ".....owwaaeeawwwwaeeaawwo."
  ".....owppwwwwwnnwwwwwppwo."
  ".....owwwwwwmwwwwmwwwwwwo."
  ".....owwwwwwwmmmmwwwwwwwo."
  "..ooooowwwwwwwwwwwwwwwwo.."
  ".oaaaaooooooooooooooooo..."
  "oaaawwwo.occccyycccco....."
  "oaaoooo.obbbwwyywwbbbo...."
  "oaao...oaaaaawwwwaaaaao..."
  "oaao...oaaoaawwwwaaoaao..."
  "oaaao..oaaoaawwwwaaoaao..."
  ".oaaaooowwoaawwwwaaowwo..."
  ".obaaaaooooaaawwaaaooo...."
  "..obbaaa.obaaaaaaaabo....."
  "...oooo..obaaaooaaabo....."
  ".........obaao..oaabo....."
  ".........obaao..oaabo....."
  ".........owwwo..owwwo....."
  ".........ooooo..ooooo....."};
static const uint16_t PAL_cat_b[] = {0x4A6D,0x39EB,0x2E8B,0xFE87,0xFFFF,0x18A4,0xCAAF,0xFC76,0x2907,0xFBF5,0xF7BF,0xFE87};
static const Sprite SPR_cat_b = {26, 30, "abcehkmnopwy", PAL_cat_b,
  ".........................."
  "..oo..............oo......"
  "..opo...oooooo...opo......"
  "..oppoooaaaaaaoooppo......"
  ".oappaaaaaaaaaaaappao....."
  ".oaaaaaaaaaaaaaaaaaao....."
  ".oaaaaaaaaaaaaaaaaaao....."
  ".oaaaaaaaawwaaaaaaaao....."
  ".oaaaheeaawwaaheeaaao....."
  ".oaaaekeawwwwaekeaaao....."
  ".oaaaekeawwwwaekeaaao.ooo."
  ".oaaaaaaawwwwaaaaaaaoowwwo"
  ".oappawwwwnnwwwwappaoowwao"
  ".oaawwwwmwwwwmwwwwaao.oaao"
  ".oaawwwwwmmmmwwwwwaao.oaao"
  "..oawwwwwwwwwwwwwwao.oaao."
  "...oooooooooooooooo.oaao.."
  ".....occccyycccco..oaao..."
  "....obbbwwyywwbbbo.oaao..."
  "...oaaaawwwwwwaaaaooaao..."
  "...oaaoaawwwwaaoaaooaao..."
  "...oaaoaawwwwaaoaaoaao...."
  "...owwoaawwwwaaowwoaao...."
  "....oooaaawwaaaoooaao....."
  ".....obaaaaaaaabo.oo......"
  ".....obaaaooaaabo........."
  ".....obaao..oaabo........."
  ".....obaao..oaabo........."
  ".....owwwo..owwwo........."
  ".....ooooo..ooooo........."};
static const uint16_t PAL_wx_clear_day_10[] = {0xFFB8,0xFCE3,0xFE87};
static const Sprite SPR_wx_clear_day_10 = {10, 10, "hoy", PAL_wx_clear_day_10,
  "....oo...."
  ".o......o."
  "...yyyy..."
  "..yhhyyy.."
  "o.yhyyyy.o"
  "o.yyyyyo.o"
  "..yyyyoo.."
  "...oooo..."
  ".o......o."
  "....oo...."};
static const uint16_t PAL_wx_clear_day_20[] = {0xFFB8,0xFCE3,0xFE87};
static const Sprite SPR_wx_clear_day_20 = {20, 20, "hoy", PAL_wx_clear_day_20,
  "...................."
  ".........oo........."
  "..o......oo......o.."
  "...o.....oo.....o..."
  "....o..........o...."
  ".......yyyyyy......."
  "......yyyyyyyy......"
  ".....yyhhyyyyyy....."
  ".....yhyyyyyyyy....."
  ".ooo.yhyyyyyyyy.ooo."
  ".ooo.yyyyyyyyyo.ooo."
  ".....yyyyyyyyyo....."
  ".....yyyyyyyyoo....."
  "......yyyyyooo......"
  ".......oooooo......."
  "....o..........o...."
  "...o.....oo.....o..."
  "..o......oo......o.."
  ".........oo........."
  "...................."};
static const uint16_t PAL_wx_clear_night_10[] = {0xFF95,0xFFFF,0xFF95};
static const Sprite SPR_wx_clear_night_10 = {10, 10, "akm", PAL_wx_clear_night_10,
  ".........."
  "...m....a."
  "..mm...aka"
  ".mmm....a."
  ".mmm......"
  ".mmmm....k"
  ".mmmmm..m."
  "..mmmmmmm."
  "...mmmmm.."
  ".........."};
static const uint16_t PAL_wx_clear_night_20[] = {0xFF95,0xFFFF,0xFF95};
static const Sprite SPR_wx_clear_night_20 = {20, 20, "akm", PAL_wx_clear_night_20,
  "...................."
  "...................."
  ".......m......a....."
  ".....mmm.....aka...."
  "....mmm.......a....."
  "...mmmm............."
  "..mmmmm............."
  "..mmmmm............."
  ".mmmmmm...........k."
  ".mmmmmm............."
  ".mmmmmm............."
  ".mmmmmmm............"
  ".mmmmmmmm..........."
  "..mmmmmmmmm.....m..."
  "..mmmmmmmmmmmmmmm..."
  "...mmmmmmmmmmmmm...."
  "....mmmmmmmmmmm....."
  ".....mmmmmmmmm......"
  ".......mmmmm........"
  "...................."};
static const uint16_t PAL_wx_cloudy_10[] = {0x6BF2,0x8CF6,0xADD9,0xD6FD,0xF7BF};
static const Sprite SPR_wx_cloudy_10 = {10, 10, "Ggstw", PAL_wx_cloudy_10,
  ".........."
  "......gg.."
  "....ggggg."
  "...ggggggg"
  "......gggG"
  "...ww...gG"
  ".wwwwwt..G"
  "wwwwwwwt.."
  "twwwwwtt.."
  ".ssssss..."};
static const uint16_t PAL_wx_cloudy_20[] = {0x6BF2,0x8CF6,0xADD9,0xD6FD,0xF7BF};
static const Sprite SPR_wx_cloudy_20 = {20, 20, "Ggstw", PAL_wx_cloudy_20,
  "...................."
  "...................."
  "...................."
  "............gggg...."
  "..........gggggggg.."
  ".........gggggggggg."
  "......gg.ggggggggggg"
  ".....ggg......gggggg"
  ".....g...wwww...gggg"
  ".......wwwwwwww..ggg"
  "......wwwwwwwwww..gG"
  "...ww.wwwwwwwwwwt..."
  "..wwwwwwwwwwwwwwtt.."
  ".wwwwwwwwwwwwwwwwtt."
  ".wwwwwwwwwwwwwwwwwt."
  ".twwwwwwwwwwwwwwwtt."
  ".ttttttttttttttttts."
  "..ssssssssssssssss.."
  "...................."
  "...................."};
static const uint16_t PAL_wx_drizzle_10[] = {0x7E3F,0xADD9,0xD6FD,0xF7BF};
static const Sprite SPR_wx_drizzle_10 = {10, 10, "cstw", PAL_wx_drizzle_10,
  "....www..."
  ".ww.wwwwt."
  "wwwwwwwwwt"
  "twwwwwwwtt"
  ".ssssssss."
  ".........."
  "..c....c.."
  "..c....c.."
  ".........."
  "....c....c"};
static const uint16_t PAL_wx_drizzle_20[] = {0x7E3F,0xADD9,0xD6FD,0xF7BF};
static const Sprite SPR_wx_drizzle_20 = {20, 20, "cstw", PAL_wx_drizzle_20,
  "...................."
  ".........wwww......."
  ".......wwwwwwww....."
  "......wwwwwwwwww...."
  "...ww.wwwwwwwwwwt..."
  "..wwwwwwwwwwwwwwtt.."
  ".wwwwwwwwwwwwwwwwtt."
  ".wwwwwwwwwwwwwwwwwt."
  ".twwwwwwwwwwwwwwwtt."
  ".ttttttttttttttttts."
  "..ssssssssssssssss.."
  "...................."
  "...................."
  "....c.....c.....c..."
  "....c.....c.....c..."
  "...................."
  ".......c.....c......"
  ".......c.....c......"
  "...................."
  "...................."};
static const uint16_t PAL_wx_fog_10[] = {0x9516,0xCE9B};
static const Sprite SPR_wx_fog_10 = {10, 10, "Zz", PAL_wx_fog_10,
  "....zzz..."
  ".zz.zzzz.."
  "zzzzzzzzz."
  ".ZZZZZZZZ."
  ".........."
  "zzzzzzzz.."
  ".........."
  "..zzzzzzzz"
  ".........."
  ".ZZZZZZ..."};
static const uint16_t PAL_wx_fog_20[] = {0x9516,0xCE9B};
static const Sprite SPR_wx_fog_20 = {20, 20, "Zz", PAL_wx_fog_20,
  "...................."
  ".........zzzz......."
  ".......zzzzzzzz....."
  "......zzzzzzzzzz...."
  "...zz.zzzzzzzzzzz..."
  "..zzzzzzzzzzzzzzzz.."
  ".zzzzzzzzzzzzzzzzzz."
  ".zzzzzzzzzzzzzzzzzz."
  ".zzzzzzzzzzzzzzzzzz."
  ".zzzzzzzzzzzzzzzzzZ."
  "..ZZZZZZZZZZZZZZZZ.."
  "...................."
  "...................."
  "..zzzzzzzzzzzzzz...."
  "...................."
  "...................."
  ".....zzzzzzzzzzzzzz."
  "...................."
  "...................."
  "...ZZZZZZZZZZ......."};
static const uint16_t PAL_wx_partly_day_10[] = {0xFFB8,0xFCE3,0xADD9,0xD6FD,0xF7BF,0xFE87};
static const Sprite SPR_wx_partly_day_10 = {10, 10, "hostwy", PAL_wx_partly_day_10,
  "....o....."
  ".o.....o.."
  "...yyy...."
  "..yhyyy..."
  "o.yyyyy..."
  "..yyyywww."
  "...ywwwwt."
  "..wwwwwwwt"
  "..twwwwwtt"
  "...ssssss."};
static const uint16_t PAL_wx_partly_day_20[] = {0xFFB8,0xFCE3,0xADD9,0xD6FD,0xF7BF,0xFE87};
static const Sprite SPR_wx_partly_day_20 = {20, 20, "hostwy", PAL_wx_partly_day_20,
  "......oo............"
  ".o....oo....o......."
  "..o........o........"
  ".....yyyy..........."
  "....yyyyyy.........."
  "...yyhhyyyy........."
  "oo.yhyyyyyy.oo......"
  "oo.yyyyyyyo.oo......"
  "...yyyyyyyo........."
  "....yyyyoo.........."
  ".....oooo...wwww...."
  "..o.......wwwwwwww.."
  ".o.......wwwwwwwwwt."
  "......ww.wwwwwwwwwwt"
  ".....wwwwwwwwwwwwwwt"
  ".....wwwwwwwwwwwwwwt"
  ".....twwwwwwwwwwwwtt"
  ".....tttttttttttttts"
  "......sssssssssssss."
  "...................."};
static const uint16_t PAL_wx_partly_night_10[] = {0xFFFF,0xFF95,0xADD9,0xD6FD,0xF7BF};
static const Sprite SPR_wx_partly_night_10 = {10, 10, "kmstw", PAL_wx_partly_night_10,
  "..mm......"
  ".mm.....k."
  ".mm......."
  "mmm......."
  "mmmm......"
  ".mmmmwww.."
  "..mwwwwwt."
  "..wwwwwwwt"
  "..twwwwwtt"
  "...ssssss."};
static const uint16_t PAL_wx_partly_night_20[] = {0xFFFF,0xFF95,0xADD9,0xD6FD,0xF7BF};
static const Sprite SPR_wx_partly_night_20 = {20, 20, "kmstw", PAL_wx_partly_night_20,
  "...................."
  "....mm.............."
  "...mmm........k....."
  "..mmm..............."
  "..mmm..............."
  ".mmmm..............."
  ".mmmm..............."
  ".mmmmm.............."
  ".mmmmmm............."
  "..mmmmmmm..........."
  "..mmmmmmm...wwww...."
  "...mmmmm..wwwwwwww.."
  ".........wwwwwwwwwt."
  "......ww.wwwwwwwwwwt"
  ".....wwwwwwwwwwwwwwt"
  ".....wwwwwwwwwwwwwwt"
  ".....twwwwwwwwwwwwtt"
  ".....tttttttttttttts"
  "......sssssssssssss."
  "...................."};
static const uint16_t PAL_wx_rain_10[] = {0x3D1F,0xADD9,0xD6FD,0xF7BF};
static const Sprite SPR_wx_rain_10 = {10, 10, "bstw", PAL_wx_rain_10,
  "....www..."
  ".ww.wwwwt."
  "wwwwwwwwwt"
  "twwwwwwwtt"
  ".ssssssss."
  ".........."
  "...b..b..b"
  "..b..b..b."
  ".b..b..b.."
  ".........."};
static const uint16_t PAL_wx_rain_20[] = {0x3D1F,0xADD9,0xD6FD,0xF7BF};
static const Sprite SPR_wx_rain_20 = {20, 20, "bstw", PAL_wx_rain_20,
  "...................."
  ".........wwww......."
  ".......wwwwwwww....."
  "......wwwwwwwwww...."
  "...ww.wwwwwwwwwwt..."
  "..wwwwwwwwwwwwwwtt.."
  ".wwwwwwwwwwwwwwwwtt."
  ".wwwwwwwwwwwwwwwwwt."
  ".twwwwwwwwwwwwwwwtt."
  ".ttttttttttttttttts."
  "..ssssssssssssssss.."
  "...................."
  "...................."
  "......b...b...b....."
  "......b...b...b....."
  ".....b...b...b......"
  "....b...b...b...b..."
  "....b...b...b...b..."
  "...b...b...b...b...."
  "...................."};
static const uint16_t PAL_wx_snow_10[] = {0xE7BF,0xADD9,0xD6FD,0xF7BF,0x7EFF};
static const Sprite SPR_wx_snow_10 = {10, 10, "fstwx", PAL_wx_snow_10,
  "....www..."
  ".ww.wwwwt."
  "wwwwwwwwwt"
  "twwwwwwwtt"
  ".ssssssss."
  ".........."
  "..x......."
  ".xfx...x.."
  "..x...xfx."
  ".......x.."};
static const uint16_t PAL_wx_snow_20[] = {0xE7BF,0xADD9,0xD6FD,0xF7BF,0x7EFF};
static const Sprite SPR_wx_snow_20 = {20, 20, "fstwx", PAL_wx_snow_20,
  "...................."
  ".........wwww......."
  ".......wwwwwwww....."
  "......wwwwwwwwww...."
  "...ww.wwwwwwwwwwt..."
  "..wwwwwwwwwwwwwwtt.."
  ".wwwwwwwwwwwwwwwwtt."
  ".wwwwwwwwwwwwwwwwwt."
  ".twwwwwwwwwwwwwwwtt."
  ".ttttttttttttttttts."
  "..ssssssssssssssss.."
  "...................."
  "...................."
  ".....x.........x...."
  "....xfx.......xfx..."
  ".....x.........x...."
  "..........x........."
  ".........xfx........"
  "..........x........."
  "...................."};
static const uint16_t PAL_wx_storm_10[] = {0x532F,0x6BD2,0x84B5,0xFCE3,0xFE87};
static const Sprite SPR_wx_storm_10 = {10, 10, "Ddeoy", PAL_wx_storm_10,
  "....eee..."
  ".ee.eeeed."
  "eeeeeeeeed"
  "deeeeeeedd"
  ".DDDD.yoD."
  ".....yo..."
  "....yyyo.."
  ".....yo..."
  "....yo...."
  "....o....."};
static const uint16_t PAL_wx_storm_20[] = {0x532F,0x6BD2,0x84B5,0xFCE3,0xFE87};
static const Sprite SPR_wx_storm_20 = {20, 20, "Ddeoy", PAL_wx_storm_20,
  "...................."
  ".........eeee......."
  ".......eeeeeeee....."
  "......eeeeeeeeee...."
  "...ee.eeeeeeeeeed..."
  "..eeeeeeeeeeeeeedd.."
  ".eeeeeeeeeeeeeeeedd."
  ".eeeeeeeeeeeeeeeeed."
  ".deeeeeeeeeeeeeeedd."
  ".ddddddddd.....dddD."
  "..DDDDDDD..yyo.DDD.."
  "..........yyo......."
  ".........yyo........"
  "........yyyyyo......"
  "..........yyo......."
  ".........yyo........"
  ".........yo........."
  ".........o.........."
  "...................."
  "...................."};
static const uint16_t PAL_wx_unknown_10[] = {0x6BF2,0x8CF6,0xFFFF};
static const Sprite SPR_wx_unknown_10 = {10, 10, "Ggk", PAL_wx_unknown_10,
  ".........."
  "...gggg..."
  "..ggkkkgg."
  ".ggggggkgg"
  "gggggkgggg"
  "gggggggggg"
  "gggggkgggG"
  ".GGGGGGGG."
  ".........."
  ".........."};
static const uint16_t PAL_wx_unknown_20[] = {0x6BF2,0x8CF6,0xFFFF};
static const Sprite SPR_wx_unknown_20 = {20, 20, "Ggk", PAL_wx_unknown_20,
  "...................."
  "...................."
  "...................."
  "...................."
  ".........gggg......."
  ".......gggggggg....."
  "......gggkkkgggg...."
  "...gg.ggkkgkkgggg..."
  "..gggggggggkkggggg.."
  ".gggggggggkkggggggg."
  ".gggggggggkgggggggg."
  ".gggggggggggggggggg."
  ".gggggggggkgggggggG."
  "..GGGGGGGGGGGGGGGG.."
  "...................."
  "...................."
  "...................."
  "...................."
  "...................."
  "...................."};
static const uint16_t PAL_cat_a_sleep[] = {0xF81F};
static const Sprite SPR_cat_a_sleep = {1, 1, "a", PAL_cat_a_sleep, "a"};
static const uint16_t PAL_cat_b_sleep[] = {0xF81F};
static const Sprite SPR_cat_b_sleep = {1, 1, "a", PAL_cat_b_sleep, "a"};
static const uint16_t PAL_sun_face[] = {0xF81F};
static const Sprite SPR_sun_face = {1, 1, "a", PAL_sun_face, "a"};
static const uint16_t PAL_moon_face[] = {0xF81F};
static const Sprite SPR_moon_face = {1, 1, "a", PAL_moon_face, "a"};
static const uint16_t PAL_snowman[] = {0xF81F};
static const Sprite SPR_snowman = {1, 1, "a", PAL_snowman, "a"};
static const uint16_t PAL_zzz[] = {0xF81F};
static const Sprite SPR_zzz = {1, 1, "a", PAL_zzz, "a"};
static const uint16_t PAL_sparkle[] = {0xF81F};
static const Sprite SPR_sparkle = {1, 1, "a", PAL_sparkle, "a"};
// <<< FACE ART
// >>> FACE CODE
// =========================
// Clock faces: drawing
// =========================
// Shapes are first drawn into a 1-byte-per-pixel mask and then "flushed" with
// a Paint (solid, gradient over a box, or rainbow), so any text, digit or
// shape can use a gradient. Everything is clipped to the 64x64 buffer.
static uint16_t faceFb[64 * 64];
static uint8_t faceMk[64 * 64];
static int mkX0 = 64, mkY0 = 64, mkX1 = -1, mkY1 = -1;

#define FACE_DEG "\x01"
#define FACE_COLON_OFF '\x02'

static inline void fbSet(int x, int y, uint16_t c) {
  if ((unsigned)x < 64u && (unsigned)y < 64u) faceFb[y * 64 + x] = c;
}
static void fbFill(uint16_t c) {
  for (int i = 0; i < 64 * 64; i++) faceFb[i] = c;
}
static void fbRect(int x, int y, int w, int h, uint16_t c) {
  for (int j = 0; j < h; j++)
    for (int i = 0; i < w; i++) fbSet(x + i, y + j, c);
}
static void fbFrame(int x, int y, int w, int h, uint16_t c) {
  fbRect(x, y, w, 1, c);
  fbRect(x, y + h - 1, w, 1, c);
  fbRect(x, y, 1, h, c);
  fbRect(x + w - 1, y, 1, h, c);
}

static inline void mkSet(int x, int y) {
  if ((unsigned)x >= 64u || (unsigned)y >= 64u) return;
  faceMk[y * 64 + x] = 1;
  if (x < mkX0) mkX0 = x;
  if (x > mkX1) mkX1 = x;
  if (y < mkY0) mkY0 = y;
  if (y > mkY1) mkY1 = y;
}
static void mkRect(int x, int y, int w, int h) {
  for (int j = 0; j < h; j++)
    for (int i = 0; i < w; i++) mkSet(x + i, y + j);
}

// Same algorithms as Adafruit GFX, so the classic faces keep their exact look.
static void mkLine(int x0, int y0, int x1, int y1) {
  bool steep = abs(y1 - y0) > abs(x1 - x0);
  int t;
  if (steep) { t = x0; x0 = y0; y0 = t; t = x1; x1 = y1; y1 = t; }
  if (x0 > x1) { t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }
  int dx = x1 - x0, dy = abs(y1 - y0), err = dx / 2, ystep = y0 < y1 ? 1 : -1;
  for (; x0 <= x1; x0++) {
    if (steep) mkSet(y0, x0); else mkSet(x0, y0);
    err -= dy;
    if (err < 0) { y0 += ystep; err += dx; }
  }
}
static void mkCircle(int x0, int y0, int r) {
  int f = 1 - r, ddx = 1, ddy = -2 * r, x = 0, y = r;
  mkSet(x0, y0 + r); mkSet(x0, y0 - r); mkSet(x0 + r, y0); mkSet(x0 - r, y0);
  while (x < y) {
    if (f >= 0) { y--; ddy += 2; f += ddy; }
    x++; ddx += 2; f += ddx;
    mkSet(x0 + x, y0 + y); mkSet(x0 - x, y0 + y); mkSet(x0 + x, y0 - y); mkSet(x0 - x, y0 - y);
    mkSet(x0 + y, y0 + x); mkSet(x0 - y, y0 + x); mkSet(x0 + y, y0 - x); mkSet(x0 - y, y0 - x);
  }
}
static void mkVLine(int x, int y, int h) { mkRect(x, y, 1, h); }
static void mkDisc(int x0, int y0, int r) {
  mkVLine(x0, y0 - r, 2 * r + 1);
  int f = 1 - r, ddx = 1, ddy = -2 * r, x = 0, y = r, px = x, py = y;
  while (x < y) {
    if (f >= 0) { y--; ddy += 2; f += ddy; }
    x++; ddx += 2; f += ddx;
    if (x < y + 1) { mkVLine(x0 + x, y0 - y, 2 * y + 1); mkVLine(x0 - x, y0 - y, 2 * y + 1); }
    if (y != py) { mkVLine(x0 + py, y0 - px, 2 * px + 1); mkVLine(x0 - py, y0 - px, 2 * px + 1); py = y; }
    px = x;
  }
}

static inline void c565to888(uint16_t c, int& r, int& g, int& b) {
  r = ((c >> 11) & 31) * 255 / 31;
  g = ((c >> 5) & 63) * 255 / 63;
  b = (c & 31) * 255 / 31;
}
static inline uint16_t rgb565(int r, int g, int b) {
  if (r < 0) r = 0; if (r > 255) r = 255;
  if (g < 0) g = 0; if (g > 255) g = 255;
  if (b < 0) b = 0; if (b > 255) b = 255;
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
static uint16_t lerp565(uint16_t a, uint16_t b, float t) {
  int r1, g1, b1, r2, g2, b2;
  c565to888(a, r1, g1, b1);
  c565to888(b, r2, g2, b2);
  return rgb565(r1 + (int)lroundf((r2 - r1) * t), g1 + (int)lroundf((g2 - g1) * t), b1 + (int)lroundf((b2 - b1) * t));
}
static uint16_t dim565(uint16_t c, float f) { return lerp565(0, c, f); }
static uint16_t hsv565(float h, float s, float v) {
  h = fmodf(h, 360.0f);
  if (h < 0) h += 360.0f;
  float c = v * s, x = c * (1 - fabsf(fmodf(h / 60.0f, 2.0f) - 1)), m = v - c, r, g, b;
  if (h < 60) { r = c; g = x; b = 0; }
  else if (h < 120) { r = x; g = c; b = 0; }
  else if (h < 180) { r = 0; g = c; b = x; }
  else if (h < 240) { r = 0; g = x; b = c; }
  else if (h < 300) { r = x; g = 0; b = c; }
  else { r = c; g = 0; b = x; }
  return rgb565((int)((r + m) * 255), (int)((g + m) * 255), (int)((b + m) * 255));
}

// Color of paint p at (x,y) for a gradient spanning the box (bx,by,bw,bh).
static uint16_t paintAt(const Paint& p, int x, int y, int bx, int by, int bw, int bh) {
  if (p.dir == 0) return p.a;
  float t = 0;
  if (p.dir == 1) t = bh > 1 ? (float)(y - by) / (bh - 1) : 0;
  else if (p.dir == 3) t = bw + bh > 2 ? (float)((x - bx) + (y - by)) / (bw + bh - 2) : 0;
  else t = bw > 1 ? (float)(x - bx) / (bw - 1) : 0;
  if (t < 0) t = 0;
  if (t > 1) t = 1;
  if (p.dir == 4) return hsv565(t * 300.0f, 1, 1);
  return lerp565(p.a, p.b, t);
}

static void mkFlush(const Paint& p, int bx, int by, int bw, int bh) {
  if (mkX1 >= mkX0) {
    for (int y = mkY0; y <= mkY1; y++)
      for (int x = mkX0; x <= mkX1; x++) {
        int i = y * 64 + x;
        if (!faceMk[i]) continue;
        faceMk[i] = 0;
        faceFb[i] = paintAt(p, x, y, bx, by, bw, bh);
      }
  }
  mkX0 = 64; mkY0 = 64; mkX1 = -1; mkY1 = -1;
}
static Paint solid(uint16_t c) { Paint p = {c, c, 0}; return p; }

// ---- text: font 0 = TomThumb 3x5, font 1 = classic 5x7 (GFX default) ----
// y is the top of the capitals. "\x01" draws a degree ring, '\x02' is an
// invisible colon (blinking colon in its "off" second).
static int charAdv(char c, uint8_t font, uint8_t sc) {
  if (c == '\x01') return 4 * sc;
  return (font == 0 ? 4 : 6) * sc;
}
static int textW(const char* s, uint8_t font, uint8_t sc) {
  int w = 0;
  for (const char* c = s; *c; c++) w += charAdv(*c, font, sc);
  return w > 0 ? w - sc : 0;
}
static int textH(uint8_t font, uint8_t sc) { return (font == 0 ? 5 : 7) * sc; }
static void mkGlyphTom(char c, int x, int y, uint8_t sc) {
  if (c < (char)TomThumb.first || c > (char)TomThumb.last) return;
  const GFXglyph* g = &TomThumb.glyph[c - TomThumb.first];
  const uint8_t* bm = TomThumb.bitmap;
  uint16_t bo = g->bitmapOffset;
  uint8_t bits = 0, bit = 0;
  for (int yy = 0; yy < g->height; yy++)
    for (int xx = 0; xx < g->width; xx++) {
      if (!(bit++ & 7)) bits = bm[bo++];
      if (bits & 0x80) mkRect(x + (g->xOffset + xx) * sc, y + (5 + g->yOffset + yy) * sc, sc, sc);
      bits <<= 1;
    }
}
static void mkGlyphClassic(unsigned char c, int x, int y, uint8_t sc) {
  for (int i = 0; i < 5; i++) {
    uint8_t line = font[c * 5 + i];
    for (int j = 0; j < 8; j++, line >>= 1)
      if (line & 1) mkRect(x + i * sc, y + j * sc, sc, sc);
  }
}
static void mkDegree(int x, int y, uint8_t sc) {
  static const uint8_t ring[3] = {7, 5, 7};
  for (int j = 0; j < 3; j++)
    for (int i = 0; i < 3; i++)
      if (ring[j] & (4 >> i)) mkRect(x + i * sc, y + j * sc, sc, sc);
}
static void mkText(const char* s, int x, int y, uint8_t font, uint8_t sc) {
  for (const char* c = s; *c; c++) {
    if (*c == '\x01') mkDegree(x, y, sc);
    else if (*c == FACE_COLON_OFF) {}
    else if (font == 0) mkGlyphTom(*c, x, y, sc);
    else mkGlyphClassic((unsigned char)*c, x, y, sc);
    x += charAdv(*c, font, sc);
  }
}
// align: 0 = x is the left edge, 1 = x is the center, 2 = x is the right edge.
static int alignX(int x, int w, uint8_t align) { return align == 1 ? x - w / 2 : align == 2 ? x - w + 1 : x; }
static void drawText(const char* s, int x, int y, uint8_t font, uint8_t sc, uint8_t align, const Paint& p) {
  int w = textW(s, font, sc), x0 = alignX(x, w, align);
  mkText(s, x0, y, font, sc);
  mkFlush(p, x0, y, w, textH(font, sc));
}

// ---- 7-segment style digits ----
// style 0 = LED segments with gaps, 1 = chunky joined blocks, 2 = thin lines.
// bits: a=1 top, b=2 upper right, c=4 lower right, d=8 bottom, e=16 lower left, f=32 upper left, g=64 middle
static uint8_t segBits(char c) {
  switch (c) {
    case '0': return 0x3F; case '1': return 0x06; case '2': return 0x5B; case '3': return 0x4F;
    case '4': return 0x66; case '5': return 0x6D; case '6': return 0x7D; case '7': return 0x07;
    case '8': return 0x7F; case '9': return 0x6F; case '-': return 0x40; case 'A': return 0x77;
    case 'P': return 0x73; case 'C': return 0x39; case 'E': return 0x79; case 'F': return 0x71;
    default: return 0;
  }
}
static void mkSeg(int x, int y, int w, int h, int th, uint8_t style, uint8_t bits) {
  if (style == 2) th = 1;
  if (th < 1) th = 1;
  int midY = y + (h - th) / 2;
  if (style != 0) {
    if (bits & 1) mkRect(x, y, w, th);
    if (bits & 8) mkRect(x, y + h - th, w, th);
    if (bits & 64) mkRect(x, midY, w, th);
    if (bits & 32) mkRect(x, y, th, midY - y + th);
    if (bits & 2) mkRect(x + w - th, y, th, midY - y + th);
    if (bits & 16) mkRect(x, midY, th, y + h - midY);
    if (bits & 4) mkRect(x + w - th, midY, th, y + h - midY);
    return;
  }
  // LED look: every segment runs between the centers of its two corners and
  // has pointed ends, leaving a 1 px diagonal gap where segments meet.
  int c = (th - 1) / 2;
  int xl = x + c, xr = x + w - 1 - c;                 // column centers
  int yt = y + c, ym = midY + c, yb = y + h - 1 - c;  // lane centers
  for (int k = 0; k < th; k++) {
    int off = abs(2 * k - (th - 1)) / 2 + 1;
    if (bits & 1) mkRect(xl + off, y + k, xr - xl - 2 * off + 1, 1);                 // a
    if (bits & 64) mkRect(xl + off, midY + k, xr - xl - 2 * off + 1, 1);             // g
    if (bits & 8) mkRect(xl + off, y + h - th + k, xr - xl - 2 * off + 1, 1);        // d
    if (bits & 32) mkRect(x + k, yt + off, 1, ym - yt - 2 * off + 1);                // f
    if (bits & 2) mkRect(x + w - th + k, yt + off, 1, ym - yt - 2 * off + 1);        // b
    if (bits & 16) mkRect(x + k, ym + off, 1, yb - ym - 2 * off + 1);                // e
    if (bits & 4) mkRect(x + w - th + k, ym + off, 1, yb - ym - 2 * off + 1);        // c
  }
}
static void mkColon(int x, int y, int cw, int h, int th) {
  int ds = th < 1 ? 1 : th;
  if (ds > cw) ds = cw;
  int dx = x + (cw - ds) / 2;
  mkRect(dx, y + h / 3 - ds / 2, ds, ds);
  mkRect(dx, y + (2 * h) / 3 - ds / 2, ds, ds);
}
static int segW(const char* s, int dw, int cw, int gap) {
  int w = 0, n = 0;
  for (const char* c = s; *c; c++, n++) w += (*c == ':' || *c == FACE_COLON_OFF) ? cw : dw;
  return n ? w + gap * (n - 1) : 0;
}
// Draws digits; ghost (optional) paints the unlit segments first. The gradient
// box is the text box unless bw > 0 (lets several rows share one gradient).
static void segDraw(const char* s, int x, int y, int dw, int dh, int th, int gap, int cw, uint8_t style,
                    uint8_t align, const Paint& on, const Paint* ghost, int bx, int by, int bw, int bh) {
  int w = segW(s, dw, cw, gap), x0 = alignX(x, w, align);
  if (bw <= 0) { bx = x0; by = y; bw = w; bh = dh; }
  for (int pass = ghost ? 0 : 1; pass < 2; pass++) {
    int cx = x0;
    for (const char* c = s; *c; c++) {
      if (*c == ':' || *c == FACE_COLON_OFF) {
        bool lit = *c == ':';
        if (lit == (pass == 1)) mkColon(cx, y, cw, dh, style == 2 ? 1 : th);
        cx += cw + gap;
        continue;
      }
      uint8_t b = segBits(*c);
      mkSeg(cx, y, dw, dh, th, style, pass == 1 ? b : (uint8_t)(~b & 0x7F));
      cx += dw + gap;
    }
    if (pass == 0) mkFlush(*ghost, bx, by, bw, bh);
    else mkFlush(on, bx, by, bw, bh);
  }
}

// ---- sprites ----
// Pixel art as strings: '.' is transparent, other chars index the palette.
// recolorKey1/2 (0 = none) let faces change a sprite color (e.g. cat fur).
static void drawSprite(const Sprite& s, int x, int y, char rk1, uint16_t rc1, char rk2, uint16_t rc2) {
  for (int j = 0; j < s.h; j++)
    for (int i = 0; i < s.w; i++) {
      char c = s.px[j * s.w + i];
      if (c == '.') continue;
      uint16_t col;
      if (rk1 && c == rk1) col = rc1;
      else if (rk2 && c == rk2) col = rc2;
      else {
        const char* k = strchr(s.keys, c);
        if (!k) continue;
        col = s.pal[k - s.keys];
      }
      fbSet(x + i, y + j, col);
    }
}
static void drawSpritePlain(const Sprite& s, int x, int y) { drawSprite(s, x, y, 0, 0, 0, 0); }

// ---- weather icons ----
static int wxKind(int code) {
  if (code < 0) return 8;
  if (code == 0) return 0;
  if (code <= 2) return 1;
  if (code == 3) return 2;
  if (code == 45 || code == 48) return 3;
  if (code >= 51 && code <= 57) return 4;
  if ((code >= 61 && code <= 67) || (code >= 80 && code <= 82)) return 5;
  if ((code >= 71 && code <= 77) || code == 85 || code == 86) return 6;
  if (code >= 95) return 7;
  return 8;
}
static const Sprite* wxSprite(int code, bool night, bool small) {
  static const Sprite* const big[9] = {&SPR_wx_clear_day_20, &SPR_wx_partly_day_20, &SPR_wx_cloudy_20, &SPR_wx_fog_20,
                                       &SPR_wx_drizzle_20, &SPR_wx_rain_20, &SPR_wx_snow_20, &SPR_wx_storm_20, &SPR_wx_unknown_20};
  static const Sprite* const sm[9] = {&SPR_wx_clear_day_10, &SPR_wx_partly_day_10, &SPR_wx_cloudy_10, &SPR_wx_fog_10,
                                      &SPR_wx_drizzle_10, &SPR_wx_rain_10, &SPR_wx_snow_10, &SPR_wx_storm_10, &SPR_wx_unknown_10};
  int k = wxKind(code);
  if (night && k == 0) return small ? &SPR_wx_clear_night_10 : &SPR_wx_clear_night_20;
  if (night && k == 1) return small ? &SPR_wx_partly_night_10 : &SPR_wx_partly_night_20;
  return small ? sm[k] : big[k];
}
// Icon for the current weather, centered in a size x size box at (x,y).
static void drawWxNow(const FaceEnv& e, int x, int y, bool small) {
  const Sprite* s = wxSprite(e.wxValid ? e.code : -1, e.night, small);
  int box = small ? 10 : 20;
  drawSpritePlain(*s, x + (box - s->w) / 2, y + (box - s->h) / 2);
}
static void drawWxDay(const WxDay& d, int x, int y, bool small) {
  if (!d.valid) return;
  const Sprite* s = wxSprite(d.code, false, small);
  int box = small ? 10 : 20;
  drawSpritePlain(*s, x + (box - s->w) / 2, y + (box - s->h) / 2);
}

// Wi-Fi strength icon (9x7): arcs lit by signal, dim when disconnected.
static void drawWifi(int x, int y, bool ok, int rssi, uint16_t c) {
  static const char* const art =
    "..aaaaa.."
    ".a.....a."
    "a.bbbbb.a"
    "..b...b.."
    "...ccc..."
    "........."
    "....d....";
  int bars = !ok ? 0 : rssi > -60 ? 3 : rssi > -70 ? 2 : 1;
  uint16_t dim = dim565(c, 0.22f);
  for (int j = 0; j < 7; j++)
    for (int i = 0; i < 9; i++) {
      char k = art[j * 9 + i];
      if (k == '.') continue;
      bool lit = ok && (k == 'd' || (k == 'c' && bars >= 1) || (k == 'b' && bars >= 2) || (k == 'a' && bars >= 3));
      fbSet(x + i, y + j, lit ? c : dim);
    }
  if (!ok) {
    uint16_t red = rgb565(255, 70, 80);
    for (int i = 0; i < 4; i++) { fbSet(x + 5 + i, y + 3 + i, red); fbSet(x + 8 - i, y + 3 + i, red); }
  }
}

// ---- shapes with paints ----
static void drawAnalog(int cx, int cy, int r, const struct tm& t, bool secs, uint16_t ringC, uint16_t tickC,
                       uint16_t hourC, uint16_t minC, uint16_t secC, uint16_t centerC) {
  mkCircle(cx, cy, r);
  mkFlush(solid(ringC), 0, 0, 64, 64);
  for (int i = 0; i < 12; i++) {
    float a = (i / 12.0f) * 2.0f * (float)M_PI - (float)M_PI / 2.0f;
    fbSet((int)(cx + cosf(a) * (r - 3)), (int)(cy + sinf(a) * (r - 3)), tickC);
  }
  float ma = (t.tm_min / 60.0f) * 2.0f * (float)M_PI - (float)M_PI / 2.0f;
  float ha = ((t.tm_hour % 12 + t.tm_min / 60.0f) / 12.0f) * 2.0f * (float)M_PI - (float)M_PI / 2.0f;
  mkLine(cx, cy, (int)(cx + cosf(ha) * r * 0.50f), (int)(cy + sinf(ha) * r * 0.50f));
  mkFlush(solid(hourC), 0, 0, 64, 64);
  mkLine(cx, cy, (int)(cx + cosf(ma) * r * 0.76f), (int)(cy + sinf(ma) * r * 0.76f));
  mkFlush(solid(minC), 0, 0, 64, 64);
  if (secs) {
    float sa = (t.tm_sec / 60.0f) * 2.0f * (float)M_PI - (float)M_PI / 2.0f;
    mkLine(cx, cy, (int)(cx + cosf(sa) * r * 0.82f), (int)(cy + sinf(sa) * r * 0.82f));
    mkFlush(solid(secC), 0, 0, 64, 64);
  }
  mkDisc(cx, cy, 1);
  mkFlush(solid(centerC), 0, 0, 64, 64);
}
// Frame around the screen; a rainbow paint turns into a continuous ring.
static void drawBorder(int th, const Paint& p) {
  for (int y = 0; y < 64; y++)
    for (int x = 0; x < 64; x++) {
      int d = x < y ? x : y;
      if (63 - x < d) d = 63 - x;
      if (63 - y < d) d = 63 - y;
      if (d >= th) continue;
      uint16_t c;
      if (p.dir == 4) {
        float ang = atan2f(y - 31.5f, x - 31.5f);
        c = hsv565((ang + (float)M_PI) * 180.0f / (float)M_PI + 135.0f, 1, 1);
      } else {
        c = paintAt(p, x, y, 0, 0, 64, 64);
      }
      faceFb[y * 64 + x] = c;
    }
}

// ---- dates and numbers ----
static const char* const DOW3[2][7] = {{"DOM", "LUN", "MAR", "MIE", "JUE", "VIE", "SAB"},
                                       {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"}};
static const char* const DOW2[2][7] = {{"DO", "LU", "MA", "MI", "JU", "VI", "SA"},
                                       {"SU", "MO", "TU", "WE", "TH", "FR", "SA"}};
static const char* const DOWC[2][7] = {{"Dom", "Lun", "Mar", "Mie", "Jue", "Vie", "Sab"},
                                       {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"}};
static const char* const DAYF[2][7] = {{"DOMINGO", "LUNES", "MARTES", "MIERCOLES", "JUEVES", "VIERNES", "SABADO"},
                                       {"SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY", "THURSDAY", "FRIDAY", "SATURDAY"}};
static const char* const MON3[2][12] = {{"ENE", "FEB", "MAR", "ABR", "MAY", "JUN", "JUL", "AGO", "SEP", "OCT", "NOV", "DIC"},
                                        {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"}};
static int faceLang(const FaceSettings& s) { return s.lang == 1 ? 1 : 0; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int wday(const FaceEnv& e, int plusDays) { return (clampi(e.t.tm_wday, 0, 6) + plusDays) % 7; }
static int hourShown(const FaceSettings& s, const FaceEnv& e) {
  int h = clampi(e.t.tm_hour, 0, 23);
  if (s.h24) return h;
  h %= 12;
  return h ? h : 12;
}
static bool colonOn(const FaceSettings& s, const FaceEnv& e) { return !s.blink || (e.t.tm_sec % 2) == 0; }
// "HH:MM" (or with the invisible colon on odd seconds when blinking).
static void fmtTime(char* out, size_t n, const FaceSettings& s, const FaceEnv& e) {
  if (!e.timeValid) { snprintf(out, n, "--:--"); return; }
  snprintf(out, n, "%02d%c%02d", hourShown(s, e), colonOn(s, e) ? ':' : FACE_COLON_OFF, clampi(e.t.tm_min, 0, 59));
}
static void fmtHH(char* out, size_t n, const FaceSettings& s, const FaceEnv& e) {
  if (!e.timeValid) snprintf(out, n, "--"); else snprintf(out, n, "%02d", hourShown(s, e));
}
static void fmtMM(char* out, size_t n, const FaceEnv& e) {
  if (!e.timeValid) snprintf(out, n, "--"); else snprintf(out, n, "%02d", clampi(e.t.tm_min, 0, 59));
}
static void fmtDeg(char* out, size_t n, bool valid, float v, bool deg) {
  if (!valid) snprintf(out, n, "--");
  else snprintf(out, n, deg ? "%ld" FACE_DEG : "%ld", lroundf(v));
}
static const char* wxShort(int code, int lang) {
  static const char* const es[] = {"SOL", "NUB", "NIE", "LLU", "NVE", "TOR", "CLM"};
  static const char* const en[] = {"SUN", "CLD", "FOG", "RAIN", "SNOW", "STRM", "---"};
  int k = code == 0 ? 0 : code <= 3 ? 1 : (code == 45 || code == 48) ? 2
        : ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) ? 3 : (code >= 71 && code <= 77) ? 4 : code >= 95 ? 5 : 6;
  return lang == 1 ? en[k] : es[k];
}

// =========================
// Clock faces
// =========================
// Classic faces: the five designs the clock had before, pixel for pixel.
// Slots: p0 bg, p1 primary, p2 secondary, p3 accent, p4 weather;
// o0 seconds, o1 date, o2 temperature, o3 humidity, o4 weather.
static void legacyText(const char* s, int y, uint8_t sc, uint16_t c) {
  int x = (64 - (int)strlen(s) * 6 * sc) / 2;
  if (x < 0) x = 0;
  drawText(s, x, y, 1, sc, 0, solid(c));
}
static void faceClassic(const FaceSettings& s, const FaceEnv& e, int mode) {
  uint16_t pri = s.p[1].a, sec = s.p[2].a, acc = s.p[3].a, wxc = s.p[4].a;
  bool showSec = s.o[0], showDate = s.o[1], showTemp = s.o[2], showHum = s.o[3], showWx = s.o[4];
  fbFill(s.p[0].a);
  char tm[8], date[16], buf[16];
  fmtTime(tm, sizeof tm, s, e);
  const char* suffix = (!s.h24 && e.timeValid) ? (e.t.tm_hour >= 12 ? "P" : "A") : "";
  if (e.timeValid) snprintf(date, sizeof date, "%02d %s", e.t.tm_mday, MON3[faceLang(s)][clampi(e.t.tm_mon, 0, 11)]);
  else snprintf(date, sizeof date, "-- ---");

  if (mode == 3 || mode == 4) {
    int cx = mode == 4 ? 20 : 32, cy = mode == 4 ? 24 : 30, r = mode == 4 ? 18 : 27;
    if (e.timeValid) drawAnalog(cx, cy, r, e.t, showSec, pri, sec, pri, acc, sec, acc);
    if (mode == 3) {
      if (showDate) legacyText(date, 57, 1, sec);
      return;
    }
    drawText(tm, 63, 8, 1, 1, 2, solid(pri));
    if (*suffix) drawText(suffix, 57, 16, 1, 1, 0, solid(pri));
    if (e.wxValid) {
      if (showTemp) { snprintf(buf, sizeof buf, "%ldC", lroundf(e.temp)); drawText(buf, 39, 25, 1, 1, 0, solid(wxc)); }
      if (showHum) { snprintf(buf, sizeof buf, "%ld%%", lroundf(e.hum)); drawText(buf, 39, 35, 1, 1, 0, solid(wxc)); }
      if (showWx) drawText(wxShort(e.code, faceLang(s)), 39, 45, 1, 1, 0, solid(acc));
    }
    if (showDate) drawText(date, 63, 56, 1, 1, 2, solid(sec));
    return;
  }
  int timeY = (mode == 0 && !showDate) ? 20 : 8;
  legacyText(tm, timeY, 2, pri);
  if (*suffix) drawText(suffix, 56, timeY + 14, 1, 1, 0, solid(sec));
  int y = timeY + 19;
  if (showSec) {
    if (e.timeValid) snprintf(buf, sizeof buf, ":%02d", clampi(e.t.tm_sec, 0, 59)); else snprintf(buf, sizeof buf, ":--");
    legacyText(buf, y, 1, sec);
    y += 10;
  }
  if (showDate) { legacyText(date, y, 1, sec); y += 10; }
  if (mode == 2 && e.wxValid) {
    int wx = 2;
    if (showTemp) { snprintf(buf, sizeof buf, "%ldC", lroundf(e.temp)); drawText(buf, wx, 53, 1, 1, 0, solid(wxc)); wx += 23; }
    if (showHum) { snprintf(buf, sizeof buf, "%ld%%", lroundf(e.hum)); drawText(buf, wx, 53, 1, 1, 0, solid(wxc)); }
    if (showWx) drawWxNow(e, 50, 48, true);
  }
}
static void faceClassicDigital(const FaceSettings& s, const FaceEnv& e) { faceClassic(s, e, 0); }
static void faceClassicFecha(const FaceSettings& s, const FaceEnv& e) { faceClassic(s, e, 1); }
static void faceClassicClima(const FaceSettings& s, const FaceEnv& e) { faceClassic(s, e, 2); }
static void faceClassicAnalog(const FaceSettings& s, const FaceEnv& e) { faceClassic(s, e, 3); }
static void faceClassicHibrido(const FaceSettings& s, const FaceEnv& e) { faceClassic(s, e, 4); }

// Minimal apilado: hours over minutes. p0 digits, p1 date, p2 bg; o0 date, o1 style.
static void faceMinimal(const FaceSettings& s, const FaceEnv& e) {
  fbFill(s.p[2].a);
  bool showDate = s.o[0];
  uint8_t style = s.o[1] > 2 ? 0 : s.o[1];
  char hh[4], mm[4];
  fmtHH(hh, sizeof hh, s, e);
  fmtMM(mm, sizeof mm, e);
  int dw = showDate ? 16 : 18, dh = showDate ? 24 : 27, gap = 4, th = style == 2 ? 1 : style == 0 ? 4 : 3;
  int y1 = 3, y2 = showDate ? 30 : 34;
  int w = 2 * dw + gap;
  segDraw(hh, 32, y1, dw, dh, th, gap, 4, style, 1, s.p[0], nullptr, 32 - w / 2, y1, w, y2 + dh - y1);
  segDraw(mm, 32, y2, dw, dh, th, gap, 4, style, 1, s.p[0], nullptr, 32 - w / 2, y1, w, y2 + dh - y1);
  if (showDate) {
    char d[16];
    if (e.timeValid) snprintf(d, sizeof d, "%s %d", MON3[faceLang(s)][clampi(e.t.tm_mon, 0, 11)], e.t.tm_mday);
    else snprintf(d, sizeof d, "--- --");
    drawText(d, 32, 58, 0, 1, 1, s.p[1]);
  }
}

// Marco arcoiris: stacked digits in a gradient frame. p0 digits, p1 frame, p2 bg; o0 frame px, o1 style.
static void faceRainbowFrame(const FaceSettings& s, const FaceEnv& e) {
  fbFill(s.p[2].a);
  drawBorder(clampi(s.o[0], 1, 3), s.p[1]);
  uint8_t style = s.o[1] > 2 ? 1 : s.o[1];
  char hh[4], mm[4];
  fmtHH(hh, sizeof hh, s, e);
  fmtMM(mm, sizeof mm, e);
  int th = style == 2 ? 1 : style == 1 ? 4 : 3;
  segDraw(hh, 32, 8, 15, 22, th, 4, 4, style, 1, s.p[0], nullptr, 15, 8, 34, 49);
  segDraw(mm, 32, 35, 15, 22, th, 4, 4, style, 1, s.p[0], nullptr, 15, 8, 34, 49);
}

// Segmentos XL: huge LED digits with ghost segments.
// p0 lit segments, p1 ghost, p2 texts, p3 bg, p4 temperature; o0 wifi icon, o1 ghost.
static void faceSegXL(const FaceSettings& s, const FaceEnv& e) {
  fbFill(s.p[3].a);
  int L = faceLang(s);
  char l2[16], tm[8], t[12];
  if (e.timeValid) {
    drawText(DAYF[L][wday(e, 0)], 1, 1, 0, 1, 0, s.p[2]);
    snprintf(l2, sizeof l2, "%s %d", MON3[L][clampi(e.t.tm_mon, 0, 11)], e.t.tm_mday);
    drawText(l2, 1, 8, 0, 1, 0, s.p[2]);
  }
  if (s.o[0]) drawWifi(54, 1, e.wifi, e.rssi, s.p[2].a);
  fmtTime(tm, sizeof tm, s, e);
  Paint ghost = s.p[1];
  segDraw(tm, 32, 17, 11, 24, 3, 3, 4, 0, 1, s.p[0], s.o[1] ? &ghost : nullptr, 0, 0, 0, 0);
  if (!s.h24 && e.timeValid) drawText(e.t.tm_hour >= 12 ? "P" : "A", 2, 49, 1, 1, 0, s.p[2]);
  drawWxNow(e, 27, 46, true);
  fmtDeg(t, sizeof t, e.wxValid, e.temp, true);
  drawText(t, 62, 49, 1, 1, 2, s.p[4]);
}

// Clima 4 dias. p0 date, p1 time, p2 weekday, p3 temperature, p4 max, p5 min,
// p6 humidity, p7 lines, p8 bg, p9 forecast weekdays; o0 per-day data (0 max/min, 1 max/humidity).
static void faceWeather4(const FaceSettings& s, const FaceEnv& e) {
  fbFill(s.p[8].a);
  int L = faceLang(s);
  char a[16], b[16], c[16], h[16];
  if (e.timeValid) snprintf(a, sizeof a, "%02d-%02d", e.t.tm_mday, clampi(e.t.tm_mon, 0, 11) + 1);
  else snprintf(a, sizeof a, "--/--");
  drawText(a, 1, 1, 0, 1, 0, s.p[0]);
  fmtTime(b, sizeof b, s, e);
  drawText(b, 32, 1, 0, 1, 1, s.p[1]);
  if (e.timeValid) drawText(DOW3[L][wday(e, 0)], 62, 1, 0, 1, 2, s.p[2]);
  fbRect(0, 7, 64, 1, s.p[7].a);

  drawWxNow(e, 1, 9, false);
  fmtDeg(a, sizeof a, e.wxValid, e.temp, true);
  uint8_t sc = textW(a, 1, 2) > 41 ? 1 : 2;
  drawText(a, 23 + (41 - textW(a, 1, sc)) / 2, sc == 2 ? 9 : 12, 1, sc, 0, s.p[3]);

  // max|min + humidity, shrinking if it does not fit in the 41 px column
  const WxDay& t0 = e.d[0];
  bool deg = true, hum = true;
  for (int tries = 0; tries < 3; tries++) {
    fmtDeg(a, sizeof a, t0.valid, t0.tmax, deg);
    fmtDeg(c, sizeof c, t0.valid, t0.tmin, deg);
    if (e.wxValid) snprintf(h, sizeof h, "%ld%%", lroundf(e.hum)); else snprintf(h, sizeof h, "--%%");
    int w = textW(a, 0, 1) + 1 + 3 + 1 + textW(c, 0, 1) + (hum ? 3 + textW(h, 0, 1) : 0);
    if (w <= 41) {
      int x = 23 + (41 - w) / 2;
      drawText(a, x, 24, 0, 1, 0, s.p[4]); x += textW(a, 0, 1) + 1;
      drawText("|", x, 24, 0, 1, 0, s.p[7]); x += 4;
      drawText(c, x, 24, 0, 1, 0, s.p[5]); x += textW(c, 0, 1) + 3;
      if (hum) drawText(h, x, 24, 0, 1, 0, s.p[6]);
      break;
    }
    if (deg) deg = false; else hum = false;
  }
  fbRect(0, 31, 64, 1, s.p[7].a);

  for (int i = 0; i < 4; i++) {
    int cx = 8 + i * 16;
    const WxDay& d = e.d[i];
    if (e.timeValid) drawText(DOW2[L][wday(e, i)], cx, 34, 0, 1, 1, s.p[9]);
    drawWxDay(d, cx - 5, 40, true);
    fmtDeg(a, sizeof a, d.valid, d.tmax, true);
    drawText(a, cx, 51, 0, 1, 1, s.p[4]);
    if (s.o[0] == 1) {
      if (d.valid) snprintf(b, sizeof b, "%ld%%", lroundf(d.hum)); else snprintf(b, sizeof b, "--");
      drawText(b, cx, 57, 0, 1, 1, s.p[6]);
    } else {
      fmtDeg(b, sizeof b, d.valid, d.tmin, true);
      drawText(b, cx, 57, 0, 1, 1, s.p[5]);
    }
  }
}

// Shared bottom panel of the mascot faces: character box on the left, time
// and weekday boxes on the right.
static void mascotPanel(const FaceSettings& s, const FaceEnv& e, uint16_t panelBg, uint16_t frame,
                        const Paint& timeP, const Paint& dayP, const Sprite* buddy) {
  fbRect(0, 37, 64, 27, panelBg);
  fbFrame(0, 37, 64, 27, frame);
  fbRect(27, 37, 1, 27, frame);
  fbRect(27, 50, 37, 1, frame);
  if (buddy) drawSpritePlain(*buddy, 1 + (26 - buddy->w) / 2, 38 + (25 - buddy->h) / 2);
  char tm[8], d[8];
  fmtTime(tm, sizeof tm, s, e);
  segDraw(tm, 45, 39, 6, 10, 1, 1, 3, 2, 1, timeP, nullptr, 0, 0, 0, 0);
  if (e.timeValid) snprintf(d, sizeof d, "%s.", DOWC[faceLang(s)][wday(e, 0)]); else snprintf(d, sizeof d, "---");
  drawText(d, 45, 53, 1, 1, 1, dayP);
}
static void drawCats(bool sleeping, uint16_t furA, uint16_t furB) {
  const Sprite& a = sleeping ? SPR_cat_a_sleep : SPR_cat_a;
  const Sprite& b = sleeping ? SPR_cat_b_sleep : SPR_cat_b;
  bool ra = furA != SPR_cat_a.pal[0], rb = furB != SPR_cat_b.pal[0];
  drawSprite(a, 3, 34 - a.h, ra ? 'a' : 0, furA, ra ? 'b' : 0, dim565(furA, 0.8f));
  drawSprite(b, 35, 34 - b.h, rb ? 'a' : 0, furB, rb ? 'b' : 0, dim565(furB, 0.8f));
}
static void drawStars(uint16_t c, bool many) {
  static const uint8_t pts[][2] = {{1, 2}, {30, 3}, {61, 5}, {31, 16}, {2, 20}, {62, 22}, {29, 27}, {16, 1}, {47, 1}};
  int n = many ? 9 : 5;
  for (int i = 0; i < n; i++) fbSet(pts[i][0], pts[i][1], c);
}

// Mascotas (day): p0 sky, p1 night sky, p2 ground, p3 frame, p4 time, p5 weekday,
// p6 fur cat A, p7 fur cat B, p8 panel bg; o0 night version after sunset, o1 sparkles.
static void faceMascotsDay(const FaceSettings& s, const FaceEnv& e) {
  bool night = s.o[0] && e.night;
  fbFill(0);
  mkRect(0, 0, 64, 34);
  mkFlush(night ? s.p[1] : s.p[0], 0, 0, 64, 34);
  if (night) drawStars(rgb565(255, 241, 168), false);
  else if (s.o[1]) drawSpritePlain(SPR_sparkle, 29, 8);
  fbRect(0, 34, 64, 3, s.p[2].a);
  fbRect(0, 34, 64, 1, lerp565(s.p[2].a, 0xFFFF, 0.35f));
  drawCats(false, s.p[6].a, s.p[7].a);
  mascotPanel(s, e, s.p[8].a, s.p[3].a, s.p[4], s.p[5], night ? &SPR_moon_face : &SPR_sun_face);
}

// Mascotas de noche: p0 sky, p1 ground, p2 frame, p3 time, p4 weekday, p5 fur A,
// p6 fur B, p7 stars, p8 panel bg; o0 buddy (0 snowman, 1 moon).
static void faceMascotsNight(const FaceSettings& s, const FaceEnv& e) {
  fbFill(0);
  mkRect(0, 0, 64, 34);
  mkFlush(s.p[0], 0, 0, 64, 34);
  drawStars(s.p[7].a, true);
  fbRect(0, 34, 64, 3, s.p[1].a);
  fbRect(0, 34, 64, 1, lerp565(s.p[1].a, 0xFFFF, 0.25f));
  drawCats(true, s.p[5].a, s.p[6].a);
  drawSpritePlain(SPR_zzz, 22, 2);
  drawSpritePlain(SPR_zzz, 54, 4);
  mascotPanel(s, e, s.p[8].a, s.p[2].a, s.p[3], s.p[4], s.o[0] == 1 ? &SPR_moon_face : &SPR_snowman);
}

// Tablero: big time, date band, three user pictures, year / weather / month.
// p0 bg, p1 digits, p2 band, p3 band text, p4 frames, p5 bottom text, p6 bottom boxes.
static void faceDashboard(const FaceSettings& s, const FaceEnv& e) {
  fbFill(s.p[0].a);
  char tm[8], a[16];
  fmtTime(tm, sizeof tm, s, e);
  segDraw(tm, 32, 1, 12, 20, 3, 2, 4, 1, 1, s.p[1], nullptr, 0, 0, 0, 0);
  fbRect(0, 23, 64, 8, s.p[2].a);
  int L = faceLang(s);
  if (e.timeValid) {
    snprintf(a, sizeof a, "%02d", e.t.tm_mday);
    drawText(a, 2, 25, 0, 1, 0, s.p[3]);
    drawText(DOW3[L][wday(e, 0)], 61, 25, 0, 1, 2, s.p[3]);
  }
  for (int i = 0; i < 3; i++) {
    int x = 1 + i * 21;
    fbFrame(x, 32, 20, 20, s.p[4].a);
    if (e.slot[i]) {
      for (int j = 0; j < 18; j++)
        for (int k = 0; k < 18; k++) fbSet(x + 1 + k, 33 + j, e.slot[i][j * 18 + k]);
    } else {
      fbRect(x + 1, 33, 18, 18, dim565(s.p[4].a, 0.18f));
      fbRect(x + 9, 38, 2, 8, dim565(s.p[4].a, 0.6f));
      fbRect(x + 6, 41, 8, 2, dim565(s.p[4].a, 0.6f));
    }
  }
  fbRect(1, 53, 20, 10, s.p[6].a);
  fbRect(43, 53, 20, 10, s.p[6].a);
  if (e.timeValid) {
    snprintf(a, sizeof a, "%d", e.t.tm_year + 1900);
    drawText(a, 11, 56, 0, 1, 1, s.p[5]);
    drawText(MON3[L][clampi(e.t.tm_mon, 0, 11)], 53, 56, 0, 1, 1, s.p[5]);
  }
  drawWxNow(e, 27, 53, true);
}

typedef void (*FaceFn)(const FaceSettings&, const FaceEnv&);
struct FaceDef { const char* id; FaceFn fn; };
static const FaceDef FACES[] = {
  {"clasico-digital", faceClassicDigital},
  {"clasico-fecha", faceClassicFecha},
  {"clasico-clima", faceClassicClima},
  {"clasico-analogico", faceClassicAnalog},
  {"clasico-hibrido", faceClassicHibrido},
  {"minimal-apilado", faceMinimal},
  {"marco-arcoiris", faceRainbowFrame},
  {"segmentos-xl", faceSegXL},
  {"clima-4dias", faceWeather4},
  {"mascotas-dia", faceMascotsDay},
  {"mascotas-noche", faceMascotsNight},
  {"tablero", faceDashboard},
};
static const int FACE_COUNT = sizeof(FACES) / sizeof(FACES[0]);
static bool faceExists(const char* id) {
  for (int i = 0; i < FACE_COUNT; i++)
    if (strcmp(FACES[i].id, id) == 0) return true;
  return false;
}
// Renders a face into faceFb (unknown ids fall back to the first classic face).
static void renderFace(const FaceSettings& s, const FaceEnv& e) {
  mkX0 = 64; mkY0 = 64; mkX1 = -1; mkY1 = -1;
  memset(faceMk, 0, sizeof faceMk);
  fbFill(0);
  for (int i = 0; i < FACE_COUNT; i++)
    if (strcmp(FACES[i].id, s.face) == 0) { FACES[i].fn(s, e); return; }
  FACES[0].fn(s, e);
}
// <<< FACE CODE

// =========================
// Persisted display state
// =========================
// What the panel shows is saved to the microSD so it survives power loss:
//   /config/state.json   kind (frame | anim | clock | idle), anim path, brightness, clock config
//   /config/last.rgb565  last static frame (kind == frame)
// Writes go to a .tmp file and are renamed, so an unplug mid-write never leaves
// a corrupt state. Saves are debounced (STATE_SAVE_DELAY_MS) to spare the SD.
const char* CONFIG_DIR = "/config";
const char* STATE_PATH = "/config/state.json";
const char* STATE_TMP = "/config/state.tmp";
const char* LAST_FRAME_PATH = "/config/last.rgb565";
const char* LAST_FRAME_TMP = "/config/last.tmp";
constexpr uint32_t STATE_SAVE_DELAY_MS = 1500;
String stateKind = "";
String stateAnim = "";
bool stateDirty = false;
uint32_t stateDirtyAt = 0;

void markStateDirty(){ stateDirty = true; stateDirtyAt = millis(); }
void markState(const char* kind, const String& anim = ""){ stateKind = kind; stateAnim = anim; markStateDirty(); }

String jsonString(const String& json, const char* key){
  String needle = String("\"") + key + "\":\"";
  int p = json.indexOf(needle);
  if (p < 0) return "";
  p += needle.length();
  int e = json.indexOf('"', p);
  return e < 0 ? "" : json.substring(p, e);
}

bool writeAtomic(const char* path, const char* tmp, const uint8_t* data, size_t len){
  SD_MMC.remove(tmp);
  File f = SD_MMC.open(tmp, FILE_WRITE);
  if (!f) return false;
  bool ok = f.write(data, len) == len;
  f.close();
  if (!ok) { SD_MMC.remove(tmp); return false; }
  SD_MMC.remove(path);
  return SD_MMC.rename(tmp, path);
}

// Falls back to the .tmp file if power was lost between remove() and rename().
File openWithFallback(const char* path, const char* tmp){
  if (SD_MMC.exists(path)) return SD_MMC.open(path, FILE_READ);
  if (SD_MMC.exists(tmp)) return SD_MMC.open(tmp, FILE_READ);
  return File();
}

void saveState(){
  ensureDir(CONFIG_DIR);
  if (stateKind == "frame") writeAtomic(LAST_FRAME_PATH, LAST_FRAME_TMP, frameBuffer, FRAME_BYTES);
  String j = "{\"v\":1";
  j += ",\"kind\":\"" + stateKind + "\"";
  j += ",\"anim\":\"" + stateAnim + "\"";
  j += ",\"brightness\":" + String(clockCfg.brightness);
  j += ",\"cMode\":" + String(clockCfg.mode);
  j += ",\"cH24\":" + String(clockCfg.hour24 ? 1 : 0);
  j += ",\"cSeconds\":" + String(clockCfg.showSeconds ? 1 : 0);
  j += ",\"cDate\":" + String(clockCfg.showDate ? 1 : 0);
  j += ",\"cTemp\":" + String(clockCfg.showTemp ? 1 : 0);
  j += ",\"cHumidity\":" + String(clockCfg.showHumidity ? 1 : 0);
  j += ",\"cWeather\":" + String(clockCfg.showWeather ? 1 : 0);
  j += ",\"cBg\":" + String(clockCfg.bg);
  j += ",\"cPrimary\":" + String(clockCfg.primary);
  j += ",\"cSecondary\":" + String(clockCfg.secondary);
  j += ",\"cAccent\":" + String(clockCfg.accent);
  j += ",\"cWeatherColor\":" + String(clockCfg.weatherColor);
  j += "}";
  if (writeAtomic(STATE_PATH, STATE_TMP, (const uint8_t*)j.c_str(), j.length())) {
    Log.println("Estado guardado: " + (stateKind.length() ? stateKind : String("(vacio)")));
  } else {
    Log.println("ERROR guardando estado en microSD");
  }
}

void serviceState(){
  if (stateDirty && millis() - stateDirtyAt >= STATE_SAVE_DELAY_MS) {
    stateDirty = false;
    saveState();
  }
}

// Default screen for a fresh device (no saved state) or when restore fails.
void showSplash(){
  stopClock(); stopAnim();
  display.clearDisplay();
  display.setTextWrap(false);
  display.setTextSize(2);
  const char* letters = "RGB";
  const uint16_t colors[3] = { display.color565(255, 0, 0), display.color565(0, 255, 0), display.color565(0, 90, 255) };
  for (int i = 0; i < 3; i++) {
    display.setTextColor(colors[i]);
    display.setCursor(15 + i * 12, 16);
    display.print(letters[i]);
  }
  display.setTextSize(1);
  display.setTextColor(display.color565(255, 255, 255));
  display.setCursor(18, 40);
  display.print("Panel");
  display.showBuffer();
}

void restoreState(){
  ensureDir(CONFIG_DIR);
  String j;
  File sf = openWithFallback(STATE_PATH, STATE_TMP);
  if (sf) { j = sf.readString(); sf.close(); }
  if (!j.length()) { Log.println("Sin estado guardado: pantalla inicial"); showSplash(); return; }

  uint8_t b = (uint8_t)constrain((int)jsonNumber(j, "brightness", 20), 1, 255);
  clockCfg.brightness = b;
  display.setBrightness(b);

  String kind = jsonString(j, "kind");
  if (kind == "frame") {
    File ff = openWithFallback(LAST_FRAME_PATH, LAST_FRAME_TMP);
    if (ff && ff.size() == FRAME_BYTES && ff.read(frameBuffer, FRAME_BYTES) == FRAME_BYTES) {
      ff.close(); applyFrame(); stateKind = kind;
      Log.println("Estado restaurado: frame"); return;
    }
    if (ff) ff.close();
  } else if (kind == "anim") {
    String p = jsonString(j, "anim");
    if (p.length() && playAnim(p)) {
      stateKind = kind; stateAnim = p;
      Log.println("Estado restaurado: animacion " + p); return;
    }
  } else if (kind == "clock") {
    FaceSettings s;
    if (loadClockCfg(s)) {
      faceCfg = s;
    } else {
      // Saved by an older firmware: turn the old clock mode into its classic face.
      faceDefaults(s);
      int mode = constrain((int)jsonNumber(j, "cMode", 0), 0, 4);
      strncpy(s.face, LEGACY_FACES[mode], sizeof(s.face) - 1);
      s.h24 = jsonNumber(j, "cH24", 1) != 0;
      const uint16_t c[5] = {(uint16_t)jsonNumber(j, "cBg", 0), (uint16_t)jsonNumber(j, "cPrimary", 0xFFFF),
                             (uint16_t)jsonNumber(j, "cSecondary", 0x269D), (uint16_t)jsonNumber(j, "cAccent", 0xFB46),
                             (uint16_t)jsonNumber(j, "cWeatherColor", 0x7E9F)};
      for (int i = 0; i < 5; i++) s.p[i] = {c[i], c[i], 0};
      s.o[0] = jsonNumber(j, "cSeconds", 0) != 0;
      s.o[1] = jsonNumber(j, "cDate", 1) != 0;
      s.o[2] = jsonNumber(j, "cTemp", 1) != 0;
      s.o[3] = jsonNumber(j, "cHumidity", 1) != 0;
      s.o[4] = jsonNumber(j, "cWeather", 1) != 0;
      faceCfg = s;
      saveClockCfg();
      Log.println("Reloj anterior migrado a la caratula " + String(s.face));
    }
    loadFaceSlots();
    stopAnim();
    clockCfg.enabled = true;
    forceWeatherRefresh = true;
    lastClockSecond = 0;
    renderClock();
    stateKind = kind;
    Log.println("Estado restaurado: reloj " + String(faceCfg.face)); return;
  }
  Log.println("Estado '" + kind + "' no restaurable: pantalla inicial");
  showSplash();
}

// =========================
// Clock runtime
// =========================
// The active face and its settings live in faceCfg (saved in /config/clock.json).

// Defaults of the classic faces (also used when nothing was saved yet).
void faceDefaults(FaceSettings& s) {
  memset(&s, 0, sizeof(s));
  strncpy(s.face, LEGACY_FACES[0], sizeof(s.face) - 1);
  s.h24 = true;
  const uint16_t cols[5] = {0x0000, 0xFFFF, 0x269D, 0xFB46, 0x7E9F};  // bg, primary, secondary, accent, weather
  for (int i = 0; i < FACE_PAINTS; i++) s.p[i] = {i < 5 ? cols[i] : (uint16_t)0, i < 5 ? cols[i] : (uint16_t)0, 0};
  s.o[1] = s.o[2] = s.o[3] = s.o[4] = 1;
}

// Reads face settings from the request (face, h24, lang, blink, a0..a9, b0..b9, d0..d9, o0..o7).
void faceFromArgs(FaceSettings& s) {
  String f = server.arg("face");
  if (f.length() && faceExists(f.c_str())) {
    memset(s.face, 0, sizeof(s.face));
    strncpy(s.face, f.c_str(), sizeof(s.face) - 1);
  }
  if (server.hasArg("h24")) s.h24 = server.arg("h24") == "1";
  if (server.hasArg("lang")) s.lang = server.arg("lang") == "1" ? 1 : 0;
  if (server.hasArg("blink")) s.blink = server.arg("blink") == "1";
  for (int i = 0; i < FACE_PAINTS; i++) {
    String k = String(i);
    if (server.hasArg("a" + k)) s.p[i].a = parseHex565(server.arg("a" + k));
    if (server.hasArg("b" + k)) s.p[i].b = parseHex565(server.arg("b" + k));
    if (server.hasArg("d" + k)) s.p[i].dir = (uint8_t)constrain(server.arg("d" + k).toInt(), 0, 4);
  }
  for (int i = 0; i < FACE_OPTS; i++) {
    String k = "o" + String(i);
    if (server.hasArg(k)) s.o[i] = (uint8_t)constrain(server.arg(k).toInt(), 0, 255);
  }
}

String hex565(uint16_t c) {
  char b[8];
  int r = ((c >> 11) & 31) * 255 / 31, g = ((c >> 5) & 63) * 255 / 63, bl = (c & 31) * 255 / 31;
  snprintf(b, sizeof(b), "#%02x%02x%02x", r, g, bl);
  return String(b);
}

void saveClockCfg() {
  String j = "{\"face\":\"" + String(faceCfg.face) + "\",\"h24\":" + String(faceCfg.h24 ? 1 : 0) +
             ",\"lang\":" + String(faceCfg.lang) + ",\"blink\":" + String(faceCfg.blink ? 1 : 0);
  const char* keys[3] = {"a", "b", "d"};
  for (int k = 0; k < 3; k++) {
    j += ",\"" + String(keys[k]) + "\":[";
    for (int i = 0; i < FACE_PAINTS; i++) {
      if (i) j += ",";
      j += String(k == 0 ? faceCfg.p[i].a : k == 1 ? faceCfg.p[i].b : faceCfg.p[i].dir);
    }
    j += "]";
  }
  j += ",\"o\":[";
  for (int i = 0; i < FACE_OPTS; i++) {
    if (i) j += ",";
    j += String(faceCfg.o[i]);
  }
  j += "]}";
  ensureDir(CONFIG_DIR);
  if (!writeAtomic(CLOCK_PATH, CLOCK_TMP, (const uint8_t*)j.c_str(), j.length())) Log.println("ERROR guardando carátula en microSD");
}

bool loadClockCfg(FaceSettings& s) {
  File f = openWithFallback(CLOCK_PATH, CLOCK_TMP);
  if (!f) return false;
  String j = f.readString();
  f.close();
  String id = jsonString(j, "face");
  if (!faceExists(id.c_str())) return false;
  faceDefaults(s);
  memset(s.face, 0, sizeof(s.face));
  strncpy(s.face, id.c_str(), sizeof(s.face) - 1);
  s.h24 = jsonNumber(j, "h24", 1) != 0;
  s.lang = jsonNumber(j, "lang", 0) == 1 ? 1 : 0;
  s.blink = jsonNumber(j, "blink", 0) != 0;
  float v[FACE_PAINTS];
  int n = jsonNumbers(j, 0, "a", v, FACE_PAINTS);
  for (int i = 0; i < n; i++) s.p[i].a = (uint16_t)v[i];
  n = jsonNumbers(j, 0, "b", v, FACE_PAINTS);
  for (int i = 0; i < n; i++) s.p[i].b = (uint16_t)v[i];
  n = jsonNumbers(j, 0, "d", v, FACE_PAINTS);
  for (int i = 0; i < n; i++) s.p[i].dir = (uint8_t)constrain((int)v[i], 0, 4);
  n = jsonNumbers(j, 0, "o", v, FACE_OPTS);
  for (int i = 0; i < n; i++) s.o[i] = (uint8_t)constrain((int)v[i], 0, 255);
  return true;
}

// Pictures of the "tablero" face: /config/slot0..2.rgb565 (18x18 RGB565, little endian).
void loadFaceSlots() {
  for (int i = 0; i < 3; i++) {
    faceSlotOk[i] = false;
    String p = String(CONFIG_DIR) + "/slot" + String(i) + ".rgb565";
    File f = SD_MMC.open(p, FILE_READ);
    if (!f) continue;
    if (f.size() == sizeof(faceSlots[i])) faceSlotOk[i] = f.read((uint8_t*)faceSlots[i], sizeof(faceSlots[i])) == sizeof(faceSlots[i]);
    f.close();
  }
}

// nightMode: 0 = real day/night, 1 = force day, 2 = force night (editor preview).
void faceEnvNow(FaceEnv& e, int nightMode) {
  memset(&e, 0, sizeof(e));
  time_t now = time(nullptr);
  localtime_r(&now, &e.t);
  e.timeValid = e.t.tm_year > (2016 - 1900);
  bool isDay;
  int sr, ss;
  portENTER_CRITICAL(&wxMux);
  e.wxValid = weatherValid;
  e.temp = weatherTempC;
  e.hum = weatherHumidityPct;
  e.code = weatherCode;
  isDay = weatherIsDay;
  sr = wxSunriseMin;
  ss = wxSunsetMin;
  for (int i = 0; i < 4; i++) e.d[i] = wxDays[i];
  portEXIT_CRITICAL(&wxMux);
  int minutes = e.t.tm_hour * 60 + e.t.tm_min;
  if (e.timeValid && sr >= 0 && ss >= 0) e.night = minutes < sr || minutes >= ss;
  else if (e.wxValid) e.night = !isDay;
  else e.night = e.t.tm_hour < 7 || e.t.tm_hour >= 19;
  if (nightMode == 1) e.night = false;
  else if (nightMode == 2) e.night = true;
  e.wifi = WiFi.status() == WL_CONNECTED;
  e.rssi = WiFi.RSSI();
  for (int i = 0; i < 3; i++) e.slot[i] = faceSlotOk[i] ? faceSlots[i] : nullptr;
}

void blitFace() {
  uint32_t started = millis();
  display.clearDisplay();
  for (int y = 0; y < 64; y++)
    for (int x = 0; x < 64; x++) display.drawPixelRGB565(x, y, faceFb[y * 64 + x]);
  display.showBuffer();
  lastRenderMs = millis() - started;
}

void renderClock() {
  if (!clockCfg.enabled) return;
  FaceEnv e;
  faceEnvNow(e, 0);
  renderFace(faceCfg, e);
  blitFace();
}

// Redraws once per second, right when the second changes.
void serviceClock() {
  if (!clockCfg.enabled || previewHoldUntil) return;
  time_t now = time(nullptr);
  if (now == lastClockSecond) return;
  lastClockSecond = now;
  renderClock();
}

void stopClock() {
  clockCfg.enabled = false;
}

// Loop-side housekeeping: end of an editor preview and time zone changes
// reported by the weather task (applied here, on the loop core).
void serviceClockAux() {
  if (previewHoldUntil && (int32_t)(millis() - previewHoldUntil) >= 0) {
    previewHoldUntil = 0;
    lastClockSecond = 0;
    if (!clockCfg.enabled && !animationPlaying) {
      if (stateKind == "frame") applyFrame();
      else showSplash();
    }
  }
  if (utcOffsetPending) {
    int32_t off;
    portENTER_CRITICAL(&wxMux);
    off = pendingUtcOffset;
    utcOffsetPending = false;
    location.utcOffset = off;
    portEXIT_CRITICAL(&wxMux);
    applyUtcOffset(off);
    saveLocation();
    lastClockSecond = 0;
    Log.println("Zona horaria: UTC" + String(off >= 0 ? "+" : "") + String(off / 3600.0f, 1));
  }
}

// =========================
// Location (weather + time zone)
// =========================
void loadLocation() {
  File f = openWithFallback(LOCATION_PATH, LOCATION_TMP);
  if (!f) return;
  String j = f.readString();
  f.close();
  String name = jsonString(j, "name");
  float lat = jsonNumber(j, "lat", NAN), lon = jsonNumber(j, "lon", NAN);
  int32_t off = (int32_t)jsonNumber(j, "tz", (float)location.utcOffset);
  if (isnan(lat) || isnan(lon) || lat < -90 || lat > 90 || lon < -180 || lon > 180) return;
  portENTER_CRITICAL(&wxMux);
  if (name.length()) {
    memset(location.name, 0, sizeof(location.name));
    strncpy(location.name, name.c_str(), sizeof(location.name) - 1);
  }
  location.lat = lat;
  location.lon = lon;
  if (off > -50400 && off < 50400) location.utcOffset = off;
  portEXIT_CRITICAL(&wxMux);
}

void saveLocation() {
  LocationCfg loc;
  portENTER_CRITICAL(&wxMux);
  loc = location;
  portEXIT_CRITICAL(&wxMux);
  String j = "{\"name\":\"" + jsonEsc(String(loc.name)) + "\",\"lat\":" + String(loc.lat, 4) +
             ",\"lon\":" + String(loc.lon, 4) + ",\"tz\":" + String(loc.utcOffset) + "}";
  ensureDir(CONFIG_DIR);
  if (!writeAtomic(LOCATION_PATH, LOCATION_TMP, (const uint8_t*)j.c_str(), j.length())) Log.println("ERROR guardando ubicacion en microSD");
}

// =========================
// WiFi / mDNS
// =========================
void startMDNS(){if(mdnsReady){MDNS.end();mdnsReady=false;}if(MDNS.begin(MDNS_HOST)){MDNS.addService("http","tcp",80);mdnsReady=true;}}
void connectWiFi(){
  // Credentials are passed on every boot; skip saving them to NVS so Wi-Fi
  // (re)connects never write flash, which stalls both cores and the refresh.
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA); WiFi.setSleep(false); WiFi.begin(WIFI_SSID,WIFI_PASSWORD);
  // Keep a restored animation/clock running while waiting for the network.
  while(WiFi.status()!=WL_CONNECTED){serviceAnim();serviceClock();delay(10);}
  configTime(location.utcOffset, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  startMDNS();
}
void serviceWiFi(){
  if(millis()-lastWifiCheck<5000)return;lastWifiCheck=millis();
  static bool wasConnected=true;
  if(WiFi.status()==WL_CONNECTED){
    if(!wasConnected){wasConnected=true;Log.println("Wi-Fi reconectado: "+WiFi.localIP().toString());}
    if(!mdnsReady)startMDNS();return;
  }
  if(wasConnected){wasConnected=false;Log.println("Wi-Fi perdido, reconectando...");}
  mdnsReady=false;WiFi.disconnect();WiFi.begin(WIFI_SSID,WIFI_PASSWORD);
}

// =========================
// Upload handlers
// =========================
void frameUpload(){
  HTTPUpload& u=server.upload();
  if(u.status==UPLOAD_FILE_START){uploadBytes=0;uploadOK=true;stopAnim();stopClock();}
  else if(u.status==UPLOAD_FILE_WRITE){if(uploadBytes+u.currentSize<=FRAME_BYTES)memcpy(frameBuffer+uploadBytes,u.buf,u.currentSize);else uploadOK=false;uploadBytes+=u.currentSize;}
  else if(u.status==UPLOAD_FILE_END){if(uploadBytes!=FRAME_BYTES)uploadOK=false;}
  else if(u.status==UPLOAD_FILE_ABORTED)uploadOK=false;
}

void genericUpload(){
  HTTPUpload& u=server.upload();
  if(u.status==UPLOAD_FILE_START){
    uploadOK=true;
    String path=safePath(server.arg("path"));
    if(SD_MMC.exists(path))SD_MMC.remove(path);
    uploadFile=SD_MMC.open(path,FILE_WRITE);
    if(!uploadFile)uploadOK=false;
  }else if(u.status==UPLOAD_FILE_WRITE){
    if(!uploadFile||uploadFile.write(u.buf,u.currentSize)!=u.currentSize)uploadOK=false;
    
  }else if(u.status==UPLOAD_FILE_END){if(uploadFile){Log.println(String("Archivo guardado: ")+uploadFile.path()+" ("+String((uint32_t)u.totalSize)+" B)");uploadFile.close();}}
  else if(u.status==UPLOAD_FILE_ABORTED){if(uploadFile)uploadFile.close();uploadOK=false;}
}

void firmwareUpload(){
  HTTPUpload& u=server.upload();
  if(u.status==UPLOAD_FILE_START){
    otaSuccess=false; otaError="";
    stopAnim();
    Log.println("OTA: recibiendo firmware...");
    if(!Update.begin(UPDATE_SIZE_UNKNOWN)){otaError=Update.errorString();}
  }else if(u.status==UPLOAD_FILE_WRITE){
    if(otaError.length()==0 && Update.write(u.buf,u.currentSize)!=u.currentSize)otaError=Update.errorString();
    
  }else if(u.status==UPLOAD_FILE_END){
    if(otaError.length()==0){
      if(Update.end(true))otaSuccess=true; else otaError=Update.errorString();
    }
  }else if(u.status==UPLOAD_FILE_ABORTED){Update.abort();otaError="Upload abortado";}
}

// =========================
// API
// =========================
String listJson(String path){
  File d=SD_MMC.open(path);
  if(!d||!d.isDirectory())return "{\"items\":[]}";
  String j="{\"items\":[";bool first=true;File f=d.openNextFile();
  while(f){if(!first)j+=",";first=false;String n=f.name();int s=n.lastIndexOf('/');if(s>=0)n=n.substring(s+1);j+="{\"name\":\""+n+"\",\"path\":\""+String(f.path())+"\",\"dir\":"+(f.isDirectory()?"true":"false")+",\"size\":"+String((uint32_t)f.size())+"}";f.close();f=d.openNextFile();}
  d.close();j+="]}";return j;
}
String galleryJson(){
  String j="{\"images\":";j+=listJson(IMAGE_DIR).substring(9); // replace key cheaply below
  // easier rebuild
  File d=SD_MMC.open(IMAGE_DIR);j="{\"images\":[";bool first=true;File f=d.openNextFile();
  while(f){if(!f.isDirectory()){if(!first)j+=",";first=false;String n=f.name();int s=n.lastIndexOf('/');if(s>=0)n=n.substring(s+1);j+="{\"name\":\""+n+"\",\"size\":"+String((uint32_t)f.size())+"}";}f.close();f=d.openNextFile();}d.close();
  j+="],\"animations\":[";d=SD_MMC.open(ANIM_DIR);first=true;f=d.openNextFile();
  while(f){if(!f.isDirectory()){if(!first)j+=",";first=false;String n=f.name();int s=n.lastIndexOf('/');if(s>=0)n=n.substring(s+1);j+="{\"name\":\""+n+"\",\"size\":"+String((uint32_t)f.size())+"}";}f.close();f=d.openNextFile();}d.close();j+="]}";return j;
}

// Lists stored GIFs from GIF_DIR. Each entry keeps a <base>.gif (preview) and
// a <base>.pma (PMA2 played on the panel). Only the .gif files are reported.
String gifsJson(){
  String j="{\"gifs\":[";File d=SD_MMC.open(GIF_DIR);bool first=true;
  if(d){File f=d.openNextFile();
    while(f){
      if(!f.isDirectory()){
        String n=f.name();int s=n.lastIndexOf('/');if(s>=0)n=n.substring(s+1);
        if(n.endsWith(".gif")){
          if(!first)j+=",";first=false;
          String base=n.substring(0,n.length()-4);
          j+="{\"name\":\""+base+"\",\"size\":"+String((uint32_t)f.size())+"}";
        }
      }
      f.close();f=d.openNextFile();
    }
    d.close();
  }
  j+="]}";return j;
}

String jsonEsc(const String& in){
  String o; o.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((uint8_t)c < 0x20) { char b[7]; snprintf(b, sizeof(b), "\\u%04x", (uint8_t)c); o += b; }
    else o += c;
  }
  return o;
}

const char* resetReasonText(){
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "Encendido / corte de energia";
    case ESP_RST_SW: return "Reinicio por software (OTA)";
    case ESP_RST_PANIC: return "Fallo del firmware (panic)";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: return "Watchdog";
    case ESP_RST_BROWNOUT: return "Bajo voltaje (brownout)";
    case ESP_RST_EXT: return "Boton reset";
    case ESP_RST_DEEPSLEEP: return "Deep sleep";
    default: return "Desconocido";
  }
}

void setupServer(){
  server.on("/",HTTP_GET,[]{
    server.sendHeader("Cache-Control","no-store, no-cache, must-revalidate, max-age=0");
    if(!serveFile("/www/index.html"))server.send(500,"text/plain","Falta index.html");
  });

  server.on("/tailwind.css",HTTP_GET,[]{
    server.sendHeader("Cache-Control","no-store, no-cache, must-revalidate, max-age=0");
    if(!serveFile("/www/tailwind.css"))server.send(404,"text/plain","No CSS");
  });

  // Compatibilidad con HTML viejo que todavía pida /app.css.
  server.on("/app.css",HTTP_GET,[]{
    server.sendHeader("Cache-Control","no-store, no-cache, must-revalidate, max-age=0");
    if(!serveFile("/www/tailwind.css"))server.send(404,"text/plain","No CSS");
  });

  server.on("/app.js",HTTP_GET,[]{
    server.sendHeader("Cache-Control","no-store, no-cache, must-revalidate, max-age=0");
    if(!serveFile("/www/app.js"))server.send(404,"text/plain","No JS");
  });

  server.on("/api/frame",HTTP_POST,[]{if(uploadOK&&uploadBytes==FRAME_BYTES){applyFrame();markState("frame");Log.println("Frame recibido: render "+String(lastRenderMs)+" ms");String j="{\"ok\":true,\"renderMs\":"+String(lastRenderMs)+"}";server.send(200,"application/json",j);}else server.send(400,"text/plain","Frame invalido");},frameUpload);


  server.on("/api/brightness", HTTP_POST, [](){
    int value = constrain(server.arg("v").toInt(), 1, 255);
    display.setBrightness((uint8_t)value);
    clockCfg.brightness = value;
    markStateDirty();
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/clock/config", HTTP_POST, [](){
    FaceSettings s = faceCfg;
    if (!faceExists(s.face)) faceDefaults(s);
    faceFromArgs(s);
    faceCfg = s;
    loadFaceSlots();
    stopAnim();
    previewHoldUntil = 0;
    clockCfg.enabled = true;
    if (!weatherValid) forceWeatherRefresh = true;
    lastClockSecond = 0;
    renderClock();
    saveClockCfg();
    markState("clock");
    Log.println("Reloj activado: " + String(faceCfg.face));
    server.send(200, "application/json", "{\"ok\":true}");
  });

  // Renders a face with the settings of the request (not saved) and returns
  // its 64x64 pixels as RGB565 little endian. panel=1 also shows it on the
  // panel for 20 s; night=day|night forces the day/night version.
  server.on("/api/clock/preview", HTTP_POST, [](){
    FaceSettings s;
    faceDefaults(s);
    faceFromArgs(s);
    String n = server.arg("night");
    FaceEnv e;
    faceEnvNow(e, n == "day" ? 1 : n == "night" ? 2 : 0);
    renderFace(s, e);
    if (server.arg("panel") == "1") {
      previewHoldUntil = millis() + 20000;
      if (!previewHoldUntil) previewHoldUntil = 1;
      blitFace();
    }
    server.sendHeader("Cache-Control", "no-store");
    server.setContentLength(sizeof(faceFb));
    server.send(200, "application/octet-stream", "");
    server.sendContent((const char*)faceFb, sizeof(faceFb));
  });

  server.on("/api/clock/preview/end", HTTP_POST, [](){
    if (previewHoldUntil) previewHoldUntil = millis() ? millis() : 1;
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/clock/stop", HTTP_POST, [](){
    stopClock();
    markState("idle");
    Log.println("Modo reloj desactivado");
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/clock/weather", HTTP_POST, [](){
    forceWeatherRefresh = true;
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/clock/status", HTTP_GET, [](){
    FaceEnv e;
    faceEnvNow(e, 0);
    LocationCfg loc;
    float feels;
    portENTER_CRITICAL(&wxMux);
    loc = location;
    feels = weatherFeelsC;
    portEXIT_CRITICAL(&wxMux);
    String j = "{";
    j += "\"enabled\":" + String(clockCfg.enabled ? "true" : "false");
    j += ",\"face\":\"" + String(faceCfg.face) + "\"";
    j += ",\"h24\":" + String(faceCfg.h24 ? 1 : 0) + ",\"lang\":" + String(faceCfg.lang) + ",\"blink\":" + String(faceCfg.blink ? 1 : 0);
    j += ",\"a\":[";
    for (int i = 0; i < FACE_PAINTS; i++) j += String(i ? "," : "") + "\"" + hex565(faceCfg.p[i].a) + "\"";
    j += "],\"b\":[";
    for (int i = 0; i < FACE_PAINTS; i++) j += String(i ? "," : "") + "\"" + hex565(faceCfg.p[i].b) + "\"";
    j += "],\"d\":[";
    for (int i = 0; i < FACE_PAINTS; i++) j += String(i ? "," : "") + String(faceCfg.p[i].dir);
    j += "],\"o\":[";
    for (int i = 0; i < FACE_OPTS; i++) j += String(i ? "," : "") + String(faceCfg.o[i]);
    j += "],\"brightness\":" + String(clockCfg.brightness);
    j += ",\"city\":\"" + jsonEsc(String(loc.name)) + "\",\"lat\":" + String(loc.lat, 4) + ",\"lon\":" + String(loc.lon, 4) + ",\"tz\":" + String(loc.utcOffset);
    j += ",\"weatherValid\":" + String(e.wxValid ? "true" : "false");
    j += ",\"temp\":" + String(e.temp, 1) + ",\"humidity\":" + String(e.hum, 0) + ",\"feels\":" + String(feels, 1);
    j += ",\"weatherCode\":" + String(e.code) + ",\"weatherText\":\"" + String(weatherShort(e.code)) + "\"";
    j += ",\"night\":" + String(e.night ? "true" : "false");
    j += ",\"days\":[";
    for (int i = 0; i < 4; i++) {
      const WxDay& d = e.d[i];
      if (i) j += ",";
      j += "{\"valid\":" + String(d.valid ? "true" : "false") + ",\"code\":" + String(d.code) + ",\"max\":" + String(d.tmax, 1) +
           ",\"min\":" + String(d.tmin, 1) + ",\"hum\":" + String(d.hum, 0) + "}";
    }
    j += "]}";
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", j);
  });

  server.on("/api/location", HTTP_GET, [](){
    LocationCfg loc;
    portENTER_CRITICAL(&wxMux);
    loc = location;
    portEXIT_CRITICAL(&wxMux);
    server.send(200, "application/json", "{\"name\":\"" + jsonEsc(String(loc.name)) + "\",\"lat\":" + String(loc.lat, 4) +
                ",\"lon\":" + String(loc.lon, 4) + ",\"tz\":" + String(loc.utcOffset) + "}");
  });

  server.on("/api/location", HTTP_POST, [](){
    if (!server.hasArg("lat") || !server.hasArg("lon")) { server.send(400, "text/plain", "Faltan coordenadas"); return; }
    float lat = server.arg("lat").toFloat(), lon = server.arg("lon").toFloat();
    if (lat < -90 || lat > 90 || lon < -180 || lon > 180) { server.send(400, "text/plain", "Coordenadas invalidas"); return; }
    String name = server.arg("name");
    name.replace("\"", "");
    name.replace("\\", "");
    name.trim();
    if (!name.length()) name = String(lat, 2) + ", " + String(lon, 2);
    portENTER_CRITICAL(&wxMux);
    memset(location.name, 0, sizeof(location.name));
    strncpy(location.name, name.c_str(), sizeof(location.name) - 1);
    location.lat = lat;
    location.lon = lon;
    portEXIT_CRITICAL(&wxMux);
    saveLocation();
    forceWeatherRefresh = true;
    Log.println("Ubicacion: " + name + " (" + String(lat, 4) + ", " + String(lon, 4) + ")");
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/fs/read", HTTP_GET, [](){
    server.sendHeader("Cache-Control", "no-store");
    if (!serveFile(safePath(server.arg("path")))) server.send(404, "text/plain", "No encontrado");
  });

  server.on("/api/gallery/save-image",HTTP_POST,[]{
    String name=safeName(server.arg("name")); if(!name.endsWith(".rgb565"))name+=".rgb565";
    String p=String(IMAGE_DIR)+"/"+name;if(SD_MMC.exists(p))SD_MMC.remove(p);
    File f=SD_MMC.open(p,FILE_WRITE);if(!f){server.send(500,"text/plain","No se pudo guardar");return;}
    f.write(frameBuffer,FRAME_BYTES);f.close();server.send(200,"application/json","{\"ok\":true}");
  });
  server.on("/api/gallery",HTTP_GET,[]{server.send(200,"application/json",galleryJson());});
  server.on("/api/gallery/show-image",HTTP_POST,[]{
    String p=String(IMAGE_DIR)+"/"+safeName(server.arg("name"));File f=SD_MMC.open(p,FILE_READ);
    if(!f||f.size()!=FRAME_BYTES){server.send(404,"text/plain","No encontrado");return;}
    f.read(frameBuffer,FRAME_BYTES);f.close();stopClock();stopAnim();applyFrame();markState("frame");Log.println("Mostrando imagen: "+p);server.send(200,"application/json","{\"ok\":true}");
  });
  server.on("/api/gallery/play-animation",HTTP_POST,[]{
    String p=String(ANIM_DIR)+"/"+safeName(server.arg("name"));if(!playAnim(p))server.send(400,"text/plain","Animacion invalida");else{markState("anim",p);Log.println("Reproduciendo: "+p);server.send(200,"application/json","{\"ok\":true}");}
  });
  server.on("/api/gallery/delete",HTTP_DELETE,[]{
    String type=server.arg("type"),name=safeName(server.arg("name"));
    String p=(type=="image"?String(IMAGE_DIR):String(ANIM_DIR))+"/"+name;
    stopAnim(); if(stateKind=="anim"&&stateAnim==p)markState("idle"); if(!SD_MMC.remove(p))server.send(500,"text/plain","No se pudo eliminar");else server.send(200,"application/json","{\"ok\":true}");
  });

  server.on("/api/gifs",HTTP_GET,[]{server.send(200,"application/json",gifsJson());});
  server.on("/gifs",HTTP_GET,[]{
    String name=safeName(server.arg("name"));if(!name.endsWith(".gif"))name+=".gif";
    String p=String(GIF_DIR)+"/"+name;
    if(!SD_MMC.exists(p)){server.send(404,"text/plain","No encontrado");return;}
    File f=SD_MMC.open(p,FILE_READ);server.streamFile(f,"image/gif");f.close();
  });
  server.on("/api/gifs/play",HTTP_POST,[]{
    String p=String(GIF_DIR)+"/"+safeName(server.arg("name"))+".pma";
    if(!playAnim(p))server.send(400,"text/plain","GIF invalido");else{markState("anim",p);Log.println("Reproduciendo: "+p);server.send(200,"application/json","{\"ok\":true}");}
  });
  server.on("/api/gifs/delete",HTTP_DELETE,[]{
    String base=safeName(server.arg("name"));stopAnim();
    if(stateKind=="anim"&&stateAnim==String(GIF_DIR)+"/"+base+".pma")markState("idle");
    Log.println("GIF borrado: "+base);
    SD_MMC.remove(String(GIF_DIR)+"/"+base+".gif");
    SD_MMC.remove(String(GIF_DIR)+"/"+base+".pma");
    server.send(200,"application/json","{\"ok\":true}");
  });

  server.on("/api/fs/list",HTTP_GET,[]{server.send(200,"application/json",listJson(safePath(server.arg("path"))));});
  server.on("/api/fs/mkdir",HTTP_POST,[]{String p=safePath(server.arg("path"));if(SD_MMC.mkdir(p))server.send(200,"application/json","{\"ok\":true}");else server.send(500,"text/plain","No se pudo crear");});
  server.on("/api/fs/delete",HTTP_DELETE,[]{String p=safePath(server.arg("path"));bool ok=SD_MMC.remove(p)||SD_MMC.rmdir(p);if(ok)server.send(200,"application/json","{\"ok\":true}");else server.send(500,"text/plain","No se pudo eliminar");});
  server.on("/api/fs/upload",HTTP_POST,[]{if(uploadOK)server.send(200,"application/json","{\"ok\":true}");else server.send(500,"text/plain","Upload fallido");},genericUpload);

  server.on("/api/firmware",HTTP_POST,[]{
    if(otaSuccess){Log.println("OTA: firmware instalado, reiniciando");server.send(200,"text/plain","Firmware instalado. Reiniciando...");delay(500);ESP.restart();}
    else{Log.println("OTA ERROR: "+(otaError.length()?otaError:String("desconocido")));server.send(500,"text/plain",otaError.length()?otaError:"Error OTA");}
  },firmwareUpload);


  server.on("/remote",HTTP_GET,[]{
    String name=safeName(server.arg("name"));
    String p=String(REMOTE_DIR)+"/"+name;
    if(!SD_MMC.exists(p)){server.send(404,"text/plain","No encontrado");return;}
    File f=SD_MMC.open(p,FILE_READ);
    String ct="image/jpeg";
    if(name.endsWith(".png"))ct="image/png";
    else if(name.endsWith(".webp"))ct="image/webp";
    server.streamFile(f,ct);f.close();
  });

  server.on("/api/pixilart/import",HTTP_POST,[]{
    String pageUrl=server.arg("url");
    if(!pageUrl.startsWith("https://www.pixilart.com/") && !pageUrl.startsWith("https://es.pixilart.com/")){
      server.send(400, "application/json", R"json({"error":"La URL debe ser publica y pertenecer a pixilart.com"})json");
      return;
    }

    WiFiClientSecure secure;
    secure.setInsecure();
    HTTPClient http;

    if(!http.begin(secure,pageUrl)){
      server.send(502, "application/json", R"json({"error":"No se pudo abrir Pixilart"})json");
      return;
    }

    http.setUserAgent("Mozilla/5.0 MatrixStudio/4.2");
    int code=http.GET();
    if(code<200 || code>=400){
      http.end();
      server.send(502, "application/json", R"json({"error":"Pixilart rechazo la pagina"})json");
      return;
    }

    String html=http.getString();
    http.end();
    int marker = html.indexOf("property=\"og:image\"");
    if(marker<0) marker=html.indexOf("property='og:image'");
    if(marker<0){
      server.send(404, "application/json", R"json({"error":"No encontre metadato og:image en esa obra"})json");
      return;
    }

    int contentPos=html.indexOf("content=",marker);
    if(contentPos<0){
      server.send(404, "application/json", R"json({"error":"No encontre la imagen publica"})json");
      return;
    }

    int q1=html.indexOf('"',contentPos+8);
    char quote='"';
    if(q1<0 || q1-contentPos>4){
      q1 = html.indexOf('\'', contentPos + 8);
      quote = '\'';
    }
    int q2=html.indexOf(quote,q1+1);
    if(q1<0 || q2<0){
      server.send(404, "application/json", R"json({"error":"No pude leer og:image"})json");
      return;
    }

    String imageUrl=html.substring(q1+1,q2);
    imageUrl.replace("&amp;","&");

    String ext=".jpg";
    String lower=imageUrl; lower.toLowerCase();
    if(lower.indexOf(".png")>=0)ext=".png";
    else if(lower.indexOf(".webp")>=0)ext=".webp";

    String filename="pixilart_"+String((uint32_t)millis())+ext;
    String localPath=String(REMOTE_DIR)+"/"+filename;

    WiFiClientSecure imageClient;
    imageClient.setInsecure();
    HTTPClient imageHttp;

    if(!imageHttp.begin(imageClient,imageUrl)){
      server.send(502, "application/json", R"json({"error":"No pude abrir la imagen de Pixilart"})json");
      return;
    }

    imageHttp.setUserAgent("Mozilla/5.0 MatrixStudio/4.2");
    int imageCode=imageHttp.GET();
    if(imageCode<200 || imageCode>=400){
      imageHttp.end();
      server.send(502, "application/json", R"json({"error":"No pude descargar la imagen"})json");
      return;
    }

    if(SD_MMC.exists(localPath))SD_MMC.remove(localPath);
    File out=SD_MMC.open(localPath,FILE_WRITE);
    if(!out){
      imageHttp.end();
      server.send(500, "application/json", R"json({"error":"No pude guardar en microSD"})json");
      return;
    }

    WiFiClient* stream=imageHttp.getStreamPtr();
    uint8_t buf[1024];
    int remaining=imageHttp.getSize();

    while(imageHttp.connected() && (remaining>0 || remaining==-1)){
      size_t available=stream->available();
      if(available){
        int n=stream->readBytes(buf,min((size_t)sizeof(buf),available));
        if(n>0){out.write(buf,n);if(remaining>0)remaining-=n;}
      }else{
        delay(1);
      }
      
    }

    out.close();
    imageHttp.end();

    String response = String(R"json({"ok":true,"localUrl":"/remote?name=)json") + filename + R"json("})json";
    server.send(200,"application/json",response);
  });

  server.on("/api/status",HTTP_GET,[](){
    uint64_t total = SD_MMC.totalBytes();
    uint64_t used  = SD_MMC.usedBytes();
    String j = "{";
    j += "\"host\":\"" + String(MDNS_HOST) + ".local\",";
    j += "\"city\":\"" + jsonEsc(String(location.name)) + "\",";
    j += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
    j += "\"ssid\":\"" + jsonEsc(WiFi.SSID()) + "\",";
    j += "\"mac\":\"" + WiFi.macAddress() + "\",";
    j += "\"rssi\":" + String(WiFi.RSSI()) + ",";
    j += "\"sdTotalMB\":" + String((uint32_t)(total / 1048576ULL)) + ",";
    j += "\"sdUsedMB\":" + String((uint32_t)(used / 1048576ULL)) + ",";
    j += "\"lastRenderMs\":" + String(lastRenderMs) + ",";
    j += "\"clockEnabled\":" + String(clockCfg.enabled ? "true" : "false") + ",";
    j += "\"uptimeS\":" + String((uint32_t)(millis() / 1000)) + ",";
    j += "\"resetReason\":\"" + String(resetReasonText()) + "\",";
    j += "\"heapFree\":" + String(ESP.getFreeHeap()) + ",";
    j += "\"heapMin\":" + String(ESP.getMinFreeHeap()) + ",";
    j += "\"heapSize\":" + String(ESP.getHeapSize()) + ",";
    j += "\"psramFree\":" + String(ESP.getFreePsram()) + ",";
    j += "\"psramSize\":" + String(ESP.getPsramSize()) + ",";
    j += "\"chip\":\"" + String(ESP.getChipModel()) + " rev " + String(ESP.getChipRevision()) + "\",";
    j += "\"cpuMHz\":" + String(ESP.getCpuFreqMHz()) + ",";
    j += "\"sketchSize\":" + String(ESP.getSketchSize()) + ",";
    j += "\"sketchFree\":" + String(ESP.getFreeSketchSpace()) + ",";
    j += "\"sdk\":\"" + jsonEsc(String(ESP.getSdkVersion())) + "\",";
    j += "\"webVersion\":\"" + String(WEB_ASSET_VERSION) + "\",";
    j += "\"build\":\"" __DATE__ " " __TIME__ "\",";
    j += "\"stateKind\":\"" + stateKind + "\",";
    j += "\"stateAnim\":\"" + jsonEsc(stateAnim) + "\",";
    j += "\"animPlaying\":" + String(animationPlaying ? "true" : "false") + ",";
    j += "\"animFrames\":" + String(animFrames) + ",";
    j += "\"brightness\":" + String(clockCfg.brightness) + ",";
    j += "\"weatherValid\":" + String(weatherValid ? "true" : "false") + ",";
    j += "\"temp\":" + String(weatherTempC, 1) + ",";
    j += "\"humidity\":" + String(weatherHumidityPct, 0);
    j += "}";
    server.send(200,"application/json",j);
  });

  // Serial monitor for the web: lines with seq >= since. "next" is the seq to
  // ask for next time; if it is lower than the client's since, the ESP rebooted.
  server.on("/api/log",HTTP_GET,[](){
    static LogLine buf[LOG_LINES];
    uint32_t since = (uint32_t)server.arg("since").toInt();
    int n = Log.snapshot(since, buf, LOG_LINES);
    String j = "{\"next\":" + String(Log.nextSeq()) + ",\"lines\":[";
    for (int i = 0; i < n; i++) {
      if (i) j += ",";
      j += "{\"s\":" + String(buf[i].seq) + ",\"t\":" + String(buf[i].ms) + ",\"m\":\"" + jsonEsc(String(buf[i].text)) + "\"}";
    }
    j += "]}";
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json",j);
  });

  server.onNotFound([](){server.send(404,"text/plain","404");});
  server.begin();
}

// =========================
// Setup
// =========================
void setupDisplay(){
  display.begin(32,P_CLK,P_MOSI,P_MISO,P_SS);
  display.setMuxPattern(BINARY);
  display.setScanPattern(LINE);

  // FastUpdate puede ser sensible al timing en algunos paneles.
  // Priorizamos estabilidad y dejamos el refresh al timer hardware.
  display.setFastUpdate(false);

  display.setBrightness(20);
  display.clearDisplay(false);
  display.clearDisplay(true);

  startMatrixRefreshTask();
}
void setupSD(){
  if(!SD_MMC.begin("/sdcard",true)){while(true){delay(1000);}}
  provisionWeb();
}
void setup(){
  Serial.begin(115200);
  delay(300);

  Log.println();
  Log.println("=== Matrix Studio boot ===");

  Log.println("[1/5] Inicializando display...");
  setupDisplay();
  Log.println("[1/5] Display OK");

  Log.println("[2/5] Montando microSD...");
  setupSD();
  Log.println("[2/5] microSD OK");

  faceDefaults(faceCfg);
  loadLocation();
  applyUtcOffset(location.utcOffset);
  Log.println("Ubicacion: " + String(location.name));

  Log.println("Restaurando ultimo estado del panel...");
  restoreState();

  Log.println("[3/5] Conectando Wi-Fi...");
  connectWiFi();
  Log.print("[3/5] Wi-Fi OK: ");
  Log.println(WiFi.localIP());

  Log.println("[4/5] Iniciando servidor HTTP...");
  setupServer();
  Log.println("[4/5] Servidor HTTP OK");

  Log.println("[5/5] Iniciando tarea de clima...");
  BaseType_t weatherResult = xTaskCreatePinnedToCore(
    weatherTask,
    "weather",
    8192,
    nullptr,
    1,
    &weatherTaskHandle,
    0
  );

  if (weatherResult == pdPASS) {
    Log.println("[5/5] Clima OK");
  } else {
    weatherTaskHandle = nullptr;
    Log.println("[5/5] ADVERTENCIA: no se pudo iniciar tarea de clima");
  }

  Log.println("=== Matrix Studio listo ===");
  Log.print("mDNS: http://");
  Log.print(MDNS_HOST);
  Log.println(".local");
  Log.print("IP: http://");
  Log.println(WiFi.localIP());
}
void loop(){
  serviceAnim();
  serviceClock();
  serviceState();
  serviceClockAux();
  if(WiFi.status()==WL_CONNECTED)server.handleClient();
  serviceWiFi();
}
