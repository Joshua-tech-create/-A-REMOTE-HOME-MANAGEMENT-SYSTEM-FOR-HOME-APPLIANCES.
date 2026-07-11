/*
 * ╔══════════════════════════════════════════════════════════════════╗
 * ║        UICT SMART HOME  —  Local Web Server Edition  v3.3       ║
 * ║  Board  : NodeMCU ESP8266 12E                                    ║
 * ║  Config : 2 relays, no physical buttons                          ║
 * ║                                                                  ║
 * ║  NEW in v3.2 — Login gate                                        ║
 * ║   • Every route requires a valid session cookie                  ║
 * ║   • Unauthenticated requests → redirect to /login                ║
 * ║   • POST /login validates username + password                    ║
 * ║   • On success: sets SID cookie (HttpOnly, 8-hour expiry)        ║
 * ║   • GET  /logout clears cookie and returns to /login             ║
 * ║   • Session token is a random 64-hex-char string, regenerated   ║
 * ║     each boot (no persistent storage needed)                     ║
 * ║                                                                  ║
 * ║  Endpoints:                                                       ║
 * ║   GET  /login                  — login page                      ║
 * ║   POST /login                  — authenticate                    ║
 * ║   GET  /logout                 — clear session                   ║
 * ║   GET  /          (auth)       — dashboard UI                    ║
 * ║   GET  /state     (auth)       — JSON snapshot                   ║
 * ║   POST /toggle?relay=N (auth)  — flip relay (N = 1 or 2)        ║
 * ║   POST /set?relay=N&state=0|1  — set relay explicitly            ║
 * ╚══════════════════════════════════════════════════════════════════╝
 *
 *  WIRING  (NodeMCU ESP8266 12E)
 *  ──────────────────────────────────────────────────────────────────
 *  Relay board (active-LOW typical):
 *    RELAY1 → D1 (GPIO5)     RELAY2 → D2 (GPIO4)
 *  Status LED: D4 (GPIO2) — built-in LED, active-LOW on NodeMCU
 *              (LED is ON when GPIO2 = LOW, OFF when GPIO2 = HIGH)
 *  No physical push-buttons used.
 *  ──────────────────────────────────────────────────────────────────
 *
 *  LIBRARIES  (Arduino Library Manager)
 *  ──────────────────────────────────────────────────────────────────
 *    ArduinoJson  >= 6.x   (Benoit Blanchon)
 *    ArduinoOTA            bundled with ESP8266 Arduino core
 *    ESP8266WiFi           bundled with ESP8266 Arduino core
 *    ESP8266WebServer      bundled with ESP8266 Arduino core
 *    EEPROM                bundled with ESP8266 Arduino core
 *
 *  BOARD MANAGER
 *  ──────────────────────────────────────────────────────────────────
 *    URL : https://arduino.esp8266.com/stable/package_esp8266com_index.json
 *    Board: "NodeMCU 1.0 (ESP-12E Module)"
 *
 *  KEY DIFFERENCES FROM ESP32 VERSION
 *  ──────────────────────────────────────────────────────────────────
 *    • ESP8266WiFi / ESP8266WebServer replace WiFi / WebServer
 *    • ArduinoOTA uses ESP8266mDNS (included automatically)
 *    • Preferences (NVS) replaced with EEPROM emulation
 *    • esp_random() replaced with RANDOM_REG32 (hardware RNG)
 *    • STATUS_LED is active-LOW (inverted vs ESP32 DevKit V1)
 *    • GPIO pins remapped to NodeMCU D-pin equivalents
 *    • WiFi.setHostname() → WiFi.hostname()
 *    • Serial.printf() works the same way
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <EEPROM.h>

// ═══════════════════════════════════════════════════════════════════════════════
//  USER CONFIGURATION
// ═══════════════════════════════════════════════════════════════════════════════

// ── WiFi credentials (tries each in order; connects to the first that succeeds) ──
struct WifiCred {
  const char* ssid;
  const char* password;
};
static const WifiCred WIFI_NETWORKS[] = {
  { "KIGULA's S24 Ultra", "iambrina123" },
  { "Galaxy A06 6012", "0778839552" },
  // Add more networks here as needed:
  // { "MyOtherSSID", "MyOtherPass" },
};
static const int WIFI_NETWORK_COUNT = sizeof(WIFI_NETWORKS) / sizeof(WIFI_NETWORKS[0]);

#define WIFI_HOSTNAME "smarthome"

#define OTA_HOSTNAME "SmartHome-OTA"
#define OTA_PASSWORD "ota_secure_pass"

// ── Web UI login credentials ──────────────────────────────────────────────────
#define UI_USERNAME "s_home"
#define UI_PASSWORD "2026"

// ── Relay count ───────────────────────────────────────────────────────────────
#define NUM_RELAYS 2

// ── Relay labels & icons ──────────────────────────────────────────────────────
// Valid icons: "bulb" "drop" "fan" "lock" "bolt" "thermo"
#define R1_LABEL "Living Room Light"
#define R1_ICON "bulb"
#define R2_LABEL "Garden Pump"
#define R2_ICON "drop"

// ── Relay logic ───────────────────────────────────────────────────────────────
#define RELAY_ON HIGH  // flipped: active-HIGH
#define RELAY_OFF LOW  // flipped: active-LOW

// ── Timing ────────────────────────────────────────────────────────────────────
#define WIFI_RETRY_BASE_MS 1000
#define WIFI_RETRY_MAX_MS 30000
#define LED_FAST_PERIOD_MS 150
#define LED_SLOW_PERIOD_MS 2000

// ── Session ───────────────────────────────────────────────────────────────────
#define SESSION_COOKIE_NAME "SID"
#define SESSION_MAX_AGE 28800  // 8 hours in seconds

// ── EEPROM ────────────────────────────────────────────────────────────────────
// ESP8266 has no NVS/Preferences; we use EEPROM emulation instead.
// Layout: byte[0] = magic (0xAB), byte[1] = relay bitmask
#define EEPROM_SIZE 8
#define EEPROM_MAGIC 0xAB
#define EEPROM_ADDR_MAGIC 0
#define EEPROM_ADDR_RELAYS 1

// ═══════════════════════════════════════════════════════════════════════════════
//  PIN MAP  (NodeMCU ESP8266 12E)
//  NodeMCU label → GPIO number
//  D1 = GPIO5, D2 = GPIO4, D4 = GPIO2 (built-in LED, active-LOW)
// ═══════════════════════════════════════════════════════════════════════════════
#define RELAY1_PIN 4   // D2
#define RELAY2_PIN 14  // D5
#define STATUS_LED 2   // D4 — active-LOW on NodeMCU (LED ON when pin LOW)

// ═══════════════════════════════════════════════════════════════════════════════
//  DEBUG
// ═══════════════════════════════════════════════════════════════════════════════
#define LOG_I(m) Serial.println(F("[INFO] " m))
#define LOG_W(m) Serial.println(F("[WARN] " m))
#define LOG_V(k, v) \
  { \
    Serial.print(F("[DBG]  " k)); \
    Serial.println(v); \
  }

// ═══════════════════════════════════════════════════════════════════════════════
//  LOGIN PAGE HTML  (PROGMEM)
// ═══════════════════════════════════════════════════════════════════════════════
const char HTML_LOGIN[] PROGMEM =
  "<!DOCTYPE html><html lang='en'><head>"
  "<meta charset='UTF-8'/>"
  "<meta name='viewport' content='width=device-width,initial-scale=1'/>"
  "<title>SmartHome &mdash; Sign In</title>"
  "<link href='https://fonts.googleapis.com/css2?family=Space+Grotesk:wght@400;500;700&family=JetBrains+Mono:wght@400;500&display=swap' rel='stylesheet'/>"
  "<style>"
  "*,*::before,*::after{box-sizing:border-box;margin:0;padding:0;}"
  "body{background:#000;color:#87ceeb;font-family:'Space Grotesk',sans-serif;"
  "min-height:100vh;display:flex;flex-direction:column;align-items:center;justify-content:center;}"
  "body::before{content:'';position:fixed;inset:0;"
  "background-image:linear-gradient(rgba(135,206,235,.04)1px,transparent 1px),"
  "linear-gradient(90deg,rgba(135,206,235,.04)1px,transparent 1px);"
  "background-size:48px 48px;pointer-events:none;}"
  ".card{position:relative;z-index:1;background:#0d0d0d;"
  "border:2px solid #87ceeb;"
  "border-radius:20px;padding:48px 40px 40px;width:100%;max-width:420px;}"
  ".card::before{content:'';position:absolute;top:0;left:28px;right:28px;height:2px;"
  "background:linear-gradient(90deg,transparent,#ffe600,transparent);}"
  ".logo{text-align:center;margin-bottom:36px;}"
  ".logo-title{font-size:34px;font-weight:700;letter-spacing:-.5px;color:#87ceeb;}"
  ".logo-title span{color:#ffe600;}"
  ".logo-sub{font-family:'JetBrains Mono',monospace;font-size:13px;color:#b8a0d8;"
  "letter-spacing:1px;margin-top:8px;text-transform:uppercase;}"
  ".field{margin-bottom:20px;}"
  ".field label{display:block;font-family:'JetBrains Mono',monospace;font-size:13px;"
  "color:#b8a0d8;letter-spacing:.8px;text-transform:uppercase;margin-bottom:9px;font-weight:500;}"
  ".field input{width:100%;background:#000;border:2px solid #b8a0d8;"
  "border-radius:12px;padding:16px 18px;"
  "font-family:'Space Grotesk',sans-serif;font-size:18px;color:#87ceeb;"
  "outline:none;transition:border-color .2s;}"
  ".field input:focus{border-color:#ffe600;}"
  ".field input::placeholder{color:#333;}"
  ".pw-wrap{position:relative;}"
  ".pw-wrap input{padding-right:52px;}"
  ".eye{position:absolute;right:14px;top:50%;transform:translateY(-50%);"
  "background:none;border:none;cursor:pointer;color:#b8a0d8;padding:4px;"
  "display:flex;align-items:center;justify-content:center;transition:color .2s;}"
  ".eye:hover{color:#ffe600;}"
  ".eye svg{width:22px;height:22px;stroke:currentColor;fill:none;stroke-width:1.8;"
  "stroke-linecap:round;stroke-linejoin:round;}"
  ".err-msg{background:rgba(255,68,68,.1);border:2px solid rgba(255,68,68,.4);"
  "border-radius:10px;padding:14px 18px;"
  "font-family:'JetBrains Mono',monospace;font-size:14px;color:#ff4444;"
  "margin-bottom:20px;display:none;letter-spacing:.3px;}"
  ".err-msg.show{display:block;}"
  ".btn{width:100%;background:#ffe600;border:none;border-radius:12px;"
  "padding:18px;margin-top:10px;"
  "font-family:'Space Grotesk',sans-serif;font-size:20px;font-weight:700;"
  "color:#000;cursor:pointer;transition:background .2s,transform .1s;letter-spacing:.3px;}"
  ".btn:hover{background:#ffd000;}"
  ".btn:active{transform:scale(.98);}"
  ".btn:disabled{opacity:.5;cursor:not-allowed;}"
  ".footer-note{text-align:center;margin-top:28px;"
  "font-family:'JetBrains Mono',monospace;font-size:11px;color:#b8a0d8;"
  "letter-spacing:.5px;line-height:1.8;}"
  "@keyframes shake{"
  "0%,100%{transform:translateX(0);}15%{transform:translateX(-7px);}30%{transform:translateX(7px);}"
  "45%{transform:translateX(-5px);}60%{transform:translateX(5px);}75%{transform:translateX(-3px);}"
  "90%{transform:translateX(3px);}}"
  ".shake{animation:shake .45s ease;}"
  "@keyframes spin{to{transform:rotate(360deg);}}"
  ".spin{display:inline-block;width:18px;height:18px;border:3px solid rgba(0,0,0,.3);"
  "border-top-color:#000;border-radius:50%;animation:spin .7s linear infinite;"
  "vertical-align:middle;margin-right:8px;}"
  "</style></head><body>"
  "<div class='card' id='card'>"
  "<div class='logo'>"
  "<div class='logo-title'>Smart<span>Home</span></div>"
  "<div class='logo-sub'>Secure Access</div>"
  "</div>"
  "<div class='err-msg' id='err'>Incorrect username or password</div>"
  "<div class='field'><label>Username</label>"
  "<input type='text' id='usr' placeholder='Enter username' autocomplete='username' autofocus/></div>"
  "<div class='field'><label>Password</label>"
  "<div class='pw-wrap'>"
  "<input type='password' id='pwd' placeholder='Enter password' autocomplete='current-password'"
  " onkeydown='if(event.key==\"Enter\")doLogin()'/>"
  "<button class='eye' onclick='togglePw()' tabindex='-1'>"
  "<svg id='eye-icon' viewBox='0 0 24 24'>"
  "<path d='M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z'/><circle cx='12' cy='12' r='3'/>"
  "</svg></button></div></div>"
  "<button class='btn' id='btn' onclick='doLogin()'>Sign In</button>"
  "<div class='footer-note'>NodeMCU ESP8266 12E &nbsp;&middot;&nbsp; UICT Smart Home v3.3</div>"
  "</div>"
  "<script>"
  "var pwVisible=false;"
  "function togglePw(){"
  "pwVisible=!pwVisible;"
  "var i=document.getElementById('pwd');i.type=pwVisible?'text':'password';"
  "document.getElementById('eye-icon').innerHTML=pwVisible?"
  "\"<path d='M17.94 17.94A10.07 10.07 0 0 1 12 20c-7 0-11-8-11-8a18.45 18.45 0 0 1 5.06-5.94'/>"
  "<path d='M9.9 4.24A9.12 9.12 0 0 1 12 4c7 0 11 8 11 8a18.5 18.5 0 0 1-2.16 3.19'/>"
  "<line x1='1' y1='1' x2='23' y2='23'/>\":"
  "\"<path d='M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z'/><circle cx='12' cy='12' r='3'/>\";}"
  "function doLogin(){"
  "var u=document.getElementById('usr').value.trim();"
  "var p=document.getElementById('pwd').value;"
  "var btn=document.getElementById('btn'),err=document.getElementById('err');"
  "if(!u||!p){err.classList.add('show');return;}"
  "btn.disabled=true;btn.innerHTML='<span class=\\'spin\\'></span>Signing in...';"
  "err.classList.remove('show');"
  "var body='username='+encodeURIComponent(u)+'&password='+encodeURIComponent(p);"
  "fetch('/login',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body})"
  ".then(function(r){"
  "if(r.ok||r.redirected){window.location.href='/';}"
  "else{btn.disabled=false;btn.innerHTML='Sign In';err.classList.add('show');"
  "document.getElementById('card').classList.add('shake');"
  "setTimeout(function(){document.getElementById('card').classList.remove('shake');},500);}"
  "}).catch(function(){btn.disabled=false;btn.innerHTML='Sign In';err.classList.add('show');});}"
  "</script></body></html>";

// ═══════════════════════════════════════════════════════════════════════════════
//  DASHBOARD HTML SECTIONS
// ═══════════════════════════════════════════════════════════════════════════════

const char HTML_A[] PROGMEM =
  "<!DOCTYPE html><html lang='en'><head>"
  "<meta charset='UTF-8'/><meta name='viewport' content='width=device-width,initial-scale=1'/>"
  "<title>SmartHome</title>"
  "<link href='https://fonts.googleapis.com/css2?family=Space+Grotesk:wght@400;500;700&family=JetBrains+Mono:wght@400;500&display=swap' rel='stylesheet'/>"
  "<style>"
  "*,*::before,*::after{box-sizing:border-box;margin:0;padding:0;}"
  "body{background:#000;color:#87ceeb;font-family:'Space Grotesk',sans-serif;"
  "min-height:100vh;display:flex;flex-direction:column;align-items:center;}"
  "body::before{content:'';position:fixed;inset:0;"
  "background-image:linear-gradient(rgba(135,206,235,.04)1px,transparent 1px),"
  "linear-gradient(90deg,rgba(135,206,235,.04)1px,transparent 1px);"
  "background-size:48px 48px;pointer-events:none;}"
  ".w{position:relative;z-index:1;width:100%;max-width:520px;padding:32px 20px 56px;}"
  "header{display:flex;align-items:flex-start;justify-content:space-between;margin-bottom:36px;}"
  ".bt{font-size:30px;font-weight:700;letter-spacing:-.5px;color:#87ceeb;}"
  ".bt span{color:#ffe600;}"
  ".bs{font-family:'JetBrains Mono',monospace;font-size:13px;color:#b8a0d8;margin-top:5px;letter-spacing:.4px;}"
  ".hright{display:flex;align-items:center;gap:10px;}"
  ".pill{display:flex;align-items:center;gap:7px;background:#0d0d0d;border:2px solid #87ceeb;"
  "border-radius:20px;padding:8px 16px;font-family:'JetBrains Mono',monospace;font-size:13px;color:#87ceeb;}"
  ".dot{width:9px;height:9px;border-radius:50%;background:#87ceeb;}"
  ".logout-btn{background:none;border:2px solid #b8a0d8;border-radius:20px;"
  "padding:8px 16px;font-family:'JetBrains Mono',monospace;font-size:13px;"
  "color:#b8a0d8;cursor:pointer;transition:all .18s;}"
  ".logout-btn:hover{border-color:#ff4444;color:#ff4444;}"
  ".stats{display:grid;grid-template-columns:repeat(3,1fr);gap:10px;margin-bottom:26px;}"
  ".st{background:#0d0d0d;border:2px solid #b8a0d8;border-radius:12px;padding:18px 16px 14px;}"
  ".sl{font-family:'JetBrains Mono',monospace;font-size:11px;color:#b8a0d8;"
  "letter-spacing:.8px;text-transform:uppercase;margin-bottom:8px;}"
  ".sv{font-family:'JetBrains Mono',monospace;font-size:22px;font-weight:500;color:#87ceeb;}"
  ".sv.g{color:#ffe600;}.sv.b{color:#87ceeb;}"
  ".bulk{display:flex;gap:10px;margin-bottom:18px;}"
  ".bb{flex:1;background:#0d0d0d;border:2px solid #b8a0d8;border-radius:12px;"
  "padding:16px;font-family:'Space Grotesk',sans-serif;font-size:17px;font-weight:700;"
  "color:#b8a0d8;cursor:pointer;transition:all .18s;}"
  ".bb:hover{border-color:#87ceeb;color:#87ceeb;background:#0d0d0d;}"
  ".bon:hover{border-color:#ffe600!important;color:#ffe600!important;background:#0d0d0d!important;}"
  ".bof:hover{border-color:#ff4444!important;color:#ff4444!important;background:#0d0d0d!important;}"
  ".relays{display:flex;flex-direction:column;gap:12px;}"
  ".rc{background:#0d0d0d;border:2px solid #b8a0d8;border-radius:16px;"
  "padding:22px 24px;display:flex;align-items:center;gap:18px;"
  "cursor:pointer;user-select:none;position:relative;overflow:hidden;"
  "transition:border-color .2s,transform .12s,background .2s;}"
  ".rc::before{content:'';position:absolute;left:0;top:0;bottom:0;width:4px;"
  "background:#b8a0d8;border-radius:4px 0 0 4px;transition:background .3s;}"
  ".rc:hover{border-color:#87ceeb;transform:translateY(-1px);}"
  ".rc:active{transform:scale(.99) translateY(0);}"
  ".rc.on{background:#0d0d00;border-color:#ffe600;}"
  ".rc.on::before{background:#ffe600;}"
  ".rc.busy{opacity:.6;pointer-events:none;}"
  ".ri{width:52px;height:52px;border-radius:12px;background:#000;"
  "border:2px solid #b8a0d8;display:flex;align-items:center;justify-content:center;"
  "flex-shrink:0;transition:background .3s,border-color .3s;}"
  ".rc.on .ri{background:#1a1a00;border-color:#ffe600;}"
  ".ri svg{width:26px;height:26px;stroke:#b8a0d8;fill:none;"
  "stroke-width:1.8;stroke-linecap:round;stroke-linejoin:round;transition:stroke .3s;}"
  ".rc.on .ri svg{stroke:#ffe600;}"
  ".rinfo{flex:1;min-width:0;}"
  ".rname{font-size:18px;font-weight:700;color:#87ceeb;margin-bottom:5px;"
  "white-space:nowrap;overflow:hidden;text-overflow:ellipsis;}"
  ".rmeta{font-family:'JetBrains Mono',monospace;font-size:13px;color:#b8a0d8;transition:color .3s;}"
  ".rc.on .rmeta{color:#ffe600;}"
  ".tog{flex-shrink:0;width:56px;height:30px;border-radius:15px;"
  "background:#222;border:2px solid #b8a0d8;position:relative;"
  "transition:background .25s,border-color .25s;}"
  ".tog::after{content:'';position:absolute;top:3px;left:3px;width:20px;height:20px;"
  "border-radius:50%;background:#b8a0d8;"
  "transition:transform .25s cubic-bezier(.34,1.56,.64,1),background .25s;"
  "box-shadow:0 1px 3px rgba(0,0,0,.6);}"
  ".rc.on .tog{background:#333300;border-color:#ffe600;}"
  ".rc.on .tog::after{transform:translateX(26px);background:#ffe600;}"
  ".rn{font-family:'JetBrains Mono',monospace;font-size:11px;color:#b8a0d8;"
  "background:#000;border:1px solid #b8a0d8;border-radius:5px;"
  "padding:3px 7px;position:absolute;top:13px;right:16px;letter-spacing:.4px;}"
  "footer{margin-top:36px;text-align:center;font-family:'JetBrains Mono',monospace;"
  "font-size:11px;color:#b8a0d8;letter-spacing:.5px;line-height:2;position:relative;}"
  "@keyframes rpl{from{opacity:.25;transform:scale(0);}to{opacity:0;transform:scale(4);}}"
  ".rpl{position:absolute;border-radius:50%;width:64px;height:64px;"
  "background:#ffe600;animation:rpl .5s ease-out forwards;pointer-events:none;}"
  "</style></head><body><div class='w'>"
  "<header><div>"
  "<div class='bt'>Smart<span>Home</span></div>"
  "<div class='bs' id='ipl'>";

const char HTML_B[] PROGMEM =
  "</div></div>"
  "<div class='hright'>"
  "<div class='pill'><div class='dot'></div><span>online</span></div>"
  "<button class='logout-btn' onclick='doLogout()'>Sign out</button>"
  "</div></header>"
  "<div class='stats'>"
  "<div class='st'><div class='sl'>Active</div><div class='sv g' id='son'>";

const char HTML_C[] PROGMEM =
  " / 2</div></div>"
  "<div class='st'><div class='sl'>Uptime</div><div class='sv b' id='sup'>";

const char HTML_D[] PROGMEM =
  "</div></div>"
  "<div class='st'><div class='sl'>RSSI</div><div class='sv' id='srs'>";

const char HTML_E[] PROGMEM =
  " dB</div></div>"
  "</div>"
  "<div class='bulk'>"
  "<button class='bb bon' onclick='bulkSet(1)'>All On</button>"
  "<button class='bb bof' onclick='bulkSet(0)'>All Off</button>"
  "</div>"
  "<div class='relays' id='relays'>";

const char HTML_F[] PROGMEM =
  "</div>"
  "<footer>NodeMCU ESP8266 12E &nbsp;&middot;&nbsp; UICT Smart Home v3.3<br/>"
  "<span id='fip'></span></footer>"
  "</div>"
  "<script>"
  "var S={r:[false,false],rssi:0,uptime:0};"
  "function applyState(){"
  "var on=S.r.filter(Boolean).length;"
  "document.getElementById('son').textContent=on;"
  "document.getElementById('srs').textContent=S.rssi;"
  "var u=S.uptime,h=Math.floor(u/3600),m=Math.floor((u%3600)/60),s=u%60;"
  "document.getElementById('sup').textContent=(h>0?h+'h ':' ')+(m>0?m+'m ':'')+s+'s';"
  "S.r.forEach(function(on,i){"
  "var c=document.getElementById('c'+i),m=document.getElementById('m'+i);"
  "if(!c)return;"
  "c.classList.toggle('on',on);"
  "m.textContent=on?'ACTIVE \\u00b7 RUNNING':'STANDBY';"
  "});}"
  "function checkAuth(r){"
  "if(r.status===401||r.status===403){window.location.href='/login';return false;}"
  "return true;}"
  "function fetchState(){"
  "fetch('/state',{cache:'no-store'}).then(function(r){"
  "if(!checkAuth(r))return r;return r.json();"
  "}).then(function(d){"
  "if(!d)return;"
  "S.r=[!!d.relay1,!!d.relay2];"
  "S.rssi=d.rssi||0;S.uptime=d.uptime||0;"
  "var ip=d.ip||location.hostname;"
  "document.getElementById('ipl').textContent=ip;"
  "document.getElementById('fip').textContent=ip;"
  "applyState();"
  "}).catch(function(){});}"
  "function toggleRelay(i,ev){"
  "var c=document.getElementById('c'+i);"
  "c.classList.add('busy');"
  "var rect=c.getBoundingClientRect();"
  "var r=document.createElement('div');r.className='rpl';"
  "r.style.left=(ev.clientX-rect.left-28)+'px';"
  "r.style.top=(ev.clientY-rect.top-28)+'px';"
  "c.appendChild(r);setTimeout(function(){r.remove();},550);"
  "fetch('/toggle?relay='+(i+1),{method:'POST',cache:'no-store'})"
  ".then(function(r){if(!checkAuth(r))return r;return r.json();})"
  ".then(function(d){"
  "if(!d)return;"
  "S.r=[!!d.relay1,!!d.relay2];"
  "applyState();"
  "}).catch(function(){}).finally(function(){c.classList.remove('busy');});}"
  "function bulkSet(on){"
  "var ps=[];"
  "for(var i=1;i<=2;i++)ps.push(fetch('/set?relay='+i+'&state='+on,{method:'POST'}));"
  "Promise.all(ps).then(fetchState);}"
  "function doLogout(){"
  "fetch('/logout').then(function(){window.location.href='/login';});}"
  "fetchState();"
  "setInterval(fetchState,3000);"
  "</script></body></html>";

// ═══════════════════════════════════════════════════════════════════════════════
//  ICON & CARD BUILDERS
// ═══════════════════════════════════════════════════════════════════════════════
String iconSVG(const char* name) {
  String s = F("<svg viewBox='0 0 24 24'>");
  String n(name);
  if (n == "bulb")
    s += F("<line x1='9' y1='18' x2='15' y2='18'/><line x1='10' y1='22' x2='14' y2='22'/>"
           "<path d='M12 2a7 7 0 0 1 7 7c0 2.38-1.19 4.47-3 5.74V17a1 1 0 0 1-1 1H9a1 1 0 0 1-1-1v-2.26C6.19 13.47 5 11.38 5 9a7 7 0 0 1 7-7z'/>");
  else if (n == "drop")
    s += F("<path d='M12 2.69l5.66 5.66a8 8 0 1 1-11.31 0z'/>");
  else if (n == "fan")
    s += F("<path d='M12 12c1.11 0 2-.89 2-2 0-1.43-1.31-2.6-3-3-1.44-.33-3.07.62-4 2-.93 1.38-.51 2.91.5 3.5'/>"
           "<path d='M12 12c-.55.95-.68 1.95-.33 2.89.5 1.38 1.72 2.13 3.11 2.11 1.39-.02 2.72-.77 3.22-2.11.5-1.39 0-2.72-1-3.5'/>"
           "<path d='M12 12c-1.65.56-3.07.38-4.23-.5-1.17-.88-1.68-2.33-1.27-3.71.4-1.38 1.69-2.37 3.27-2.29'/>"
           "<circle cx='12' cy='12' r='1.5'/>");
  else if (n == "lock")
    s += F("<rect x='3' y='11' width='18' height='11' rx='2'/><path d='M7 11V7a5 5 0 0 1 10 0v4'/>");
  else if (n == "bolt")
    s += F("<polyline points='13 2 3 14 12 14 11 22 21 10 12 10 13 2'/>");
  else
    s += F("<path d='M14 14.76V3.5a2.5 2.5 0 0 0-5 0v11.26a4.5 4.5 0 1 0 5 0z'/>");
  s += F("</svg>");
  return s;
}

String buildCard(int idx, const char* label, const char* icon, bool isOn) {
  String s;
  s.reserve(512);
  s += F("<div class='rc");
  if (isOn) s += F(" on");
  s += F("' id='c");
  s += idx;
  s += F("' onclick='toggleRelay(");
  s += idx;
  s += F(",event)'>");
  s += F("<span class='rn'>R");
  s += (idx + 1);
  s += F("</span>");
  s += F("<div class='ri'>");
  s += iconSVG(icon);
  s += F("</div>");
  s += F("<div class='rinfo'><div class='rname'>");
  s += label;
  s += F("</div>");
  s += F("<div class='rmeta' id='m");
  s += idx;
  s += F("'>");
  s += isOn ? F("ACTIVE &middot; RUNNING") : F("STANDBY");
  s += F("</div></div><div class='tog'></div></div>");
  return s;
}

// ═══════════════════════════════════════════════════════════════════════════════
//  GLOBALS
// ═══════════════════════════════════════════════════════════════════════════════
ESP8266WebServer server(80);  // ← ESP8266WebServer instead of WebServer

struct RelayState {
  bool r[NUM_RELAYS];
} relay;

const uint8_t relayPins[NUM_RELAYS] = { RELAY1_PIN, RELAY2_PIN };
const char* relayLabels[NUM_RELAYS] = { R1_LABEL, R2_LABEL };
const char* relayIcons[NUM_RELAYS] = { R1_ICON, R2_ICON };

String sessionToken = "";

// ── Timers ────────────────────────────────────────────────────────────────────
unsigned long wifiRetryDelay = WIFI_RETRY_BASE_MS;
unsigned long wifiLastRetryTime = 0;
unsigned long ledToggleTime = 0;
bool ledState = false;

// ═══════════════════════════════════════════════════════════════════════════════
//  SESSION
//  ESP8266 has no esp_random(); use RANDOM_REG32 (hardware RNG register) instead
// ═══════════════════════════════════════════════════════════════════════════════
void generateSessionToken() {
  sessionToken = "";
  for (int i = 0; i < 8; i++) {
    char buf[9];
    // RANDOM_REG32 is the ESP8266 hardware RNG — equivalent to esp_random()
    snprintf(buf, sizeof(buf), "%08X", RANDOM_REG32);
    sessionToken += buf;
  }
  LOG_V("Session token: ", sessionToken.c_str());
}

bool isAuthenticated() {
  if (sessionToken.length() == 0) return false;
  if (!server.hasHeader(F("Cookie"))) return false;
  String cookies = server.header(F("Cookie"));
  String needle = String(SESSION_COOKIE_NAME) + "=" + sessionToken;
  return cookies.indexOf(needle) >= 0;
}

void requireAuth() {
  server.sendHeader(F("Location"), F("/login"));
  server.send(302, F("text/plain"), F("Redirecting to login..."));
}

// ═══════════════════════════════════════════════════════════════════════════════
//  EEPROM  (replaces Preferences/NVS — ESP8266 has no NVS)
// ═══════════════════════════════════════════════════════════════════════════════
void nvsave() {
  uint8_t mask = 0;
  for (int i = 0; i < NUM_RELAYS; i++)
    if (relay.r[i]) mask |= (1 << i);
  EEPROM.write(EEPROM_ADDR_MAGIC, EEPROM_MAGIC);
  EEPROM.write(EEPROM_ADDR_RELAYS, mask);
  EEPROM.commit();  // ← required on ESP8266 to actually write to flash
}

void nvload() {
  EEPROM.begin(EEPROM_SIZE);
  if (EEPROM.read(EEPROM_ADDR_MAGIC) == EEPROM_MAGIC) {
    uint8_t mask = EEPROM.read(EEPROM_ADDR_RELAYS);
    for (int i = 0; i < NUM_RELAYS; i++) relay.r[i] = (mask >> i) & 0x01;
    LOG_I("EEPROM relay states restored");
  } else {
    for (int i = 0; i < NUM_RELAYS; i++) relay.r[i] = false;
    nvsave();
    LOG_I("EEPROM first boot — all relays off");
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
//  RELAY HELPERS
// ═══════════════════════════════════════════════════════════════════════════════
void applyRelays() {
  for (int i = 0; i < NUM_RELAYS; i++)
    digitalWrite(relayPins[i], relay.r[i] ? RELAY_ON : RELAY_OFF);
}

void setRelay(int idx, bool st) {
  if (idx < 0 || idx >= NUM_RELAYS) return;
  relay.r[idx] = st;
  digitalWrite(relayPins[idx], st ? RELAY_ON : RELAY_OFF);
  nvsave();
}

// ═══════════════════════════════════════════════════════════════════════════════
//  JSON STATE
// ═══════════════════════════════════════════════════════════════════════════════
String buildStateJson() {
  StaticJsonDocument<192> doc;
  doc["relay1"] = relay.r[0];
  doc["relay2"] = relay.r[1];
  doc["rssi"] = WiFi.RSSI();
  doc["uptime"] = millis() / 1000;
  doc["ip"] = WiFi.localIP().toString();
  String out;
  serializeJson(doc, out);
  return out;
}

// ═══════════════════════════════════════════════════════════════════════════════
//  HTTP HANDLERS
// ═══════════════════════════════════════════════════════════════════════════════
void handleLoginPage() {
  server.send_P(200, "text/html", HTML_LOGIN);
}

void handleLoginPost() {
  String user = server.arg(F("username"));
  String pass = server.arg(F("password"));
  user.trim();
  pass.trim();
  if (user == UI_USERNAME && pass == UI_PASSWORD) {
    String cookie = String(SESSION_COOKIE_NAME) + "=" + sessionToken
                    + "; Max-Age=" + SESSION_MAX_AGE + "; Path=/; HttpOnly";
    server.sendHeader(F("Set-Cookie"), cookie);
    server.sendHeader(F("Location"), F("/"));
    server.send(302, F("text/plain"), F("OK"));
    LOG_I("Login success");
  } else {
    server.send(401, F("text/plain"), F("Unauthorized"));
    LOG_W("Login failed — bad credentials");
  }
}

void handleLogout() {
  String cookie = String(SESSION_COOKIE_NAME) + "=; Max-Age=0; Path=/; HttpOnly";
  server.sendHeader(F("Set-Cookie"), cookie);
  server.sendHeader(F("Location"), F("/login"));
  server.send(302, F("text/plain"), F("Logged out"));
  LOG_I("User logged out");
}

void handleRoot() {
  if (!isAuthenticated()) {
    requireAuth();
    return;
  }

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, F("text/html"), F(""));

  server.sendContent_P(HTML_A);
  server.sendContent(WiFi.localIP().toString());
  server.sendContent_P(HTML_B);

  int active = 0;
  for (int i = 0; i < NUM_RELAYS; i++)
    if (relay.r[i]) active++;
  server.sendContent(String(active));
  server.sendContent_P(HTML_C);

  unsigned long u = millis() / 1000;
  unsigned int h = u / 3600, m = (u % 3600) / 60, s = u % 60;
  String upStr;
  if (h > 0) {
    upStr += h;
    upStr += 'h';
    upStr += ' ';
  }
  if (m > 0) {
    upStr += m;
    upStr += 'm';
    upStr += ' ';
  }
  upStr += s;
  upStr += 's';
  server.sendContent(upStr);
  server.sendContent_P(HTML_D);

  server.sendContent(String(WiFi.RSSI()));
  server.sendContent_P(HTML_E);

  for (int i = 0; i < NUM_RELAYS; i++)
    server.sendContent(buildCard(i, relayLabels[i], relayIcons[i], relay.r[i]));

  server.sendContent_P(HTML_F);
  server.sendContent(F(""));
}

void handleState() {
  if (!isAuthenticated()) {
    server.send(401, F("application/json"), F("{\"error\":\"unauthorized\"}"));
    return;
  }
  server.sendHeader(F("Access-Control-Allow-Origin"), F("*"));
  server.send(200, F("application/json"), buildStateJson());
}

void handleToggle() {
  if (!isAuthenticated()) {
    server.send(401, F("application/json"), F("{\"error\":\"unauthorized\"}"));
    return;
  }
  if (!server.hasArg(F("relay"))) {
    server.send(400, F("application/json"), F("{\"error\":\"missing relay\"}"));
    return;
  }
  int idx = server.arg(F("relay")).toInt() - 1;
  if (idx < 0 || idx >= NUM_RELAYS) {
    server.send(400, F("application/json"), F("{\"error\":\"relay 1-2 only\"}"));
    return;
  }
  setRelay(idx, !relay.r[idx]);
  server.sendHeader(F("Access-Control-Allow-Origin"), F("*"));
  server.send(200, F("application/json"), buildStateJson());
}

void handleSet() {
  if (!isAuthenticated()) {
    server.send(401, F("application/json"), F("{\"error\":\"unauthorized\"}"));
    return;
  }
  if (!server.hasArg(F("relay")) || !server.hasArg(F("state"))) {
    server.send(400, F("application/json"), F("{\"error\":\"missing params\"}"));
    return;
  }
  int idx = server.arg(F("relay")).toInt() - 1;
  bool st = server.arg(F("state")).toInt() == 1;
  if (idx < 0 || idx >= NUM_RELAYS) {
    server.send(400, F("application/json"), F("{\"error\":\"relay 1-2 only\"}"));
    return;
  }
  setRelay(idx, st);
  server.sendHeader(F("Access-Control-Allow-Origin"), F("*"));
  server.send(200, F("application/json"), buildStateJson());
}

void handleNotFound() {
  if (!isAuthenticated()) {
    requireAuth();
    return;
  }
  server.send(404, F("text/plain"), F("404 Not Found"));
}

// ═══════════════════════════════════════════════════════════════════════════════
//  WIFI
// ═══════════════════════════════════════════════════════════════════════════════
static int wifiNetIdx = 0;

void wifiBegin() {
  WiFi.mode(WIFI_STA);
  WiFi.hostname(WIFI_HOSTNAME);  // ← ESP8266 uses WiFi.hostname(), not setHostname()
  WiFi.setAutoReconnect(true);

  Serial.println(F("[INFO] Scanning for known networks..."));

  for (int n = 0; n < WIFI_NETWORK_COUNT; n++) {
    Serial.print(F("[INFO] Trying SSID: "));
    Serial.println(WIFI_NETWORKS[n].ssid);

    WiFi.begin(WIFI_NETWORKS[n].ssid, WIFI_NETWORKS[n].password);

    unsigned long t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 8000) {
      delay(250);
      Serial.print('.');
      yield();  // ← ESP8266 requires yield() in tight loops to feed the watchdog
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
      wifiNetIdx = n;
      Serial.print(F("[INFO] Connected to: "));
      Serial.println(WIFI_NETWORKS[n].ssid);
      return;
    }

    WiFi.disconnect(true);
    delay(200);
    yield();
  }

  Serial.println(F("[WARN] No known network reachable — will retry in loop()"));
}

bool wifiCheck() {
  if (WiFi.status() == WL_CONNECTED) {
    wifiRetryDelay = WIFI_RETRY_BASE_MS;
    return true;
  }
  unsigned long now = millis();
  if (now - wifiLastRetryTime >= wifiRetryDelay) {
    wifiLastRetryTime = now;
    WiFi.disconnect(true);
    delay(100);
    yield();
    wifiNetIdx = (wifiNetIdx + 1) % WIFI_NETWORK_COUNT;
    WiFi.begin(WIFI_NETWORKS[wifiNetIdx].ssid, WIFI_NETWORKS[wifiNetIdx].password);
    Serial.print(F("[WARN] WiFi retry → "));
    Serial.println(WIFI_NETWORKS[wifiNetIdx].ssid);
    wifiRetryDelay = min(wifiRetryDelay * 2, (unsigned long)WIFI_RETRY_MAX_MS);
  }
  return false;
}

// ═══════════════════════════════════════════════════════════════════════════════
//  STATUS LED
//  NodeMCU built-in LED is active-LOW: HIGH = OFF, LOW = ON
//  We invert the ledState so the blink pattern feels the same as the ESP32 version
// ═══════════════════════════════════════════════════════════════════════════════
void updateLED() {
  unsigned long now = millis();
  bool wifiOK = (WiFi.status() == WL_CONNECTED);
  unsigned long period = wifiOK ? LED_SLOW_PERIOD_MS : LED_FAST_PERIOD_MS;
  if (now - ledToggleTime >= period) {
    ledToggleTime = now;
    ledState = !ledState;
    // Active-LOW: when ledState is true (LED "on"), write LOW; else HIGH
    digitalWrite(STATUS_LED, ledState ? LOW : HIGH);
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
//  OTA
// ═══════════════════════════════════════════════════════════════════════════════
void setupOTA() {
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    LOG_I("OTA start");
  });
  ArduinoOTA.onEnd([]() {
    LOG_I("OTA complete");
  });
  ArduinoOTA.onProgress([](unsigned int p, unsigned int t) {
    Serial.printf("[OTA]  %u%%\r", p / (t / 100));
  });
  ArduinoOTA.onError([](ota_error_t e) {
    const char* r[] = { "Auth", "Begin", "Connect", "Receive", "End" };
    Serial.printf("[OTA]  %s Failed\n", e < 5 ? r[e] : "Unknown");
  });
  ArduinoOTA.begin();
  LOG_I("OTA ready");
}

// ═══════════════════════════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(200);

  Serial.println(F("\n╔═════════════════════════════════════╗"));
  Serial.println(F("║  UICT Smart Home v3.3               ║"));
  Serial.println(F("║  NodeMCU ESP8266 12E  |  2 Relays   ║"));
  Serial.println(F("╚═════════════════════════════════════╝\n"));

  for (int i = 0; i < NUM_RELAYS; i++) {
    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], RELAY_OFF);
  }
  // Active-LOW LED: start with LED off → write HIGH
  pinMode(STATUS_LED, OUTPUT);
  digitalWrite(STATUS_LED, HIGH);

  nvload();
  applyRelays();
  generateSessionToken();

  wifiBegin();

  if (WiFi.status() == WL_CONNECTED) {
    LOG_I("WiFi connected");
    LOG_V("IP:   ", WiFi.localIP().toString().c_str());
    LOG_V("RSSI: ", WiFi.RSSI());
    Serial.println(F("\n  Open in browser:"));
    Serial.print(F("    http://"));
    Serial.println(WiFi.localIP());
    Serial.println(F("    http://smarthome.local/\n"));
    Serial.print(F("  Login: "));
    Serial.println(F(UI_USERNAME));
    Serial.print(F("  Pass:  "));
    Serial.println(F(UI_PASSWORD));
    setupOTA();
  } else {
    LOG_W("WiFi not ready — will retry in loop()");
  }

  // Collect Cookie header so isAuthenticated() can read it.
  // ESP8266WebServer 3.1.x uses a variadic template — pass each header
  // name as a separate String argument, NOT as an array + count.
  server.collectHeaders("Cookie");

  server.on(F("/login"), HTTP_GET, handleLoginPage);
  server.on(F("/login"), HTTP_POST, handleLoginPost);
  server.on(F("/logout"), HTTP_GET, handleLogout);
  server.on(F("/"), HTTP_GET, handleRoot);
  server.on(F("/state"), HTTP_GET, handleState);
  server.on(F("/toggle"), HTTP_POST, handleToggle);
  server.on(F("/set"), HTTP_POST, handleSet);
  server.onNotFound(handleNotFound);
  server.begin();
  LOG_I("HTTP server started on port 80");
}

// ═══════════════════════════════════════════════════════════════════════════════
//  LOOP
// ═══════════════════════════════════════════════════════════════════════════════
void loop() {
  ArduinoOTA.handle();
  updateLED();
  wifiCheck();
  server.handleClient();
  yield();  // ← keeps the ESP8266 watchdog happy
}
