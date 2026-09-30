
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

#define PxMATRIX_DOUBLE_BUFFER true
#include <PxMatrix.h>

// =========================
// WIFI
// =========================
const char* WIFI_SSID = "DarkMaster-666";
const char* WIFI_PASSWORD = "H*nnaHChan1";
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
    Serial.println("ERROR: no se pudo crear tarea de refresco HUB75");
    return false;
  }

  Serial.println("Refresh HUB75: tarea FreeRTOS cada 2 ms");
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
uint32_t lastClockDraw = 0;
const float PUEBLA_LAT = 19.0414f;
const float PUEBLA_LON = -98.2063f;

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
  <link rel="stylesheet" href="/tailwind.css?v=55">
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
  <script src="/app.js?v=55"></script>
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
  <div class="brand"><div class="eyebrow">ESP32-WROVER · HUB75 · 64×64</div><h1>Matrix Studio</h1></div>
  <div class="row-wrap"><div class="pill"><span class="dot"></span><span id="connectionText">matrix.local</span></div></div>
 </header>
 <div class="workspace">
  <aside class="sidebar">
   <button class="nav-btn active" data-page="image">Imagen</button>
   <button class="nav-btn" data-page="pixel">Pixel Art</button>
   <button class="nav-btn" data-page="text">Texto</button>
   <button class="nav-btn" data-page="clock">Modo reloj</button>
   <button class="nav-btn" data-page="library">Biblioteca</button>
   <button class="nav-btn" data-page="gallery">Galería</button>
   <button class="nav-btn" data-page="gifs">GIF's</button>
   <button class="nav-btn" data-page="admin">Admin SD</button>
   <button class="nav-btn" data-page="firmware">Firmware</button>
   <button class="nav-btn" data-page="panel">Panel</button>
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
    <div class="page-head"><div><h2>Modo reloj</h2><p>Reloj autónomo sincronizado por Internet con clima y humedad para Puebla.</p></div><span class="pill"><span class="dot"></span><span id="clockState">Inactivo</span></span></div>
    <div class="card">
     <div class="card-title">Diseño</div>
     <div class="clock-layouts" id="clockLayouts">
      <div class="clock-layout active" data-mode="0"><div class="mini">12:34</div><div class="label">Digital</div></div>
      <div class="clock-layout" data-mode="1"><div class="mini">12:34<br>21 SEP</div><div class="label">Digital + fecha</div></div>
      <div class="clock-layout" data-mode="2"><div class="mini">12:34<br>21°C<br>55%</div><div class="label">Clima</div></div>
      <div class="clock-layout" data-mode="3"><div class="mini">◯<br>10:10</div><div class="label">Analógico</div></div>
      <div class="clock-layout" data-mode="4"><div class="mini">◯ 12:34<br>21°C</div><div class="label">Híbrido</div></div>
     </div>
    </div>
    <div class="grid-2 mt-3">
     <div class="card">
      <div class="card-title">Contenido</div>
      <div class="switch-row"><span>Formato 24 horas</span><label class="switch"><input id="clock24" type="checkbox" checked><span class="switch-track"></span></label></div>
      <div class="switch-row"><span>Mostrar segundos</span><label class="switch"><input id="clockSeconds" type="checkbox"><span class="switch-track"></span></label></div>
      <div class="switch-row"><span>Mostrar fecha</span><label class="switch"><input id="clockDate" type="checkbox" checked><span class="switch-track"></span></label></div>
      <div class="switch-row"><span>Temperatura</span><label class="switch"><input id="clockTemp" type="checkbox" checked><span class="switch-track"></span></label></div>
      <div class="switch-row"><span>Humedad</span><label class="switch"><input id="clockHumidity" type="checkbox" checked><span class="switch-track"></span></label></div>
      <div class="switch-row"><span>Estado del clima</span><label class="switch"><input id="clockWeather" type="checkbox" checked><span class="switch-track"></span></label></div>
      <label class="field mt-3">Brillo del reloj<div class="quick-range mt-2"><input id="clockBrightness" type="range" min="1" max="255" value="20"><span id="clockBrightnessValue">20</span></div></label>
     </div>
     <div class="card">
      <div class="card-title">Colores</div>
      <div class="color-grid">
       <div><label class="field">Fondo</label><div class="clock-color mt-2"><input id="clockBg" type="color" value="#000000"><input class="input" id="clockBgHex" value="#000000"></div></div>
       <div><label class="field">Hora / agujas</label><div class="clock-color mt-2"><input id="clockPrimary" type="color" value="#ffffff"><input class="input" id="clockPrimaryHex" value="#ffffff"></div></div>
       <div><label class="field">Fecha / segundos</label><div class="clock-color mt-2"><input id="clockSecondary" type="color" value="#22d3ee"><input class="input" id="clockSecondaryHex" value="#22d3ee"></div></div>
       <div><label class="field">Acento</label><div class="clock-color mt-2"><input id="clockAccent" type="color" value="#ff6b35"><input class="input" id="clockAccentHex" value="#ff6b35"></div></div>
       <div><label class="field">Clima</label><div class="clock-color mt-2"><input id="clockWeatherColor" type="color" value="#7dd3fc"><input class="input" id="clockWeatherHex" value="#7dd3fc"></div></div>
      </div>
     </div>
    </div>
    <div class="grid-2 mt-3">
     <div class="card"><div class="card-title">Vista previa</div><div class="canvas-wrap"><canvas id="clockCanvas" width="64" height="64"></canvas></div></div>
     <div class="card"><div class="card-title">Puebla · datos de Internet</div><div class="clock-info"><div class="box"><div class="big" id="weatherTemp">--°C</div><div class="small">Temperatura</div></div><div class="box"><div class="big" id="weatherHumidity">--%</div><div class="small">Humedad</div></div><div class="box"><div class="big" id="weatherState">--</div><div class="small">Clima</div></div></div><div class="note mt-4">La hora del panel se sincroniza por NTP. El clima se actualiza desde Open-Meteo aproximadamente cada 15 minutos. Si Internet cae, el reloj sigue usando la última hora sincronizada y conserva el último clima recibido.</div><div class="toolbar mt-4"><button class="btn" id="refreshWeather">Actualizar clima</button><button class="btn btn-primary" id="activateClock">Activar modo reloj</button><button class="btn btn-danger" id="stopClock">Desactivar reloj</button></div></div>
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
   <section class="page" id="page-panel"><div class="page-head"><div><h2>Panel</h2><p>Estado y ajustes del sistema.</p></div></div><div class="card"><div class="stats" id="stats"></div><label class="field mt-4">Brillo<div class="quick-range mt-2"><input id="brightness" type="range" min="1" max="255" value="20"><span id="brightnessValue">20</span></div></label></div></section>
  </main>
 </div>
