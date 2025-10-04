#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <USB.h>
#include <USBHID.h>
#include <USBHIDMouse.h>
#include <USBHIDKeyboard.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <math.h>
#include <vector>

#if defined(USE_NEOPIXEL)
  #include <Adafruit_NeoPixel.h>
  Adafruit_NeoPixel strip(NEOPIXEL_COUNT, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);
#elif defined(USE_RGB_GPIO)
  // defina LED_R, LED_G, LED_B via build_flags
#endif

// ================= Wi-Fi (STA) com IP fixo =================
static const char* WIFI_SSID = "wifi-name";
static const char* WIFI_PASS = "wifi-password";
IPAddress local_IP(192, 168, 0, 44);
IPAddress gateway(192, 168, 0, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress dns1(8, 8, 8, 8), dns2(1, 1, 1, 1);

// ================= USB HID =================
USBHID HID;
USBHIDMouse Mouse;
USBHIDKeyboard Keyboard;

// ================= Web / Store =================
WebServer server(80);
Preferences prefs;

// ================= Modelo de passos =================
// type: "tap" | "drag" | "type" | "key" | "wait"
struct Step {
  String type;
  int x=0, y=0, x2=0, y2=0;
  String text;        // para type/key
  String btn="left";  // "left" | "right" | "middle"
  int delayMs=0;      // delay POS-ação (se 0, usa actionDelay global)
  int durMs=0;        // duração do DRAG (ms)
  int stepsN=1;       // passos do DRAG (interpolação)
};
static const int MAX_STEPS = 160;
static Step steps[MAX_STEPS];
static int stepCount = 0;

// Config macro e serviço Go
int   screenW = 1920;
int   screenH = 1080;
float countsPerPixel = 5.0f; // comece alto; depois calibre
int   actionDelay = 1500;    // ✅ Delay padrão global (ms)
bool  autoRunOnBoot = false;

String pcHost = "127.0.0.1";
int    pcPort = 5005;

// Execução/estado
TaskHandle_t runnerTask = nullptr;
volatile bool wantStop = false;
volatile bool runningLoop = false;
volatile int  runStepIndex = 0;
volatile long loopsRemaining = 0;  // >0 = contador; 0 = parar; -1 = infinito

// ================= LED helpers =================
void ledSet(uint8_t r, uint8_t g, uint8_t b){
#if defined(USE_NEOPIXEL)
  strip.setPixelColor(0, strip.Color(r,g,b));
  strip.show();
#elif defined(USE_RGB_GPIO)
  analogWrite(LED_R, r);
  analogWrite(LED_G, g);
  analogWrite(LED_B, b);
#endif
}
void ledStandby(){ ledSet(0, 0, 180); }   // azul
void ledRunning(){ ledSet(0, 180, 0); }   // verde
void ledStopped(){ ledSet(180, 0, 0); }   // vermelho

// ================= Persistência =================
void persistAll(){
  prefs.begin("cfg", false);
  prefs.putInt("w", screenW);
  prefs.putInt("h", screenH);
  prefs.putFloat("cpp", countsPerPixel);
  prefs.putInt("delay", actionDelay);
  prefs.putBool("autorun", autoRunOnBoot);
  prefs.putString("pchost", pcHost);
  prefs.putInt("pcport", pcPort);

  DynamicJsonDocument doc(32768);
  JsonArray arr = doc.createNestedArray("steps");
  for(int i=0;i<stepCount;i++){
    JsonObject o = arr.createNestedObject();
    o["type"]=steps[i].type;
    o["x"]=steps[i].x; o["y"]=steps[i].y; o["x2"]=steps[i].x2; o["y2"]=steps[i].y2;
    o["text"]=steps[i].text; o["btn"]=steps[i].btn;
    o["delayMs"]=steps[i].delayMs; o["durMs"]=steps[i].durMs; o["stepsN"]=steps[i].stepsN;
  }
  String s; serializeJson(doc, s);
  prefs.putString("macro", s);
  prefs.end();
}

void loadAll(){
  prefs.begin("cfg", true);
  screenW = prefs.getInt("w", 1920);
  screenH = prefs.getInt("h", 1080);
  countsPerPixel = prefs.getFloat("cpp", 5.0f);
  actionDelay = prefs.getInt("delay", 1500); // ✅ 1500 padrão
  autoRunOnBoot = prefs.getBool("autorun", false);
  pcHost = prefs.getString("pchost", "127.0.0.1");
  pcPort = prefs.getInt("pcport", 5005);
  String s = prefs.getString("macro", "");
  prefs.end();

  stepCount = 0;
  if(s.length()){
    DynamicJsonDocument doc(32768);
    if(deserializeJson(doc, s)==DeserializationError::Ok){
      for(JsonObject o : doc["steps"].as<JsonArray>()){
        if(stepCount>=MAX_STEPS) break;
        Step st;
        st.type = (const char*)o["type"];
        st.x=o["x"]|0; st.y=o["y"]|0; st.x2=o["x2"]|0; st.y2=o["y2"]|0;
        st.text=(const char*)(o["text"]|"");
        st.btn = (const char*)(o["btn"] | (const char*)(o["button"] | "left"));
        st.delayMs = o["delayMs"] | (o["ms"] | 0);
        st.durMs   = o["durMs"]   | 600;
        st.stepsN  = o["stepsN"]  | 1;
        if(st.stepsN < 1) st.stepsN = 1;
        steps[stepCount++]=st;
      }
    }
  }
}

// ================= CORS/JSON helpers =================
void sendCORS(){
  server.sendHeader("Access-Control-Allow-Origin","*");
  server.sendHeader("Access-Control-Allow-Methods","GET,POST,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers","Content-Type");
}
void sendJSON(int code, const String& body){
  sendCORS();
  server.send(code, "application/json; charset=utf-8", body);
}
void okJSON(){ sendJSON(200, "{\"ok\":true}"); }
void handleOptions(){ sendCORS(); server.send(204); }

// ================= HID helpers =================
void homeCursor(){
  for(int i=0;i<30;i++){ Mouse.move(-127,-127); delay(1); }
}
void moveRelCounts(long dx, long dy){
  long rx=dx, ry=dy;
  while(rx!=0 || ry!=0){
    int sx = (rx>0) ? (int)min<long>(rx,127) : (int)max<long>(rx,-127);
    int sy = (ry>0) ? (int)min<long>(ry,127) : (int)max<long>(ry,-127);
    Mouse.move(sx, sy);
    rx-=sx; ry-=sy;
    delay(1);
  }
}
inline void moveByPixels(int dpx, int dpy){
  long dx = lroundf((float)dpx * countsPerPixel);
  long dy = lroundf((float)dpy * countsPerPixel);
  moveRelCounts(dx, dy);
}
inline void tapMoveFromHome(int px, int py){
  homeCursor(); delay(8);
  moveByPixels(px, py);
}

uint8_t btnMaskFromName(const String& b){
  String s=b; s.toLowerCase();
  if(s=="right")  return MOUSE_RIGHT;
  if(s=="middle") return MOUSE_MIDDLE;
  return MOUSE_LEFT; // default
}
void pressBtn(const String& b){ Mouse.press(btnMaskFromName(b)); }
void releaseBtn(const String& b){ Mouse.release(btnMaskFromName(b)); }
void clickBtn(const String& b){ uint8_t m=btnMaskFromName(b); Mouse.press(m); delay(25); Mouse.release(m); }

void dragFromToBtn(int x1,int y1,int x2,int y2, const String& btn, int durMs, int stepsN){
  if(stepsN < 1) stepsN = 1;
  if(durMs < 0)  durMs = 0;

  tapMoveFromHome(x1, y1); delay(10);
  pressBtn(btn); delay(15);

  float cx = x1, cy = y1;
  const float stepx = (x2 - x1) / float(stepsN);
  const float stepy = (y2 - y1) / float(stepsN);
  int lastX = x1, lastY = y1;
  const int sleepPer = (durMs>0 ? max(1, durMs/stepsN) : 0);

  for(int i=0;i<stepsN;i++){
    cx += stepx; cy += stepy;
    int ix = lroundf(cx);
    int iy = lroundf(cy);
    int dpx = ix - lastX;
    int dpy = iy - lastY;
    if(dpx!=0 || dpy!=0){
      moveByPixels(dpx, dpy);
      lastX = ix; lastY = iy;
    }
    if(sleepPer>0) delay(sleepPer);
  }
  delay(10); releaseBtn(btn);
}

void typeText(const String& s){
  for(size_t i=0;i<s.length();i++){ Keyboard.print(s.substring(i,i+1)); delay(5); }
}

uint8_t mapKeyName(const String& name){
  String n=name; n.toLowerCase();
  if(n=="return"||n=="enter") return KEY_RETURN;
  if(n=="esc"||n=="escape") return KEY_ESC;
  if(n=="tab") return KEY_TAB;
  if(n=="space"||n=="spacebar") return ' ';
  if(n=="backspace") return KEY_BACKSPACE;
  if(n=="delete"||n=="del") return KEY_DELETE;
  if(n=="up") return KEY_UP_ARROW;
  if(n=="down") return KEY_DOWN_ARROW;
  if(n=="left") return KEY_LEFT_ARROW;
  if(n=="right") return KEY_RIGHT_ARROW;
  return 0;
}
bool modOn(const String& s){ return s=="ctrl"||s=="control"||s=="alt"||s=="shift"||s=="gui"||s=="cmd"||s=="win"; }
void holdMod(const String& m, bool press){
  String s=m; s.toLowerCase();
  if(s=="ctrl"||s=="control")  { if(press) Keyboard.press(KEY_LEFT_CTRL); else Keyboard.release(KEY_LEFT_CTRL); }
  else if(s=="alt")            { if(press) Keyboard.press(KEY_LEFT_ALT);  else Keyboard.release(KEY_LEFT_ALT); }
  else if(s=="shift")          { if(press) Keyboard.press(KEY_LEFT_SHIFT);else Keyboard.release(KEY_LEFT_SHIFT); }
  else if(s=="gui"||s=="cmd"||s=="win"){ if(press) Keyboard.press(KEY_LEFT_GUI); else Keyboard.release(KEY_LEFT_GUI); }
}
void sendKeyCombo(const String& combo){
  std::vector<String> parts;
  int start=0; while(true){ int idx=combo.indexOf('+',start); if(idx<0){ parts.push_back(combo.substring(start)); break; } parts.push_back(combo.substring(start,idx)); start=idx+1; }
  if(parts.empty()) return;
  for(size_t i=0;i<parts.size();i++){ String p=parts[i]; p.trim(); p.toLowerCase(); if(modOn(p)) holdMod(p,true); }
  String last=parts.back(); last.trim();
  uint8_t code=mapKeyName(last);
  if(code){ Keyboard.write(code); }
  else {
    String l=last; l.toLowerCase();
    if(l=="f1") Keyboard.write(KEY_F1); else if(l=="f2") Keyboard.write(KEY_F2);
    else if(l=="f3") Keyboard.write(KEY_F3); else if(l=="f4") Keyboard.write(KEY_F4);
    else if(l=="f5") Keyboard.write(KEY_F5); else if(l=="f6") Keyboard.write(KEY_F6);
    else if(l=="f7") Keyboard.write(KEY_F7); else if(l=="f8") Keyboard.write(KEY_F8);
    else if(l=="f9") Keyboard.write(KEY_F9); else if(l=="f10") Keyboard.write(KEY_F10);
    else if(l=="f11") Keyboard.write(KEY_F11); else if(l=="f12") Keyboard.write(KEY_F12);
    else Keyboard.print(last);
  }
  for(size_t i=0;i<parts.size();i++){ String p=parts[i]; p.trim(); p.toLowerCase(); if(modOn(p)) holdMod(p,false); }
}

static inline int postDelay(const Step& st){
  // se delayMs do passo for 0, usa actionDelay global (1500ms)
  return (st.delayMs>0? st.delayMs : actionDelay);
}

void runOnce(){
  ledRunning();
  wantStop = false;
  runStepIndex = 0;

  Mouse.move(1,0); delay(5); Mouse.move(-1,0); delay(5);

  for(int i=0;i<stepCount;i++){
    if(wantStop){ ledStopped(); return; }
    runStepIndex = i+1;
    const Step& st=steps[i];

    if(st.type=="tap"){
      tapMoveFromHome(st.x, st.y);
      clickBtn(st.btn);
      delay(postDelay(st));

    }else if(st.type=="drag"){
      int dur = (st.durMs>0? st.durMs : 600);
      int sn  = (st.stepsN>0? st.stepsN : 1);
      dragFromToBtn(st.x,st.y,st.x2,st.y2, st.btn, dur, sn);
      delay(postDelay(st));

    }else if(st.type=="type"){
      typeText(st.text); delay(postDelay(st));

    }else if(st.type=="key"){
      sendKeyCombo(st.text); delay(postDelay(st));

    }else if(st.type=="wait"){
      delay(st.delayMs>0? st.delayMs : actionDelay);
    }
  }
  ledStandby();
}

void runner(void*){
  while(true){
    if(runningLoop){
      runOnce();
      if(!runningLoop) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
      if(loopsRemaining > 0){
        loopsRemaining--;
        if(loopsRemaining == 0){
          runningLoop = false;
          ledStandby();
        }
      }
      for(int i=0;i<10 && runningLoop && !wantStop;i++) vTaskDelay(pdMS_TO_TICKS(20));
    }else{
      vTaskDelay(pdMS_TO_TICKS(50));
    }
  }
}

// ================= Proxy para serviço Go =================
void sendErrorJSON(const char* msg){ String s="{\"error\":\""; s+=msg; s+="\"}"; sendJSON(500,s); }

void proxyPcPos() {
  if (pcHost.length()==0){ sendJSON(400,"{\"error\":\"pcHost not set\"}"); return; }
  String url = "http://" + pcHost + ":" + String(pcPort) + "/pos";
  HTTPClient http; http.setTimeout(3000);
  if(!http.begin(url)){ sendErrorJSON("begin failed"); return; }
  int code=http.GET(); String body=http.getString(); http.end();
  if(code<0) { sendErrorJSON("pc not reachable"); return; }
  if(body.length()==0) body="{}";
  sendJSON(200, body);
}

void proxyPcCapture() {
  String delayS = server.arg("delay"); if(delayS=="") delayS="3";
  String url = "http://" + pcHost + ":" + String(pcPort) + "/capture?delay=" + delayS;
  HTTPClient http; http.setTimeout(35000);
  if(!http.begin(url)){ sendErrorJSON("begin failed"); return; }
  int code=http.GET(); String body=http.getString(); http.end();
  if(code<0) { sendErrorJSON("pc not reachable"); return; }
  if(body.length()==0) body="{}";
  sendJSON(200, body);
}

// ================= HTML UI =================
String htmlPage(){
  String rows;
  for(int i=0;i<stepCount;i++){
    const Step& s=steps[i];
    rows += "<tr>"
      "<td>"+String(i+1)+"</td>"
      "<td>"+s.type+"</td>"
      "<td>"+String(s.x)+","+String(s.y)+"</td>"
      "<td>"+String(s.x2)+","+String(s.y2)+"</td>"
      "<td>"+String(s.text)+"</td>"
      "<td>"+String(s.btn)+"</td>"
      "<td>"+String(s.delayMs)+"</td>"
      "<td>"+String(s.durMs)+"</td>"
      "<td>"+String(s.stepsN)+"</td>"
      "<td>"
        "<button formaction=\"/steps/up?i="+String(i)+"\" formmethod=\"POST\">↑</button>"
        "<button formaction=\"/steps/down?i="+String(i)+"\" formmethod=\"POST\">↓</button>"
        "<button formaction=\"/steps/del?i="+String(i)+"\" formmethod=\"POST\">🗑</button>"
      "</td>"
    "</tr>";
  }
  if(stepCount==0) rows += "<tr><td colspan='10' style='color:#666'>Sem passos ainda. Use os botões de captura abaixo.</td></tr>";

  String p = R"HTML(
<!doctype html><html lang="pt-br"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32-S3 • Sequência de Ações</title>
<style>
 body{font-family:system-ui,Arial;margin:16px}
 table{border-collapse:collapse;width:100%;max-width:1200px}
 th,td{border:1px solid #ddd;padding:6px;text-align:left;font-size:14px}
 th{background:#f5f5f5}
 input,select,textarea{padding:6px;width:100%}
 button{padding:8px 12px;border:0;border-radius:8px;cursor:pointer;margin:4px}
 .row{display:flex;gap:8px;flex-wrap:wrap;margin:8px 0}
 .tag{display:inline-block;background:#eee;border-radius:999px;padding:2px 10px;margin:0 6px 6px 0}
 .btn-go{background:#0ea5e9;color:#fff}.btn-good{background:#22c55e;color:#fff}
 .btn-stop{background:#ef4444;color:#fff}.btn-gray{background:#64748b;color:#fff}
 .card{border:1px solid #ddd;border-radius:12px;padding:16px;margin:12px 0;box-shadow:0 2px 10px rgba(0,0,0,.05)}
</style>

<h2>ESP32-S3 • Sequência de Ações (PC)</h2>
<div>
  <span class="tag">Modo: <span id="mode">__MODE__</span></span>
  <span class="tag">ESP: __ESPIP__</span>
  <span class="tag">PC: __HOST__:<span id="pport">__PORT__</span></span>
  <span class="tag">Status: <span id="st_mode">__STATUS__</span></span>
  <span class="tag">Passo: <span id="st_step">__STEP__</span>/<span id="st_count">__COUNT__</span></span>
  <span class="tag">Loops left: <span id="st_loops">__LOOPS__</span></span>
</div>

<form method="POST" action="/saveCfg" class="row">
  <label>IP do PC <input name="host" value="__HOST__"></label>
  <label>Porta <input type="number" min="1" max="65535" name="port" value="__PORT__"></label>
  <label>Screen W <input name="w" type="number" value="__W__"></label>
  <label>Screen H <input name="h" type="number" value="__H__"></label>
  <label>Counts/px <input name="cpp" type="number" step="0.1" value="__CPP__"></label>
  <label>Delay padrão (ms) <input name="delay" type="number" value="__DELAY__"></label>
  <label>AutoRun <input name="autorun" type="checkbox" __AUTOCHECK__></label>
  <button type="submit" class="btn-good">Salvar Config</button>
  <button type="button" class="btn-go" onclick="location.href='/test'">Testar /health</button>
  <a href="/export"><button type="button" class="btn-gray">Exportar JSON</button></a>
</form>

<form class="row" method="POST" action="/import">
  <textarea name="cfg" rows="8" style="width:100%;max-width:1200px"
    placeholder='Cole aqui o JSON exportado...'></textarea>
  <button type="submit" class="btn-good">Importar JSON</button>
</form>

<form method="POST" action="/noop">
<table>
  <tr>
    <th>#</th><th>Tipo</th><th>X,Y</th><th>X2,Y2</th><th>Texto/Tecla</th>
    <th>Botão</th><th>Delay (ms)</th><th>Duração (ms)</th><th>Steps</th><th>Ações</th>
  </tr>
__ROWS__
</table>
<div class="row">
  <button formaction="/clear" formmethod="POST" class="btn-gray">Limpar todos</button>
</div>
<div class="row">
  <label>Loops (N) <input id="loopN" type="number" min="1" max="100000" value="10"></label>
  <button type="button" class="btn-good" onclick="runLoopN()">Rodar Nx</button>
  <button formaction="/runLoop?n=0"   formmethod="POST" class="btn-good">Rodar ∞</button>
  <button formaction="/runOnce"       formmethod="POST" class="btn-go">Rodar uma vez</button>
  <button formaction="/stop"          formmethod="POST" class="btn-stop">Parar</button>
</div>
<div class="row">
  <button type="button" class="btn-gray" onclick="fetch('/hidTest').then(()=>alert('HID test OK (movi 50px e cliquei)')).catch(()=>alert('Falha'))">Testar HID (mover 50px →)</button>
</div>
</form>

<div class="card">
  <h3>Capturar posições do PC (via serviço Go)</h3>
  <div class="row">
    <label>Atraso (s) <input id="capDelay" type="number" min="0" max="30" value="3"></label>
    <span style="align-self:center">TAP:</span>
    <button class="btn-gray" type="button" onclick="capTapBtn('left')">Left</button>
    <button class="btn-gray" type="button" onclick="capTapBtn('right')">Right</button>
    <button class="btn-gray" type="button" onclick="capTapBtn('middle')">Middle</button>
    <span style="align-self:center">AGORA:</span>
    <button class="btn-gray" type="button" onclick="capPosBtn('left')">Left</button>
    <button class="btn-gray" type="button" onclick="capPosBtn('right')">Right</button>
    <button class="btn-gray" type="button" onclick="capPosBtn('middle')">Middle</button>
  </div>

  <div class="row">
    <label>Duração Drag (ms) <input id="capDur" type="number" min="0" max="5000" value="600"></label>
    <label>Steps Drag <input id="capSteps" type="number" min="1" max="200" value="1"></label>
  </div>

  <div class="row">
    <button class="btn-gray" type="button" onclick="capDragStart('left')">Drag INÍCIO (Left)</button>
    <button class="btn-gray" type="button" onclick="capDragStart('right')">Drag INÍCIO (Right)</button>
    <button class="btn-gray" type="button" onclick="capDragStart('middle')">Drag INÍCIO (Middle)</button>
    <button class="btn-gray" type="button" onclick="capDragEnd()">Drag FIM (salvar)</button>
  </div>

  <div class="row">
    <label>Delay pós-ação p/ novo step (ms) <input id="capDelayMs" type="number" min="0" max="10000" value="1500"></label>
  </div>

  <div class="row">
    <button class="btn-gray" type="button" onclick="addType()">Adicionar TYPE (texto manual)</button>
    <button class="btn-gray" type="button" onclick="addKey()">Adicionar KEY (ex.: ctrl+s, return)</button>
  </div>
  <small>Depois de capturar e salvar os passos, você pode desligar o serviço Go. O ESP roda solo via HID.</small>
</div>

<script>
function upd(){
  fetch('/status').then(r=>r.json()).then(s=>{
    document.getElementById('st_mode').textContent = s.running ? (s.loop?'loop':'running') : 'standby';
    document.getElementById('st_step').textContent = s.step;
    document.getElementById('st_count').textContent = s.count;
    document.getElementById('st_loops').textContent = (s.loops_left<0?'∞':s.loops_left);
    document.getElementById('mode').textContent = s.loop ? 'Loop' : 'Único';
  }).catch(()=>{});
}
setInterval(upd, 1000);

async function postJSON(url, obj){
  return fetch(url, { method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify(obj) });
}
async function getJSON(url){ const r=await fetch(url); return await r.json(); }
function getDelay(){ const el=document.getElementById('capDelay'); const v=el? (+el.value||0) : 0; return Math.max(0, Math.min(30, v)); }
function val(id, fallback){ const el=document.getElementById(id); return el? (+el.value||fallback) : fallback; }

function runLoopN(){
  const n = Math.max(1, Math.min(100000, parseInt(document.getElementById('loopN').value)||1));
  fetch('/runLoop?n='+n, { method:'POST' }).then(()=>{}).catch(()=>{});
}

// TAP com delay
async function capTapBtn(btn){
  const d = getDelay();
  const postDelay = val('capDelayMs', 1500); // ✅ default 1500
  alert(`Posicione o mouse. Vou capturar em ${d}s...`);
  const pos = await getJSON(`/pc/capture?delay=${d}`);
  if (typeof pos.x!=='number' || typeof pos.y!=='number'){ alert('Falha ao capturar pos'); return; }
  await postJSON('/steps/add', { type:'tap', x:pos.x, y:pos.y, button: btn, delayMs: postDelay });
  alert(`TAP ${btn} salvo em (${pos.x}, ${pos.y})`); location.reload();
}
// posição instantânea
async function capPosBtn(btn){
  const postDelay = val('capDelayMs', 1500); // ✅ default 1500
  const pos = await getJSON('/pc/pos');
  if (typeof pos.x!=='number' || typeof pos.y!=='number'){ alert('Falha ao capturar pos'); return; }
  await postJSON('/steps/add', { type:'tap', x:pos.x, y:pos.y, button: btn, delayMs: postDelay });
  alert(`TAP ${btn} salvo em (${pos.x}, ${pos.y})`); location.reload();
}
// DRAG 2 etapas
let dragTmp=null, dragBtn="left";
async function capDragStart(btn){
  const d=getDelay();
  dragBtn = btn || 'left';
  alert(`Posicione o mouse no INÍCIO (captura em ${d}s)...`);
  const pos=await getJSON(`/pc/capture?delay=${d}`);
  if (typeof pos.x!=='number' || typeof pos.y!=='number'){ alert('Falha no início'); return; }
  dragTmp=pos; alert(`Início (${pos.x},${pos.y}) salvo com botão ${dragBtn}. Agora clique "Drag FIM".`);
}
async function capDragEnd(){
  if(!dragTmp){ alert('Capture primeiro o INÍCIO'); return; }
  const d=getDelay();
  const dur= val('capDur', 600);
  const sn = Math.max(1, Math.min(200, val('capSteps', 1)));
  const postDelay = val('capDelayMs', 1500); // ✅ default 1500
  alert(`Posicione o mouse no FIM (captura em ${d}s)...`);
  const pos=await getJSON(`/pc/capture?delay=${d}`);
  if (typeof pos.x!=='number' || typeof pos.y!=='number'){ alert('Falha no fim'); return; }
  await postJSON('/steps/add', { type:'drag', x:dragTmp.x, y:dragTmp.y, x2:pos.x, y2:pos.y, button: dragBtn, durMs: dur, stepsN: sn, delayMs: postDelay });
  alert(`DRAG ${dragBtn} salvo: (${dragTmp.x},${dragTmp.y}) → (${pos.x},${pos.y})`);
  dragTmp=null; location.reload();
}
// TYPE
async function addType(){
  const postDelay = val('capDelayMs', 1500); // ✅ default 1500
  const txt = prompt("Digite o texto para TYPE:");
  if (txt===null) return;
  await postJSON('/steps/add', { type:'type', text: txt, delayMs: postDelay });
  alert('TYPE adicionado.'); location.reload();
}
// KEY
async function addKey(){
  const postDelay = val('capDelayMs', 1500); // ✅ default 1500
  const txt = prompt("Digite a tecla/atalho (ex.: ctrl+s, return, escape, tab, f5):");
  if (txt===null || !txt.trim()) return;
  await postJSON('/steps/add', { type:'key', text: txt.trim(), delayMs: postDelay });
  alert('KEY adicionado.'); location.reload();
}
</script>
</html>
)HTML";

  p.replace("__MODE__", runningLoop? "Loop":"Único");
  p.replace("__ESPIP__", WiFi.localIP().toString());
  p.replace("__HOST__", pcHost);
  p.replace("__PORT__", String(pcPort));
  p.replace("__STATUS__", (runningLoop? "loop": "standby"));
  p.replace("__STEP__", String(runStepIndex));
  p.replace("__COUNT__", String(stepCount));
  p.replace("__W__", String(screenW));
  p.replace("__H__", String(screenH));
  p.replace("__CPP__", String(countsPerPixel,1));
  p.replace("__DELAY__", String(actionDelay));
  p.replace("__AUTOCHECK__", autoRunOnBoot ? "checked" : "");
  p.replace("__ROWS__", rows);
  p.replace("__LOOPS__", String(loopsRemaining));
  return p;
}

// ================= Handlers HTTP =================
void handleRoot(){ sendCORS(); server.send(200,"text/html; charset=utf-8", htmlPage()); }

void handleStatus(){
  DynamicJsonDocument d(256);
  d["running"] = runningLoop;
  d["loop"] = runningLoop;
  d["step"] = runStepIndex;
  d["count"]= stepCount;
  d["loops_left"]= (int)loopsRemaining; // -1 = ∞
  String s; serializeJson(d,s); sendJSON(200,s);
}

void handleExport(){
  DynamicJsonDocument doc(32768);
  JsonObject cfg = doc.createNestedObject("config");
  cfg["w"]=screenW; cfg["h"]=screenH; cfg["cpp"]=countsPerPixel; cfg["delay"]=actionDelay; cfg["autorun"]=autoRunOnBoot;
  cfg["host"]=pcHost; cfg["port"]=pcPort;
  JsonArray arr = doc.createNestedArray("steps");
  for(int i=0;i<stepCount;i++){
    JsonObject o = arr.createNestedObject();
    o["type"]=steps[i].type; o["x"]=steps[i].x; o["y"]=steps[i].y; o["x2"]=steps[i].x2; o["y2"]=steps[i].y2;
    o["text"]=steps[i].text; o["btn"]=steps[i].btn;
    o["delayMs"]=steps[i].delayMs; o["durMs"]=steps[i].durMs; o["stepsN"]=steps[i].stepsN;
  }
  String s; serializeJson(doc,s); sendJSON(200,s);
}

void handleImport(){
  if(!server.hasArg("cfg")){ sendJSON(400,"{\"error\":\"no body\"}"); return; }
  DynamicJsonDocument doc(32768);
  if(deserializeJson(doc, server.arg("cfg"))!=DeserializationError::Ok){ sendJSON(400,"{\"error\":\"json\"}"); return; }

  if(doc["config"].is<JsonObject>()){
    JsonObject c = doc["config"];
    screenW = c["w"] | screenW;
    screenH = c["h"] | screenH;
    countsPerPixel = c["cpp"] | countsPerPixel;
    actionDelay = c["delay"] | actionDelay; // mantém 1500 se não vier
    autoRunOnBoot = c["autorun"] | autoRunOnBoot;
    pcHost = (const char*)(c["host"] | pcHost.c_str());
    pcPort = c["port"] | pcPort;
  }

  stepCount=0;
  if(doc["steps"].is<JsonArray>()){
    for(JsonObject o: doc["steps"].as<JsonArray>()){
      if(stepCount>=MAX_STEPS) break;
      Step st;
      st.type=(const char*)o["type"];
      st.x=o["x"]|0; st.y=o["y"]|0; st.x2=o["x2"]|0; st.y2=o["y2"]|0;
      st.text=(const char*)(o["text"]|"");
      st.btn = (const char*)(o["btn"] | (const char*)(o["button"] | "left"));
      st.delayMs = o["delayMs"] | (o["ms"] | 0);
      st.durMs   = o["durMs"]   | 600;
      st.stepsN  = o["stepsN"]  | 1;
      if(st.stepsN < 1) st.stepsN = 1;
      steps[stepCount++]=st;
    }
  }
  persistAll();
  server.sendHeader("Location","/"); sendCORS(); server.send(302);
}

void handleSaveCfg(){
  pcHost = server.arg("host").length()? server.arg("host") : pcHost;
  pcPort = server.arg("port").length()? server.arg("port").toInt(): pcPort;
  screenW = server.arg("w").length()? server.arg("w").toInt() : screenW;
  screenH = server.arg("h").length()? server.arg("h").toInt() : screenH;
  countsPerPixel = server.arg("cpp").length()? server.arg("cpp").toFloat() : countsPerPixel;
  actionDelay = server.arg("delay").length()? server.arg("delay").toInt() : actionDelay; // pode ajustar
  autoRunOnBoot = server.hasArg("autorun");
  persistAll();
  server.sendHeader("Location","/"); sendCORS(); server.send(302);
}

// ===== gerenciamento da lista (AÇÕES por linha) =====
void stepsSwap(int i,int j){ Step t=steps[i]; steps[i]=steps[j]; steps[j]=t; }
void handleStepsUp(){
  int i = server.hasArg("i")? server.arg("i").toInt() : -1;
  if(i>0 && i<stepCount){ stepsSwap(i,i-1); persistAll(); }
  server.sendHeader("Location","/"); sendCORS(); server.send(302);
}
void handleStepsDown(){
  int i = server.hasArg("i")? server.arg("i").toInt() : -1;
  if(i>=0 && i<stepCount-1){ stepsSwap(i,i+1); persistAll(); }
  server.sendHeader("Location","/"); sendCORS(); server.send(302);
}
void handleStepsDel(){
  int i = server.hasArg("i")? server.arg("i").toInt() : -1;
  if(i>=0 && i<stepCount){
    for(int k=i;k<stepCount-1;k++) steps[k]=steps[k+1];
    stepCount--; persistAll();
  }
  server.sendHeader("Location","/"); sendCORS(); server.send(302);
}

void handleClear(){ stepCount=0; persistAll(); server.sendHeader("Location","/"); sendCORS(); server.send(302); }
void handleRunOnce(){ wantStop=false; runningLoop=false; loopsRemaining=0; runStepIndex=0; ledRunning(); runOnce(); server.sendHeader("Location","/"); sendCORS(); server.send(302); }
void handleRunLoop(){
  long n = server.hasArg("n") ? server.arg("n").toInt() : 0; // n==0 => infinito
  if(n < 0) n = 0;
  loopsRemaining = (n==0 ? -1 : n);
  wantStop=false; runningLoop=true; ledRunning();
  sendCORS(); server.send(200,"application/json","{\"ok\":true}");
}
void handleStop(){ wantStop=true; runningLoop=false; loopsRemaining=0; ledStopped(); server.sendHeader("Location","/"); sendCORS(); server.send(302); }

void handleTest(){
  String url = "http://" + pcHost + ":" + String(pcPort) + "/health";
  HTTPClient http; http.setTimeout(2000);
  if(http.begin(url)){
    int c=http.GET(); String body=http.getString(); http.end();
    sendJSON(200, String("{\"code\":")+c+",\"body\":"+(body.length()?body:"\"\"")+"}");
  }else sendJSON(500,"{\"error\":\"begin failed\"}");
}

// Diagnóstico HID
void handleHidTest(){
  Mouse.move(50, 0); delay(50); Mouse.press(MOUSE_LEFT); delay(50); Mouse.release(MOUSE_LEFT);
  sendJSON(200, "{\"ok\":true}");
}

// APIs p/ app Go (ou JS da página)
void handleSetSteps(){
  if(!server.hasArg("plain")){ sendJSON(400,"{\"error\":\"no body\"}"); return; }
  DynamicJsonDocument doc(32768);
  if(deserializeJson(doc, server.arg("plain"))!=DeserializationError::Ok){ sendJSON(400,"{\"error\":\"json\"}"); return; }

  stepCount=0;
  for(JsonObject o: doc["steps"].as<JsonArray>()){
    if(stepCount>=MAX_STEPS) break;
    Step st;
    st.type   = (const char*)o["type"];
    st.x      = o["x"]   | 0;
    st.y      = o["y"]   | 0;
    st.x2     = o["x2"]  | 0;
    st.y2     = o["y2"]  | 0;
    st.text   = (const char*)(o["text"] | "");
    st.btn    = (const char*)(o["btn"]  | (const char*)(o["button"] | "left"));
    st.delayMs= o["delayMs"] | (o["ms"] | 0);
    st.durMs  = o["durMs"]   | 600;
    st.stepsN = o["stepsN"]  | 1;
    if(st.stepsN < 1) st.stepsN = 1;
    steps[stepCount++] = st;
  }
  persistAll();
  okJSON();
}
void handleAddStep(){
  if(!server.hasArg("plain")){ sendJSON(400,"{\"error\":\"no body\"}"); return; }
  DynamicJsonDocument d(2048);
  if(deserializeJson(d, server.arg("plain"))!=DeserializationError::Ok){ sendJSON(400,"{\"error\":\"json\"}"); return; }
  if(stepCount>=MAX_STEPS){ sendJSON(400,"{\"error\":\"max steps\"}"); return; }

  Step st;
  st.type   = (const char*)d["type"];
  st.x      = d["x"]   | 0;
  st.y      = d["y"]   | 0;
  st.x2     = d["x2"]  | 0;
  st.y2     = d["y2"]  | 0;
  st.text   = (const char*)(d["text"] | "");
  st.btn    = (const char*)(d["btn"]  | (const char*)(d["button"] | "left"));
  st.delayMs= d["delayMs"] | (d["ms"] | 0);
  st.durMs  = d["durMs"]   | 600;
  st.stepsN = d["stepsN"]  | 1;
  if(st.stepsN < 1) st.stepsN = 1;

  steps[stepCount++] = st;
  persistAll();
  okJSON();
}
void handleGetSteps(){
  DynamicJsonDocument doc(32768);
  JsonArray arr = doc.createNestedArray("steps");
  for(int i=0;i<stepCount;i++){
    JsonObject o = arr.createNestedObject();
    o["type"]=steps[i].type; o["x"]=steps[i].x; o["y"]=steps[i].y;
    o["x2"]=steps[i].x2; o["y2"]=steps[i].y2; o["text"]=steps[i].text;
    o["btn"]=steps[i].btn; o["delayMs"]=steps[i].delayMs; o["durMs"]=steps[i].durMs; o["stepsN"]=steps[i].stepsN;
  }
  String s; serializeJson(doc,s); sendJSON(200,s);
}
void handleClearStepsAPI(){ stepCount=0; persistAll(); okJSON(); }

// Proxy Go
void handlePcPos(){ proxyPcPos(); }
void handlePcCap(){ proxyPcCapture(); }

void setup(){
  Serial.begin(115200); delay(100);

#if defined(USE_NEOPIXEL)
  strip.begin(); strip.setBrightness(60); strip.show();
#elif defined(USE_RGB_GPIO)
  pinMode(LED_R, OUTPUT); pinMode(LED_G, OUTPUT); pinMode(LED_B, OUTPUT);
#endif
  ledStandby();

  USB.begin(); // importante para enumerar HID
  delay(300);

  HID.begin();
  Mouse.begin();
  Keyboard.begin();

  WiFi.mode(WIFI_STA);
  WiFi.config(local_IP, gateway, subnet, dns1, dns2);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("[WiFi] Conectando");
  for(int i=0;i<60 && WiFi.status()!=WL_CONNECTED;i++){ delay(250); Serial.print("."); }
  Serial.println();
  if(WiFi.status()==WL_CONNECTED){
    Serial.printf("[WiFi] OK %s\n", WiFi.localIP().toString().c_str());
    if(MDNS.begin("autoclicker")){ MDNS.addService("http","tcp",80); }
  } else {
    Serial.println("[WiFi] Falhou — siga usando só USB HID");
  }

  loadAll();

  // rotas
  server.onNotFound([](){
    if(server.method()==HTTP_OPTIONS){ handleOptions(); return; }
    sendCORS(); server.send(404,"application/json","{\"error\":\"not found\"}");
  });
  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/export", HTTP_GET, handleExport);
  server.on("/import", HTTP_POST, handleImport);
  server.on("/saveCfg", HTTP_POST, handleSaveCfg);
  server.on("/clear", HTTP_POST, handleClear);
  server.on("/runOnce", HTTP_POST, handleRunOnce);
  server.on("/runLoop", HTTP_POST, handleRunLoop); // aceita ?n=...
  server.on("/stop", HTTP_POST, handleStop);
  server.on("/test", HTTP_GET, handleTest);
  server.on("/hidTest", HTTP_GET, handleHidTest);

  // APIs e proxy
  server.on("/steps/set",   HTTP_POST, handleSetSteps);
  server.on("/steps/add",   HTTP_POST, handleAddStep);
  server.on("/steps/get",   HTTP_GET,  handleGetSteps);
  server.on("/steps/clear", HTTP_POST, handleClearStepsAPI);
  server.on("/steps/up",    HTTP_POST, handleStepsUp);
  server.on("/steps/down",  HTTP_POST, handleStepsDown);
  server.on("/steps/del",   HTTP_POST, handleStepsDel);

  server.on("/pc/pos",     HTTP_GET, handlePcPos);
  server.on("/pc/capture", HTTP_GET, handlePcCap);

  // preflight
  server.on("/export",      HTTP_OPTIONS, handleOptions);
  server.on("/import",      HTTP_OPTIONS, handleOptions);
  server.on("/saveCfg",     HTTP_OPTIONS, handleOptions);
  server.on("/clear",       HTTP_OPTIONS, handleOptions);
  server.on("/runOnce",     HTTP_OPTIONS, handleOptions);
  server.on("/runLoop",     HTTP_OPTIONS, handleOptions);
  server.on("/stop",        HTTP_OPTIONS, handleOptions);
  server.on("/steps/set",   HTTP_OPTIONS, handleOptions);
  server.on("/steps/add",   HTTP_OPTIONS, handleOptions);
  server.on("/steps/get",   HTTP_OPTIONS, handleOptions);
  server.on("/steps/clear", HTTP_OPTIONS, handleOptions);
  server.on("/steps/up",    HTTP_OPTIONS, handleOptions);
  server.on("/steps/down",  HTTP_OPTIONS, handleOptions);
  server.on("/steps/del",   HTTP_OPTIONS, handleOptions);
  server.on("/pc/pos",      HTTP_OPTIONS, handleOptions);
  server.on("/pc/capture",  HTTP_OPTIONS, handleOptions);

  server.begin();

  xTaskCreatePinnedToCore(runner, "runner", 4096, nullptr, 1, &runnerTask, 1);

  if(autoRunOnBoot){
    runningLoop=true; loopsRemaining=-1; ledRunning();
  } else {
    ledStandby();
  }

  Serial.println("[READY] UI: http://192.168.0.44  | mDNS: http://autoclicker.local");
}

void loop(){ server.handleClient(); }