</div>`;

$$('.nav-btn').forEach(b=>b.onclick=()=>{$$('.nav-btn').forEach(x=>x.classList.remove('active'));$$('.page').forEach(x=>x.classList.remove('active'));b.classList.add('active');$('#page-'+b.dataset.page).classList.add('active');if(b.dataset.page==='gallery')loadGallery();if(b.dataset.page==='gifs')loadGifs();if(b.dataset.page==='admin')listFiles();if(b.dataset.page==='panel')loadStatus();if(b.dataset.page==='clock'){loadClockStatus();renderClockPreview()}});

function black(ctx){ctx.fillStyle='#000';ctx.fillRect(0,0,64,64)}
function to565(canvas){const d=canvas.getContext('2d',{willReadFrequently:true}).getImageData(0,0,64,64).data,o=new Uint8Array(8192);for(let p=0;p<4096;p++){let i=p*4,v=((d[i]&248)<<8)|((d[i+1]&252)<<3)|(d[i+2]>>3);o[p*2]=(v>>8)&255;o[p*2+1]=v&255}return o}
function uploadXHR(url,field,blob,name,title='Transfiriendo'){return new Promise((resolve,reject)=>{const fd=new FormData();fd.append(field,blob,name);const x=new XMLHttpRequest();x.open('POST',url,true);showLoader(title,'Subiendo…',5);x.upload.onprogress=e=>{if(e.lengthComputable)updateLoader(`Subiendo ${Math.round(e.loaded/1024)} / ${Math.round(e.total/1024)} KB`,Math.round(e.loaded/e.total*88)+5)};x.upload.onload=()=>updateLoader('Renderizando en el panel…',94);x.onload=()=>{hideLoader();if(x.status>=200&&x.status<300){let data;try{data=JSON.parse(x.responseText)}catch{data=x.responseText}resolve(data)}else reject(new Error(x.responseText||'Error '+x.status))};x.onerror=()=>{hideLoader();reject(new Error('Error de red'))};x.send(fd)})}
async function sendCanvas(canvas){const b=to565(canvas);const data=await uploadXHR('/api/frame','frame',new Blob([b],{type:'application/octet-stream'}),'frame.rgb565','Enviando al panel');if(data?.renderMs!==undefined)toast(`Listo · render ${data.renderMs} ms`);else toast('Enviado al panel');return b}
async function sendRawFrame(bytes){return uploadXHR('/api/frame','frame',new Blob([bytes]),'frame.rgb565','Enviando al panel')}
async function saveFrame(bytes){await sendRawFrame(bytes);const v=await modal('Guardar en microSD','<label class="field">Nombre<input class="input" name="name" value="imagen"></label>','Guardar');if(!v)return;showLoader('Guardando','Escribiendo en microSD…',70);const r=await fetch('/api/gallery/save-image?name='+encodeURIComponent(v.name),{method:'POST'});hideLoader();if(!r.ok)throw Error(await r.text());toast('Guardado en microSD')}

// BRIGHTNESS
let brightnessTimer=null;function setBrightness(v){$('#globalBrightnessValue').textContent=v;$('#brightnessValue').textContent=v;$('#clockBrightnessValue').textContent=v;$('#globalBrightness').value=v;$('#brightness').value=v;$('#clockBrightness').value=v;clearTimeout(brightnessTimer);brightnessTimer=setTimeout(()=>fetch('/api/brightness?v='+v,{method:'POST'}),100)}
['globalBrightness','brightness','clockBrightness'].forEach(id=>$('#'+id).oninput=e=>setBrightness(e.target.value));

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
let clockMode=0;$$('.clock-layout').forEach(el=>el.onclick=()=>{$$('.clock-layout').forEach(x=>x.classList.remove('active'));el.classList.add('active');clockMode=+el.dataset.mode;renderClockPreview()});
const colorPairs=[['clockBg','clockBgHex'],['clockPrimary','clockPrimaryHex'],['clockSecondary','clockSecondaryHex'],['clockAccent','clockAccentHex'],['clockWeatherColor','clockWeatherHex']];colorPairs.forEach(([c,h])=>{$('#'+c).oninput=e=>{$('#'+h).value=e.target.value;renderClockPreview()};$('#'+h).onchange=e=>{if(/^#[0-9a-f]{6}$/i.test(e.target.value)){$('#'+c).value=e.target.value;renderClockPreview()}}});
['clock24','clockSeconds','clockDate','clockTemp','clockHumidity','clockWeather'].forEach(id=>$('#'+id).onchange=renderClockPreview);
function clockCfg(){return{mode:clockMode,h24:$('#clock24').checked?1:0,seconds:$('#clockSeconds').checked?1:0,date:$('#clockDate').checked?1:0,temp:$('#clockTemp').checked?1:0,humidity:$('#clockHumidity').checked?1:0,weather:$('#clockWeather').checked?1:0,brightness:+$('#clockBrightness').value,bg:$('#clockBg').value,primary:$('#clockPrimary').value,secondary:$('#clockSecondary').value,accent:$('#clockAccent').value,weatherColor:$('#clockWeatherColor').value}}
function renderClockPreview(){const c=$('#clockCanvas'),x=c.getContext('2d'),cfg=clockCfg(),now=new Date();x.fillStyle=cfg.bg;x.fillRect(0,0,64,64);let hh=now.getHours(),ampm='';if(!cfg.h24){ampm=hh>=12?'P':'A';hh=hh%12||12}const mm=String(now.getMinutes()).padStart(2,'0'),ss=String(now.getSeconds()).padStart(2,'0'),hs=String(hh).padStart(2,'0');x.imageSmoothingEnabled=false;x.textAlign='center';if(cfg.mode===3||cfg.mode===4){x.strokeStyle=cfg.primary;x.lineWidth=1;x.beginPath();x.arc(cfg.mode===4?21:32,cfg.mode===4?25:30,cfg.mode===4?18:26,0,Math.PI*2);x.stroke();const cx=cfg.mode===4?21:32,cy=cfg.mode===4?25:30,r=cfg.mode===4?18:26;const ma=(now.getMinutes()/60)*Math.PI*2-Math.PI/2,ha=((now.getHours()%12+now.getMinutes()/60)/12)*Math.PI*2-Math.PI/2;x.strokeStyle=cfg.accent;x.beginPath();x.moveTo(cx,cy);x.lineTo(cx+Math.cos(ma)*r*.75,cy+Math.sin(ma)*r*.75);x.stroke();x.strokeStyle=cfg.primary;x.beginPath();x.moveTo(cx,cy);x.lineTo(cx+Math.cos(ha)*r*.5,cy+Math.sin(ha)*r*.5);x.stroke();if(cfg.mode===4){x.fillStyle=cfg.primary;x.font='bold 9px monospace';x.fillText(`${hs}:${mm}`,48,18);x.fillStyle=cfg.weatherColor;x.font='8px monospace';if(cfg.temp)x.fillText($('#weatherTemp').textContent,47,32);if(cfg.humidity)x.fillText($('#weatherHumidity').textContent,47,43)}}else{x.fillStyle=cfg.primary;x.font='bold 16px monospace';x.fillText(`${hs}:${mm}`,32,cfg.mode===0?30:22);if(!cfg.h24){x.font='6px monospace';x.fillText(ampm,58,cfg.mode===0?30:22)}if(cfg.seconds){x.fillStyle=cfg.secondary;x.font='8px monospace';x.fillText(ss,32,cfg.mode===0?42:34)}if((cfg.mode===1||cfg.date)&&cfg.date){x.fillStyle=cfg.secondary;x.font='7px monospace';x.fillText(now.toLocaleDateString('es-MX',{day:'2-digit',month:'short'}).toUpperCase().replace('.',''),32,46)}if(cfg.mode===2){x.fillStyle=cfg.weatherColor;x.font='8px monospace';if(cfg.temp)x.fillText($('#weatherTemp').textContent,18,38);if(cfg.humidity)x.fillText($('#weatherHumidity').textContent,47,38);if(cfg.weather){x.fillStyle=cfg.accent;x.font='7px monospace';x.fillText($('#weatherState').textContent,32,53)}}}}
setInterval(()=>{if($('#page-clock').classList.contains('active'))renderClockPreview()},1000);
async function loadClockStatus(){try{const s=await (await fetch('/api/clock/status')).json();$('#clockState').textContent=s.enabled?'Activo':'Inactivo';$('#weatherTemp').textContent=s.weatherValid?`${Math.round(s.temp)}°C`:'--°C';$('#weatherHumidity').textContent=s.weatherValid?`${Math.round(s.humidity)}%`:'--%';$('#weatherState').textContent=s.weatherText||'--';if(s.brightness){setBrightness(s.brightness)}renderClockPreview()}catch(e){toast('No pude leer estado del reloj','err')}}
$('#refreshWeather').onclick=async()=>{showLoader('Actualizando clima','Consultando Open-Meteo…',35);const r=await fetch('/api/clock/weather',{method:'POST'});hideLoader();if(r.ok){toast('Actualización de clima solicitada');setTimeout(loadClockStatus,1500)}else toast(await r.text(),'err')};
$('#activateClock').onclick=async()=>{const cfg=clockCfg(),body=new URLSearchParams(Object.entries(cfg));showLoader('Activando reloj','Guardando configuración…',55);const r=await fetch('/api/clock/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});hideLoader();if(!r.ok)return toast(await r.text(),'err');$('#clockState').textContent='Activo';toast('Modo reloj activado');setBrightness(cfg.brightness)};
$('#stopClock').onclick=async()=>{await fetch('/api/clock/stop',{method:'POST'});$('#clockState').textContent='Inactivo';toast('Modo reloj desactivado')};

// LIBRARY
const presets=[{name:'Corazón',draw:c=>{black(c);c.fillStyle='#ef3340';[[3,1],[4,1],[2,2],[3,2],[4,2],[5,2],[1,3],[2,3],[3,3],[4,3],[5,3],[6,3],[2,4],[3,4],[4,4],[5,4],[3,5],[4,5]].forEach(([x,y])=>c.fillRect(x*8,y*8,8,8))}},{name:'Carita',draw:c=>{black(c);c.fillStyle='#ffd84d';c.fillRect(8,8,48,48);c.fillStyle='#111';c.fillRect(20,22,6,6);c.fillRect(38,22,6,6);c.fillRect(20,42,24,5);c.fillRect(16,37,5,5);c.fillRect(43,37,5,5)}},{name:'Estrella',draw:c=>{black(c);c.fillStyle='#ffe347';const pts=[[32,5],[39,24],[59,24],[43,36],[49,57],[32,44],[15,57],[21,36],[5,24],[25,24]];c.beginPath();pts.forEach((p,i)=>i?c.lineTo(...p):c.moveTo(...p));c.closePath();c.fill()}},{name:'Nebulosa',draw:c=>{black(c);for(let y=0;y<64;y+=4)for(let x=0;x<64;x+=4){c.fillStyle=`hsl(${(x*4+y*2)%280+190} 85% ${25+((x+y)%20)}%)`;c.fillRect(x,y,4,4)};c.fillStyle='#fff';[[8,9],[49,12],[22,44],[57,51],[35,26]].forEach(p=>c.fillRect(...p,2,2))}},{name:'Flor',draw:c=>{black(c);c.fillStyle='#22c55e';c.fillRect(30,30,4,30);c.fillStyle='#ff78c6';[[30,14],[22,22],[38,22],[22,30],[38,30]].forEach(([x,y])=>c.fillRect(x,y,8,8));c.fillStyle='#ffd84d';c.fillRect(30,22,8,8)}},{name:'Robot',draw:c=>{black(c);c.fillStyle='#5ee7f7';c.fillRect(12,14,40,36);c.fillStyle='#07101d';c.fillRect(20,24,8,8);c.fillRect(36,24,8,8);c.fillRect(20,39,24,4);c.fillStyle='#ff6b35';c.fillRect(29,7,6,7)}}];
function loadPreset(p){const c=document.createElement('canvas');c.width=64;c.height=64;p.draw(c.getContext('2d'));px.putImageData(c.getContext('2d').getImageData(0,0,64,64),0,0);$$('.nav-btn').find(b=>b.dataset.page==='pixel').click();toast(`${p.name} cargado en Pixel Art`)}const lib=$('#builtInLibrary');presets.forEach(p=>{const d=document.createElement('div');d.className='library-card';const c=document.createElement('canvas');c.className='library-preview';c.width=64;c.height=64;p.draw(c.getContext('2d'));d.innerHTML=`<div class="font-bold mt-2">${p.name}</div><div class="muted text-xs mt-1">64×64 · original local</div>`;d.prepend(c);d.onclick=()=>loadPreset(p);lib.appendChild(d)});
$('#importPixilart').onclick=async()=>{const url=$('#pixilartUrl').value.trim();if(!url)return toast('Pega una URL pública de Pixilart','err');showLoader('Importando Pixilart','Resolviendo metadatos públicos…',12);try{const r=await fetch('/api/pixilart/import?url='+encodeURIComponent(url),{method:'POST'});const data=await r.json().catch(()=>({}));if(!r.ok)throw Error(data.error||'No se pudo importar');updateLoader('Cargando imagen importada…',78);const im=new Image();im.crossOrigin='anonymous';im.onload=()=>{img=im;drawImage();hideLoader();$$('.nav-btn').find(b=>b.dataset.page==='image').click();toast('Obra importada a Imagen')};im.onerror=()=>{hideLoader();toast('Se guardó pero no se pudo previsualizar','err')};im.src=data.localUrl+'?t='+Date.now()}catch(e){hideLoader();toast(e.message,'err')}};

// GALLERY + ADMIN + OTA
async function loadGallery(){const box=$('#galleryList');box.innerHTML='<div class="muted">Cargando…</div>';const r=await fetch('/api/gallery');const d=await r.json();box.innerHTML='';const all=[...(d.images||[]).map(x=>({...x,type:'image'})),...(d.animations||[]).map(x=>({...x,type:'animation'}))];if(!all.length){box.innerHTML='<div class="muted">Sin contenido.</div>';return}for(const it of all){const e=document.createElement('div');e.className='library-card';e.innerHTML=`<div class="font-bold">${it.name}</div><div class="muted text-xs mt-1">${it.type} · ${it.size} B</div><div class="toolbar mt-3"><button class="btn play">${it.type==='image'?'Mostrar':'Reproducir'}</button><button class="btn btn-danger del">Eliminar</button></div>`;e.querySelector('.play').onclick=()=>fetch((it.type==='image'?'/api/gallery/show-image?name=':'/api/gallery/play-animation?name=')+encodeURIComponent(it.name),{method:'POST'});e.querySelector('.del').onclick=async()=>{const v=await modal('Eliminar',`¿Eliminar <b>${it.name}</b>?`,'Eliminar');if(v!==null){await fetch('/api/gallery/delete?type='+it.type+'&name='+encodeURIComponent(it.name),{method:'DELETE'});loadGallery()}};box.appendChild(e)}}$('#reloadGallery').onclick=loadGallery;
$('#adminUpload').onchange=e=>$('#adminFileName').textContent=[...e.target.files].map(f=>f.name).join(', ')||'Ningún archivo';async function listFiles(){const path=$('#adminPath').value||'/';const r=await fetch('/api/fs/list?path='+encodeURIComponent(path));const d=await r.json();let h='<table class="table"><tr><th>Nombre</th><th>Tipo</th><th>Tamaño</th><th></th></tr>';for(const it of d.items)h+=`<tr><td>${it.name}</td><td>${it.dir?'carpeta':'archivo'}</td><td>${it.dir?'':it.size}</td><td><button class="btn btn-danger fdel" data-path="${it.path}">Eliminar</button></td></tr>`;h+='</table>';$('#fileList').innerHTML=h;$$('.fdel').forEach(b=>b.onclick=async()=>{const v=await modal('Eliminar',`¿Eliminar <b>${b.dataset.path}</b>?`,'Eliminar');if(v!==null){await fetch('/api/fs/delete?path='+encodeURIComponent(b.dataset.path),{method:'DELETE'});listFiles()}})}$('#listFiles').onclick=listFiles;$('#mkdir').onclick=async()=>{const v=await modal('Crear carpeta','<label class="field">Nombre<input class="input" name="name"></label>','Crear');if(!v)return;const base=$('#adminPath').value.replace(/\/$/,'');await fetch('/api/fs/mkdir?path='+encodeURIComponent(base+'/'+v.name),{method:'POST'});listFiles()};$('#uploadFiles').onclick=async()=>{const files=[...$('#adminUpload').files];if(!files.length)return toast('Selecciona archivos','err');const base=$('#adminPath').value.replace(/\/$/,'');showLoader('Subiendo archivos','Preparando…',5);try{let i=0;for(const f of files){i++;await uploadXHR('/api/fs/upload?path='+encodeURIComponent(base+'/'+f.name),'file',f,f.name,`Subiendo ${i}/${files.length}`)}hideLoader();toast('Archivos subidos');listFiles()}catch(e){hideLoader();toast(e.message,'err')}};
$('#fwFile').onchange=e=>$('#fwFileName').textContent=e.target.files[0]?.name||'Ningún firmware';$('#fwUpload').onclick=async()=>{const f=$('#fwFile').files[0];if(!f)return toast('Selecciona un .bin','err');const v=await modal('Actualizar firmware',`Se instalará <b>${f.name}</b> y el ESP32 reiniciará. No desconectes alimentación.`,'Instalar');if(v===null)return;try{await uploadXHR('/api/firmware','firmware',f,f.name,'Actualizando firmware');toast('Firmware instalado; reiniciando…');setTimeout(()=>location.href='http://matrix.local',9000)}catch(e){toast(e.message,'err')}};
async function loadStatus(){try{const s=await (await fetch('/api/status')).json();$('#connectionText').textContent=`${s.ip} · ${s.rssi??'?'} dBm`;$('#stats').innerHTML=`<div class="stat"><div class="v">${s.ip}</div><div class="k">IP</div></div><div class="stat"><div class="v">${s.rssi??'?'} dBm</div><div class="k">Wi-Fi</div></div><div class="stat"><div class="v">${s.sdUsedMB??'?'} / ${s.sdTotalMB??'?'} MB</div><div class="k">microSD</div></div><div class="stat"><div class="v">${s.lastRenderMs??'?'} ms</div><div class="k">Último render</div></div>`}catch{$('#connectionText').textContent='sin conexión'}}loadStatus();setInterval(loadStatus,10000);loadClockStatus();

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

const char* WEB_ASSET_VERSION = "5.5";

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
    Serial.println("Web assets OK: v" + installedVersion);
    return;
  }

  Serial.println("Actualizando web assets en microSD a v" + String(WEB_ASSET_VERSION));

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
    Serial.println("Web assets actualizados correctamente.");
  } else {
    Serial.println("ERROR actualizando web assets.");
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
  if(!animationPlaying||!animFile||millis()<nextAnimAt)return;
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

void fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String url = "https://api.open-meteo.com/v1/forecast?latitude=19.0414&longitude=-98.2063&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code&timezone=America%2FMexico_City";
  if (!http.begin(client, url)) return;
  http.setConnectTimeout(5000);
  http.setTimeout(7000);
  http.setUserAgent("MatrixStudioClock/5.0");
  int code = http.GET();
  if (code >= 200 && code < 300) {
    String body = http.getString();
    float t = jsonNumber(body, "temperature_2m", weatherTempC);
    float h = jsonNumber(body, "relative_humidity_2m", weatherHumidityPct);
    float f = jsonNumber(body, "apparent_temperature", weatherFeelsC);
    int wc = (int)jsonNumber(body, "weather_code", weatherCode);
    weatherTempC = t;
    weatherHumidityPct = h;
    weatherFeelsC = f;
    weatherCode = wc;
    weatherValid = true;
  }
  http.end();
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

void drawCenteredText(const String& value, int y, uint8_t size, uint16_t color) {
  int width = value.length() * 6 * size;
  int x = (64 - width) / 2;
  if (x < 0) x = 0;
  display.setTextSize(size);
  display.setTextColor(color);
  display.setCursor(x, y);
  display.print(value);
}

void drawWeatherIcon(int x, int y, int code, uint16_t color) {
  if (code == 0) {
    display.drawCircle(x, y, 4, color);
    display.drawLine(x, y - 8, x, y - 6, color);
    display.drawLine(x, y + 6, x, y + 8, color);
    display.drawLine(x - 8, y, x - 6, y, color);
    display.drawLine(x + 6, y, x + 8, y, color);
    return;
  }
  display.fillCircle(x - 3, y, 4, color);
  display.fillCircle(x + 2, y - 2, 5, color);
  display.fillRect(x - 7, y, 14, 4, color);
  if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82) || code >= 95) {
    display.drawPixel(x - 4, y + 7, color);
    display.drawPixel(x, y + 9, color);
    display.drawPixel(x + 4, y + 7, color);
  }
}

String dateShort(struct tm& ti) {
  const char* months[] = {"ENE","FEB","MAR","ABR","MAY","JUN","JUL","AGO","SEP","OCT","NOV","DIC"};
  char buf[16];
  snprintf(buf, sizeof(buf), "%02d %s", ti.tm_mday, months[ti.tm_mon]);
  return String(buf);
}

void renderClock() {
  if (!clockCfg.enabled) return;
  struct tm ti;
  if (!getLocalTime(&ti, 50)) return;

  display.clearDisplay();
  display.fillScreen(clockCfg.bg);

  int hour = ti.tm_hour;
  const char* suffix = "";
  if (!clockCfg.hour24) {
    suffix = hour >= 12 ? "P" : "A";
    hour %= 12;
    if (hour == 0) hour = 12;
  }

  char timeBuf[12];
  snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", hour, ti.tm_min);
  String timeText(timeBuf);

  if (clockCfg.mode == 3 || clockCfg.mode == 4) {
    int cx = clockCfg.mode == 4 ? 20 : 32;
    int cy = clockCfg.mode == 4 ? 24 : 30;
    int radius = clockCfg.mode == 4 ? 18 : 27;
    display.drawCircle(cx, cy, radius, clockCfg.primary);
    for (int i = 0; i < 12; i++) {
      float a = (i / 12.0f) * 2.0f * PI - PI / 2.0f;
      int px = cx + cosf(a) * (radius - 3);
      int py = cy + sinf(a) * (radius - 3);
      display.drawPixel(px, py, clockCfg.secondary);
    }
    float minuteAngle = (ti.tm_min / 60.0f) * 2.0f * PI - PI / 2.0f;
    float hourAngle = ((ti.tm_hour % 12 + ti.tm_min / 60.0f) / 12.0f) * 2.0f * PI - PI / 2.0f;
    display.drawLine(cx, cy, cx + cosf(hourAngle) * radius * 0.50f, cy + sinf(hourAngle) * radius * 0.50f, clockCfg.primary);
    display.drawLine(cx, cy, cx + cosf(minuteAngle) * radius * 0.76f, cy + sinf(minuteAngle) * radius * 0.76f, clockCfg.accent);
    if (clockCfg.showSeconds) {
      float secAngle = (ti.tm_sec / 60.0f) * 2.0f * PI - PI / 2.0f;
      display.drawLine(cx, cy, cx + cosf(secAngle) * radius * 0.82f, cy + sinf(secAngle) * radius * 0.82f, clockCfg.secondary);
    }
    display.fillCircle(cx, cy, 1, clockCfg.accent);

    if (clockCfg.mode == 3) {
      if (clockCfg.showDate) drawCenteredText(dateShort(ti), 57, 1, clockCfg.secondary);
    } else {
      display.setTextSize(1);
      display.setTextColor(clockCfg.primary);
      display.setCursor(40, 8);
      display.print(timeText);
      if (!clockCfg.hour24) { display.setCursor(57, 16); display.print(suffix); }
      if (clockCfg.showTemp && weatherValid) {
        display.setTextColor(clockCfg.weatherColor);
        display.setCursor(39, 25);
        display.printf("%dC", (int)roundf(weatherTempC));
      }
      if (clockCfg.showHumidity && weatherValid) {
        display.setCursor(39, 35);
        display.printf("%d%%", (int)roundf(weatherHumidityPct));
      }
      if (clockCfg.showWeather && weatherValid) {
        display.setTextColor(clockCfg.accent);
        display.setCursor(39, 45);
        display.print(weatherShort(weatherCode));
      }
      if (clockCfg.showDate) {
        display.setTextColor(clockCfg.secondary);
        display.setCursor(35, 56);
        display.print(dateShort(ti));
      }
    }
  } else {
    int timeY = (clockCfg.mode == 0 && !clockCfg.showDate) ? 20 : 8;
    drawCenteredText(timeText, timeY, 2, clockCfg.primary);
    if (!clockCfg.hour24) {
      display.setTextSize(1);
      display.setTextColor(clockCfg.secondary);
      display.setCursor(56, timeY + 14);
      display.print(suffix);
    }
    int y = timeY + 19;
    if (clockCfg.showSeconds) {
      char secBuf[8]; snprintf(secBuf, sizeof(secBuf), ":%02d", ti.tm_sec);
      drawCenteredText(String(secBuf), y, 1, clockCfg.secondary);
      y += 10;
    }
    if ((clockCfg.mode == 1 || clockCfg.mode == 2 || clockCfg.showDate) && clockCfg.showDate) {
      drawCenteredText(dateShort(ti), y, 1, clockCfg.secondary);
      y += 10;
    }
    if (clockCfg.mode == 2 && weatherValid) {
      display.setTextSize(1);
      display.setTextColor(clockCfg.weatherColor);
      int wx = 2;
      if (clockCfg.showTemp) { display.setCursor(wx, 53); display.printf("%dC", (int)roundf(weatherTempC)); wx += 23; }
      if (clockCfg.showHumidity) { display.setCursor(wx, 53); display.printf("%d%%", (int)roundf(weatherHumidityPct)); }
      if (clockCfg.showWeather) drawWeatherIcon(55, 53, weatherCode, clockCfg.accent);
    }
  }

  display.showBuffer();
}

void serviceClock() {
  if (!clockCfg.enabled) return;
  uint32_t interval = clockCfg.showSeconds ? 200UL : 1000UL;
  if (millis() - lastClockDraw < interval) return;
  lastClockDraw = millis();
  renderClock();
}

void stopClock() {
  clockCfg.enabled = false;
}

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
    Serial.println("Estado guardado: " + (stateKind.length() ? stateKind : String("(vacio)")));
  } else {
    Serial.println("ERROR guardando estado en microSD");
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
  if (!j.length()) { Serial.println("Sin estado guardado: pantalla inicial"); showSplash(); return; }

  uint8_t b = (uint8_t)constrain((int)jsonNumber(j, "brightness", 20), 1, 255);
  clockCfg.brightness = b;
  display.setBrightness(b);

  String kind = jsonString(j, "kind");
  if (kind == "frame") {
    File ff = openWithFallback(LAST_FRAME_PATH, LAST_FRAME_TMP);
    if (ff && ff.size() == FRAME_BYTES && ff.read(frameBuffer, FRAME_BYTES) == FRAME_BYTES) {
      ff.close(); applyFrame(); stateKind = kind;
      Serial.println("Estado restaurado: frame"); return;
    }
    if (ff) ff.close();
  } else if (kind == "anim") {
    String p = jsonString(j, "anim");
    if (p.length() && playAnim(p)) {
      stateKind = kind; stateAnim = p;
      Serial.println("Estado restaurado: animacion " + p); return;
    }
  } else if (kind == "clock") {
    clockCfg.mode = constrain((int)jsonNumber(j, "cMode", 0), 0, 4);
    clockCfg.hour24 = jsonNumber(j, "cH24", 1) != 0;
    clockCfg.showSeconds = jsonNumber(j, "cSeconds", 0) != 0;
    clockCfg.showDate = jsonNumber(j, "cDate", 1) != 0;
    clockCfg.showTemp = jsonNumber(j, "cTemp", 1) != 0;
    clockCfg.showHumidity = jsonNumber(j, "cHumidity", 1) != 0;
    clockCfg.showWeather = jsonNumber(j, "cWeather", 1) != 0;
    clockCfg.bg = (uint16_t)jsonNumber(j, "cBg", clockCfg.bg);
    clockCfg.primary = (uint16_t)jsonNumber(j, "cPrimary", clockCfg.primary);
    clockCfg.secondary = (uint16_t)jsonNumber(j, "cSecondary", clockCfg.secondary);
    clockCfg.accent = (uint16_t)jsonNumber(j, "cAccent", clockCfg.accent);
    clockCfg.weatherColor = (uint16_t)jsonNumber(j, "cWeatherColor", clockCfg.weatherColor);
    stopAnim();
    clockCfg.enabled = true;
    forceWeatherRefresh = true;
    lastClockDraw = 0;
    renderClock();
    stateKind = kind;
    Serial.println("Estado restaurado: reloj"); return;
  }
  Serial.println("Estado '" + kind + "' no restaurable: pantalla inicial");
  showSplash();
}

// =========================
// WiFi / mDNS
// =========================
void startMDNS(){if(mdnsReady){MDNS.end();mdnsReady=false;}if(MDNS.begin(MDNS_HOST)){MDNS.addService("http","tcp",80);mdnsReady=true;}}
void connectWiFi(){
  WiFi.mode(WIFI_STA); WiFi.setSleep(false); WiFi.begin(WIFI_SSID,WIFI_PASSWORD);
  // Keep a restored animation/clock running while waiting for the network.
  while(WiFi.status()!=WL_CONNECTED){serviceAnim();serviceClock();delay(10);}
  configTime(-21600, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  startMDNS();
}
void serviceWiFi(){
  if(millis()-lastWifiCheck<5000)return;lastWifiCheck=millis();
  if(WiFi.status()==WL_CONNECTED){if(!mdnsReady)startMDNS();return;}
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
    
  }else if(u.status==UPLOAD_FILE_END){if(uploadFile)uploadFile.close();}
  else if(u.status==UPLOAD_FILE_ABORTED){if(uploadFile)uploadFile.close();uploadOK=false;}
}

void firmwareUpload(){
  HTTPUpload& u=server.upload();
  if(u.status==UPLOAD_FILE_START){
    otaSuccess=false; otaError="";
    stopAnim();
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

  server.on("/api/frame",HTTP_POST,[]{if(uploadOK&&uploadBytes==FRAME_BYTES){applyFrame();markState("frame");String j="{\"ok\":true,\"renderMs\":"+String(lastRenderMs)+"}";server.send(200,"application/json",j);}else server.send(400,"text/plain","Frame invalido");},frameUpload);


  server.on("/api/brightness", HTTP_POST, [](){
    int value = constrain(server.arg("v").toInt(), 1, 255);
    display.setBrightness((uint8_t)value);
    clockCfg.brightness = value;
    markStateDirty();
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/clock/config", HTTP_POST, [](){
    stopAnim();
    clockCfg.mode = constrain(server.arg("mode").toInt(), 0, 4);
    clockCfg.hour24 = server.arg("h24") == "1";
    clockCfg.showSeconds = server.arg("seconds") == "1";
    clockCfg.showDate = server.arg("date") == "1";
    clockCfg.showTemp = server.arg("temp") == "1";
    clockCfg.showHumidity = server.arg("humidity") == "1";
    clockCfg.showWeather = server.arg("weather") == "1";
    clockCfg.brightness = constrain(server.arg("brightness").toInt(), 1, 255);
    clockCfg.bg = parseHex565(server.arg("bg"));
    clockCfg.primary = parseHex565(server.arg("primary"));
    clockCfg.secondary = parseHex565(server.arg("secondary"));
    clockCfg.accent = parseHex565(server.arg("accent"));
    clockCfg.weatherColor = parseHex565(server.arg("weatherColor"));
    display.setBrightness(clockCfg.brightness);
    clockCfg.enabled = true;
    forceWeatherRefresh = true;
    lastClockDraw = 0;
    renderClock();
    markState("clock");
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/clock/stop", HTTP_POST, [](){
    stopClock();
    markState("idle");
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/clock/weather", HTTP_POST, [](){
    forceWeatherRefresh = true;
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/clock/status", HTTP_GET, [](){
    String j = "{";
    j += "\"enabled\":" + String(clockCfg.enabled ? "true" : "false") + ",";
    j += "\"mode\":" + String(clockCfg.mode) + ",";
    j += "\"brightness\":" + String(clockCfg.brightness) + ",";
    j += "\"weatherValid\":" + String(weatherValid ? "true" : "false") + ",";
    j += "\"temp\":" + String(weatherTempC, 1) + ",";
    j += "\"humidity\":" + String(weatherHumidityPct, 0) + ",";
    j += "\"feels\":" + String(weatherFeelsC, 1) + ",";
    j += "\"weatherCode\":" + String(weatherCode) + ",";
    j += "\"weatherText\":\"" + String(weatherShort(weatherCode)) + "\"";
    j += "}";
    server.send(200, "application/json", j);
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
    f.read(frameBuffer,FRAME_BYTES);f.close();stopClock();stopAnim();applyFrame();markState("frame");server.send(200,"application/json","{\"ok\":true}");
  });
  server.on("/api/gallery/play-animation",HTTP_POST,[]{
    String p=String(ANIM_DIR)+"/"+safeName(server.arg("name"));if(!playAnim(p))server.send(400,"text/plain","Animacion invalida");else{markState("anim",p);server.send(200,"application/json","{\"ok\":true}");}
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
    if(!playAnim(p))server.send(400,"text/plain","GIF invalido");else{markState("anim",p);server.send(200,"application/json","{\"ok\":true}");}
  });
  server.on("/api/gifs/delete",HTTP_DELETE,[]{
    String base=safeName(server.arg("name"));stopAnim();
    if(stateKind=="anim"&&stateAnim==String(GIF_DIR)+"/"+base+".pma")markState("idle");
    SD_MMC.remove(String(GIF_DIR)+"/"+base+".gif");
    SD_MMC.remove(String(GIF_DIR)+"/"+base+".pma");
    server.send(200,"application/json","{\"ok\":true}");
  });

  server.on("/api/fs/list",HTTP_GET,[]{server.send(200,"application/json",listJson(safePath(server.arg("path"))));});
  server.on("/api/fs/mkdir",HTTP_POST,[]{String p=safePath(server.arg("path"));if(SD_MMC.mkdir(p))server.send(200,"application/json","{\"ok\":true}");else server.send(500,"text/plain","No se pudo crear");});
  server.on("/api/fs/delete",HTTP_DELETE,[]{String p=safePath(server.arg("path"));bool ok=SD_MMC.remove(p)||SD_MMC.rmdir(p);if(ok)server.send(200,"application/json","{\"ok\":true}");else server.send(500,"text/plain","No se pudo eliminar");});
  server.on("/api/fs/upload",HTTP_POST,[]{if(uploadOK)server.send(200,"application/json","{\"ok\":true}");else server.send(500,"text/plain","Upload fallido");},genericUpload);

  server.on("/api/firmware",HTTP_POST,[]{
    if(otaSuccess){server.send(200,"text/plain","Firmware instalado. Reiniciando...");delay(500);ESP.restart();}
    else server.send(500,"text/plain",otaError.length()?otaError:"Error OTA");
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
    j += "\"host\":\"matrix.local\",";
    j += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
    j += "\"rssi\":" + String(WiFi.RSSI()) + ",";
    j += "\"sdTotalMB\":" + String((uint32_t)(total / 1048576ULL)) + ",";
    j += "\"sdUsedMB\":" + String((uint32_t)(used / 1048576ULL)) + ",";
    j += "\"lastRenderMs\":" + String(lastRenderMs) + ",";
    j += "\"clockEnabled\":" + String(clockCfg.enabled ? "true" : "false");
    j += "}";
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

  Serial.println();
  Serial.println("=== Matrix Studio boot ===");

  Serial.println("[1/5] Inicializando display...");
  setupDisplay();
  Serial.println("[1/5] Display OK");

  Serial.println("[2/5] Montando microSD...");
  setupSD();
  Serial.println("[2/5] microSD OK");

  Serial.println("Restaurando ultimo estado del panel...");
  restoreState();

  Serial.println("[3/5] Conectando Wi-Fi...");
  connectWiFi();
  Serial.print("[3/5] Wi-Fi OK: ");
  Serial.println(WiFi.localIP());

  Serial.println("[4/5] Iniciando servidor HTTP...");
  setupServer();
  Serial.println("[4/5] Servidor HTTP OK");

  Serial.println("[5/5] Iniciando tarea de clima...");
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
    Serial.println("[5/5] Clima OK");
  } else {
    weatherTaskHandle = nullptr;
    Serial.println("[5/5] ADVERTENCIA: no se pudo iniciar tarea de clima");
  }

  Serial.println("=== Matrix Studio listo ===");
  Serial.print("mDNS: http://");
  Serial.print(MDNS_HOST);
  Serial.println(".local");
  Serial.print("IP: http://");
  Serial.println(WiFi.localIP());
}
void loop(){
  serviceAnim();
  serviceClock();
  serviceState();
  if(WiFi.status()==WL_CONNECTED)server.handleClient();
  serviceWiFi();
}
