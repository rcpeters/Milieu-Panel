// Milieu Panel - Matter-over-Wi-Fi Multi-Button RGB Controller (1-8 Buttons)
// Target Board: Seeed Studio XIAO ESP32C6 (Arduino-ESP32 v3.x+)
// Partition Scheme: Huge APP (3MB No OTA/1MB SPIFFS)

#include <Arduino.h>
#include <DFRobot_RGBButton.h>
#include <DNSServer.h>
#include <Matter.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <Wire.h>
#include <esp_netif.h>
#include <esp_wifi.h>

#include "config.h"

static ButtonSlot g_buttons[MAX_BUTTONS];
static uint8_t g_buttonCount = 0;
static uint8_t g_buttonMask = 0;
static bool g_nvsCommissioned = false;

static DNSServer g_dnsServer;
static WebServer g_webServer(SETUP_HTTP_PORT);
static bool g_portalActive = false;
static String g_qrPayload;
static String g_manualCode;

static portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t g_lastPollMs = 0;
static uint32_t g_bootHoldStartMs = 0;
static bool g_bootHeld = false;

// -----------------------------------------------------------------------------
// I2C Helpers (Direct register access to DFR0991 at 100 kHz)
// -----------------------------------------------------------------------------

static bool probeButton(uint8_t addr) {
  Wire.beginTransmission(addr);
  Wire.write(RGBBUTTON_PID_MSB_REG);
  if (Wire.endTransmission() != 0 || Wire.requestFrom(addr, (size_t)2) != 2) {
    return false;
  }
  uint16_t pid = ((uint16_t)Wire.read() << 8) | Wire.read();
  return (pid != 0x0000 && pid != 0xFFFF);
}

static bool setButtonRgb(uint8_t addr, uint8_t r, uint8_t g, uint8_t b) {
  Wire.beginTransmission(addr);
  Wire.write(RGBBUTTON_RED_REG);
  Wire.write(r);
  Wire.write(g);
  Wire.write(b);
  return (Wire.endTransmission() == 0);
}

static bool readButtonPressed(uint8_t addr, bool &pressed) {
  Wire.beginTransmission(addr);
  Wire.write(RGBBUTTON_BUTTON_SIGNAL_REG);
  if (Wire.endTransmission() != 0 || Wire.requestFrom(addr, (size_t)1) != 1) {
    return false;
  }
  pressed = (Wire.read() != 0);
  return true;
}

static uint8_t scanButtons() {
  uint8_t mask = 0;
  for (uint8_t i = 0; i < MAX_BUTTONS; ++i) {
    if (probeButton(BUTTON_ADDR_MIN + i)) {
      mask |= (1U << i);
    }
    delay(5);
  }
  return mask;
}

static uint8_t scalePwm(uint8_t val, uint8_t maxPwm) {
  return (uint8_t)(((uint16_t)val * maxPwm + 127U) / 255U);
}

// -----------------------------------------------------------------------------
// NVS & Factory Reset (5 s BOOT Button Hold)
// -----------------------------------------------------------------------------

static void saveNvs(uint8_t mask, bool commissioned) {
  Preferences prefs;
  if (prefs.begin(NVS_NAMESPACE, false)) {
    prefs.putUChar(NVS_KEY_BTN_MASK, mask);
    prefs.putBool(NVS_KEY_COMMISSIONED, commissioned);
    prefs.end();
  }
  g_nvsCommissioned = commissioned;
}

static void checkFactoryReset(uint32_t now) {
  if (digitalRead(PIN_BOOT_BUTTON) == LOW) {
    if (!g_bootHeld) {
      g_bootHeld = true;
      g_bootHoldStartMs = now;
    } else if ((now - g_bootHoldStartMs) >= FACTORY_RESET_HOLD_MS) {
      Serial.println("\n[RESET] Factory reset triggered -> Flashing Purple 3x...");
      for (uint8_t c = 0; c < FACTORY_RESET_BLINK_COUNT; ++c) {
        for (uint8_t a = BUTTON_ADDR_MIN; a <= BUTTON_ADDR_MAX; ++a) {
          setButtonRgb(a, LED_PWM_ON, 0, LED_PWM_ON);
        }
        delay(FACTORY_RESET_BLINK_MS);
        for (uint8_t a = BUTTON_ADDR_MIN; a <= BUTTON_ADDR_MAX; ++a) {
          setButtonRgb(a, 0, 0, 0);
        }
        delay(FACTORY_RESET_BLINK_MS);
      }
      Preferences prefs;
      if (prefs.begin(NVS_NAMESPACE, false)) {
        prefs.clear();
        prefs.end();
      }
      Matter.decommission();
      ESP.restart();
    }
  } else {
    g_bootHeld = false;
  }
}

// -----------------------------------------------------------------------------
// MatterColorLight Callbacks
// -----------------------------------------------------------------------------

static bool onLightChange(uint8_t idx, bool state, espHsvColor_t hsv) {
  if (idx >= g_buttonCount) {
    return true;
  }
  espRgbColor_t rgb = espHsvColorToRgbColor({hsv.h, hsv.s, 254});
  portENTER_CRITICAL(&g_mux);
  g_buttons[idx].haSynced = true;
  g_buttons[idx].isOn = state;
  g_buttons[idx].rgb = rgb;
  portEXIT_CRITICAL(&g_mux);
  return true;
}

using LightCb = bool (*)(bool, espHsvColor_t);
static const LightCb kLightCbs[MAX_BUTTONS] = {
    [](bool s, espHsvColor_t c) { return onLightChange(0, s, c); },
    [](bool s, espHsvColor_t c) { return onLightChange(1, s, c); },
    [](bool s, espHsvColor_t c) { return onLightChange(2, s, c); },
    [](bool s, espHsvColor_t c) { return onLightChange(3, s, c); },
    [](bool s, espHsvColor_t c) { return onLightChange(4, s, c); },
    [](bool s, espHsvColor_t c) { return onLightChange(5, s, c); },
    [](bool s, espHsvColor_t c) { return onLightChange(6, s, c); },
    [](bool s, espHsvColor_t c) { return onLightChange(7, s, c); },
};

static constexpr const char *kButtonLabels[MAX_BUTTONS] = {
    "Button 1", "Button 2", "Button 3", "Button 4",
    "Button 5", "Button 6", "Button 7", "Button 8",
};

static constexpr const char *kLightLabels[MAX_BUTTONS] = {
    "Button 1 Light", "Button 2 Light", "Button 3 Light", "Button 4 Light",
    "Button 5 Light", "Button 6 Light", "Button 7 Light", "Button 8 Light",
};

static void setEndpointLabel(MatterEndPoint &ep, const char *label) {
  esp_matter::endpoint_t *endpoint = esp_matter::endpoint::get(esp_matter::node::get(), ep.getEndPointId());
  if (!endpoint) {
    return;
  }
  esp_matter::endpoint::bridged_node::config_t cfg;
  if (esp_matter::endpoint::bridged_node::add(endpoint, &cfg) == ESP_OK) {
    esp_matter::cluster_t *cluster =
        esp_matter::cluster::get(endpoint, chip::app::Clusters::BridgedDeviceBasicInformation::Id);
    if (cluster) {
      char buf[32];
      strncpy(buf, label, sizeof(buf) - 1);
      buf[sizeof(buf) - 1] = '\0';
      esp_matter::cluster::bridged_device_basic_information::attribute::create_node_label(cluster, buf, strlen(buf));
    }
  }
}

// -----------------------------------------------------------------------------
// Temporary Commissioning SoftAP & Offline QR Code Portal (192.168.4.1)
// -----------------------------------------------------------------------------

static const char kPortalHtmlHead[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Milieu Panel Setup</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;background:#0f172a;color:#f8fafc;display:flex;justify-content:center;padding:24px 16px}
.card{width:100%;max-width:400px;background:#1e293b;border:1px solid #334155;border-radius:16px;padding:24px;text-align:center;box-shadow:0 10px 25px rgba(0,0,0,.4)}
h1{font-size:1.35rem;font-weight:700;margin-bottom:4px}
.sub{font-size:.875rem;color:#94a3b8;margin-bottom:20px}
.qr-wrap{width:232px;height:232px;margin:0 auto 12px;background:#fff;padding:12px;border-radius:12px;display:flex;align-items:center;justify-content:center}
.qr-wrap svg{width:100%;height:100%;display:block}
.payload{font-family:monospace;font-size:.8rem;color:#94a3b8;margin-bottom:18px;word-break:break-all}
.label{font-size:.75rem;text-transform:uppercase;letter-spacing:.06em;color:#94a3b8;margin-bottom:6px}
.code{font-family:monospace;font-size:1.35rem;font-weight:700;letter-spacing:.08em;background:#0f172a;border:1px solid #334155;border-radius:10px;padding:12px;margin-bottom:10px;color:#38bdf8}
button{width:100%;background:#0284c7;color:#fff;border:none;border-radius:10px;padding:11px 16px;font-size:.95rem;font-weight:600;cursor:pointer;margin-bottom:18px}
button:active{background:#0369a1}
.meta{font-size:.82rem;color:#cbd5e1;background:#0f172a;border-radius:10px;padding:12px;text-align:left;line-height:1.5}
.meta strong{color:#f8fafc}
</style>
</head>
<body>
<div class="card">
  <h1>Milieu Panel</h1>
  <p class="sub">Matter-over-Wi-Fi Commissioning</p>
  <div class="qr-wrap" id="qr"></div>
  <div class="payload" id="mt"></div>
  <div class="label">Manual Pairing Code</div>
  <div class="code" id="mc"></div>
  <button type="button" id="cp" onclick="copyCode()">Copy Manual Code</button>
  <div class="meta" id="hw"></div>
</div>
<script>
function makeQrSvg(text){
  const bytes=Array.from(text,c=>c.charCodeAt(0));
  const specs=[[1,16,10,0],[2,28,16,0],[3,44,26,0],[4,80,20,1]];
  const [ver,dataLen,ecLen,ecBits]=specs.find(s=>bytes.length+2<=s[1])||specs[3];
  const size=ver*4+17,bits=[];
  const push=(v,n)=>{for(let i=n-1;i>=0;i--)bits.push((v>>i)&1);};
  push(4,4);push(bytes.length,8);bytes.forEach(b=>push(b,8));
  for(let i=0;i<4&&bits.length<dataLen*8;i++)bits.push(0);
  while(bits.length%8)bits.push(0);
  const data=[];
  for(let i=0;i<bits.length;i+=8){let b=0;for(let j=0;j<8;j++)b=(b<<1)|bits[i+j];data.push(b);}
  for(let p=0;data.length<dataLen;p^=1)data.push(p?0x11:0xEC);
  const exp=new Uint8Array(512),log=new Uint8Array(256);
  for(let i=0,x=1;i<255;i++){exp[i]=exp[i+255]=x;log[x]=i;x=(x<<1)^(x&128?0x11D:0);}
  const mul=(a,b)=>(a&&b?exp[log[a]+log[b]]:0);
  let gen=[1];
  for(let i=0;i<ecLen;i++){
    const nxt=new Array(gen.length+1).fill(0);
    for(let j=0;j<gen.length;j++){nxt[j]^=gen[j];nxt[j+1]^=mul(gen[j],exp[i]);}
    gen=nxt;
  }
  const ec=new Array(ecLen).fill(0);
  for(const b of data){const f=b^ec.shift();ec.push(0);for(let j=0;j<ecLen;j++)ec[j]^=mul(gen[j+1],f);}
  const cw=data.concat(ec);
  const grid=Array.from({length:size},()=>new Uint8Array(size));
  const used=Array.from({length:size},()=>new Uint8Array(size));
  const set=(r,c,v)=>{if(r>=0&&r<size&&c>=0&&c<size){grid[r][c]=v?1:0;used[r][c]=1;}};
  const finder=(r0,c0)=>{
    for(let dr=-4;dr<=4;dr++)for(let dc=-4;dc<=4;dc++){
      const d=Math.max(Math.abs(dr),Math.abs(dc));
      set(r0+dr,c0+dc,d<=1||d===3);
    }
  };
  finder(3,3);finder(3,size-4);finder(size-4,3);
  if(ver>=2){
    for(let dr=-2;dr<=2;dr++)for(let dc=-2;dc<=2;dc++)
      set(size-7+dr,size-7+dc,Math.max(Math.abs(dr),Math.abs(dc))!==1);
  }
  for(let i=8;i<size-8;i++){set(6,i,i%2===0);set(i,6,i%2===0);}
  for(let i=0;i<9;i++){used[8][i]=used[i][8]=1;if(i<8)used[8][size-1-i]=used[size-1-i][8]=1;}
  set(size-8,8,1);
  let bitIdx=0,up=true;
  for(let right=size-1;right>=1;right-=2){
    if(right===6)right=5;
    for(let v=0;v<size;v++){
      const r=up?size-1-v:v;
      for(let j=0;j<2;j++){
        const c=right-j;
        if(!used[r][c]){
          const bit=bitIdx<cw.length*8?(cw[bitIdx>>3]>>(7-(bitIdx&7)))&1:0;
          bitIdx++;
          grid[r][c]=bit^((r+c)%2===0?1:0);
        }
      }
    }
    up=!up;
  }
  let rem=(ecBits<<3)|0,fmt=rem<<10;
  for(let i=14;i>=10;i--)if((fmt>>i)&1)fmt^=0x537<<(i-10);
  fmt=((rem<<10)|fmt)^0x5412;
  const rP=[8,8,8,8,8,8,8,8,7,5,4,3,2,1,0],cP=[0,1,2,3,4,5,7,8,8,8,8,8,8,8,8];
  for(let i=0;i<15;i++){
    const b=(fmt>>(14-i))&1;
    grid[rP[i]][cP[i]]=b;
    if(i<7)grid[size-1-i][8]=b;else grid[8][size-15+i]=b;
  }
  let path="";
  for(let r=0;r<size;r++)for(let c=0;c<size;c++)if(grid[r][c])path+=`M${c+4},${r+4}h1v1h-1z`;
  const dim=size+8;
  return `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${dim} ${dim}" shape-rendering="crispEdges"><rect width="100%" height="100%" fill="#fff"/><path d="${path}" fill="#000"/></svg>`;
}
)rawliteral";

static String extractQrPayload(const String &qrUrl) {
  int idx = qrUrl.indexOf("data=");
  String raw = (idx >= 0) ? qrUrl.substring(idx + 5) : qrUrl;
  int amp = raw.indexOf('&');
  if (amp >= 0) {
    raw = raw.substring(0, amp);
  }
  String out;
  out.reserve(raw.length());
  for (size_t i = 0; i < raw.length(); ++i) {
    if (raw[i] == '%' && i + 2 < raw.length()) {
      char hex[3] = {raw[i + 1], raw[i + 2], '\0'};
      out += (char)strtol(hex, nullptr, 16);
      i += 2;
    } else {
      out += raw[i];
    }
  }
  return out;
}

static void handlePortalRoot() {
  String page;
  page.reserve(4800);
  page += FPSTR(kPortalHtmlHead);
  page += "const MT=\"";
  page += g_qrPayload;
  page += "\",MC=\"";
  page += g_manualCode;
  page += "\";\n";
  page += "document.getElementById('qr').innerHTML=makeQrSvg(MT);\n";
  page += "document.getElementById('mt').textContent=MT;\n";
  page += "document.getElementById('mc').textContent=MC;\n";
  page += "document.getElementById('hw').innerHTML='<strong>Discovered Buttons (";
  page += String(g_buttonCount);
  page += "):</strong> ";
  for (uint8_t i = 0; i < g_buttonCount; ++i) {
    char addrBuf[16];
    snprintf(addrBuf, sizeof(addrBuf), "#%u (0x%02X)%s",
             g_buttons[i].number, g_buttons[i].addr,
             (i + 1 < g_buttonCount) ? ", " : "");
    page += addrBuf;
  }
  page += "<br><br><strong>To pair:</strong> Scan the QR code above from the Home Assistant app, or copy the Manual Code, switch back to your home Wi-Fi, and paste it in Home Assistant.';\n";
  page += R"rawliteral(function copyCode(){
  const btn=document.getElementById('cp');
  const ta=document.createElement('textarea');
  ta.value=MC;ta.style.position='fixed';ta.style.opacity='0';
  document.body.appendChild(ta);ta.select();
  try{document.execCommand('copy');btn.textContent='Copied!';}catch(e){}
  document.body.removeChild(ta);
  setTimeout(()=>{btn.textContent='Copy Manual Code';},2000);
}
</script></body></html>)rawliteral";
  g_webServer.send(200, "text/html", page);
}

static void startSetupPortal() {
  g_manualCode = Matter.getManualPairingCode();
  g_qrPayload = extractQrPayload(Matter.getOnboardingQRCodeUrl());

  if (esp_netif_get_handle_from_ifkey("WIFI_AP_DEF") == nullptr) {
    esp_netif_create_default_wifi_ap();
  }
  esp_wifi_set_mode(WIFI_MODE_APSTA);

  wifi_config_t apCfg = {};
  strncpy(reinterpret_cast<char *>(apCfg.ap.ssid), SETUP_AP_SSID, sizeof(apCfg.ap.ssid) - 1);
  apCfg.ap.ssid_len = strlen(SETUP_AP_SSID);
  apCfg.ap.channel = 1;
  apCfg.ap.authmode = WIFI_AUTH_OPEN;
  apCfg.ap.max_connection = 4;
  apCfg.ap.beacon_interval = 100;
  esp_wifi_set_config(WIFI_IF_AP, &apCfg);

  IPAddress apIp(192, 168, 4, 1);
  g_dnsServer.start(SETUP_DNS_PORT, "*", apIp);

  g_webServer.on("/", handlePortalRoot);
  g_webServer.onNotFound([]() {
    g_webServer.sendHeader("Location", "http://192.168.4.1/", true);
    g_webServer.send(302, "text/plain", "");
  });
  g_webServer.begin();
  g_portalActive = true;

  Serial.printf("[PORTAL] Temporary Wi-Fi AP \"%s\" active at http://192.168.4.1\n", SETUP_AP_SSID);
  Serial.printf("[PORTAL] QR Payload: %s\n", g_qrPayload.c_str());
}

static void stopSetupPortal() {
  if (!g_portalActive) {
    return;
  }
  g_dnsServer.stop();
  g_webServer.stop();
  esp_wifi_set_mode(WIFI_MODE_STA);
  g_portalActive = false;
  Serial.println("[PORTAL] Commissioning complete -> Stopped temporary Wi-Fi AP.");
}

// -----------------------------------------------------------------------------
// Button Polling & LED State Machine
// -----------------------------------------------------------------------------

static NodeState getNodeState() {
  if (!Matter.isDeviceCommissioned()) {
    return NodeState::UNCOMMISSIONED;
  }
  if (!Matter.isWiFiConnected() || !Matter.isDeviceConnected()) {
    return NodeState::OFFLINE;
  }
  return NodeState::ONLINE;
}

static void disconnectSlot(ButtonSlot &b, uint32_t now) {
  if (b.connected) {
    b.connected = false;
    b.lastRetryMs = now;
    b.rawPressed = b.pressed = b.longFired = b.hasColor = false;
    Serial.printf("[I2C] Button %u (0x%02X) disconnected; retrying every 5 s.\n", b.number, b.addr);
  }
}

static void updateButtons(NodeState state, uint32_t now) {
  for (uint8_t i = 0; i < g_buttonCount; ++i) {
    ButtonSlot &b = g_buttons[i];
    if (!b.connected && (now - b.lastRetryMs) >= I2C_RETRY_INTERVAL_MS) {
      b.lastRetryMs = now;
      if (probeButton(b.addr)) {
        b.connected = true;
        b.hasColor = false;
        Serial.printf("[I2C] Button %u (0x%02X) reconnected.\n", b.number, b.addr);
      }
    }
  }

  if ((now - g_lastPollMs) < BUTTON_POLL_INTERVAL_MS) {
    return;
  }
  g_lastPollMs = now;

  if (state != NodeState::ONLINE) {
    for (uint8_t i = 0; i < g_buttonCount; ++i) {
      g_buttons[i].rawPressed = g_buttons[i].pressed = g_buttons[i].longFired = false;
    }
    return;
  }

  for (uint8_t i = 0; i < g_buttonCount; ++i) {
    ButtonSlot &b = g_buttons[i];
    if (!b.connected) {
      continue;
    }

    bool raw = false;
    if (!readButtonPressed(b.addr, raw)) {
      disconnectSlot(b, now);
      continue;
    }

    if (raw != b.rawPressed) {
      b.rawPressed = raw;
      b.debounceMs = now;
    }

    if ((now - b.debounceMs) >= BUTTON_DEBOUNCE_MS && b.pressed != b.rawPressed) {
      b.pressed = b.rawPressed;
      if (b.pressed) {
        b.pressStartMs = now;
        b.longFired = false;
        b.sw.press();
        Serial.printf("[BTN] Button %u (0x%02X) -> InitialPress\n", b.number, b.addr);
      } else if (b.longFired || (now - b.pressStartMs) >= BUTTON_LONG_PRESS_MS) {
        if (!b.longFired) {
          b.sw.longPress();
        }
        b.sw.longRelease();
        Serial.printf("[BTN] Button %u (0x%02X) -> LongRelease (%lu ms)\n",
                      b.number, b.addr, (unsigned long)(now - b.pressStartMs));
      } else {
        b.sw.release();
        Serial.printf("[BTN] Button %u (0x%02X) -> ShortRelease (%lu ms)\n",
                      b.number, b.addr, (unsigned long)(now - b.pressStartMs));
      }
    }

    if (b.pressed && !b.longFired && (now - b.pressStartMs) >= BUTTON_LONG_PRESS_MS) {
      b.longFired = true;
      b.sw.longPress();
      Serial.printf("[BTN] Button %u (0x%02X) -> LongPress\n", b.number, b.addr);
    }
  }
}

static void updateLeds(NodeState state, uint32_t now) {
  bool blinkOn = ((now / STATUS_BLINK_INTERVAL_MS) % 2) == 0;

  for (uint8_t i = 0; i < g_buttonCount; ++i) {
    ButtonSlot &b = g_buttons[i];
    if (!b.connected) {
      continue;
    }

    uint8_t r = 0, g = 0, bl = 0;
    if (state == NodeState::UNCOMMISSIONED) {
      bl = blinkOn ? LED_PWM_ON : LED_PWM_OFF;
    } else if (state == NodeState::OFFLINE) {
      r = blinkOn ? LED_PWM_ON : LED_PWM_OFF;
    } else {
      portENTER_CRITICAL(&g_mux);
      bool synced = b.haSynced;
      bool on = b.isOn;
      espRgbColor_t c = b.rgb;
      portEXIT_CRITICAL(&g_mux);

      if (synced && on) {
        r = scalePwm(c.r, LED_PWM_ON);
        g = scalePwm(c.g, LED_PWM_ON);
        bl = scalePwm(c.b, LED_PWM_ON);
      } else if (!synced) {
        r = g = bl = LED_PWM_ON; // White (10% waiting for HA sync)
      }
    }

    if (!b.hasColor || b.lastR != r || b.lastG != g || b.lastB != bl) {
      if (setButtonRgb(b.addr, r, g, bl)) {
        b.lastR = r;
        b.lastG = g;
        b.lastB = bl;
        b.hasColor = true;
      } else {
        disconnectSlot(b, now);
      }
    }
  }
}

// -----------------------------------------------------------------------------
// Setup & Loop
// -----------------------------------------------------------------------------

void setup() {
  pinMode(PIN_RF_SWITCH_EN, OUTPUT);
  digitalWrite(PIN_RF_SWITCH_EN, LOW);
  pinMode(PIN_RF_ANT_SELECT, OUTPUT);
  digitalWrite(PIN_RF_ANT_SELECT, USE_EXTERNAL_ANTENNA ? HIGH : LOW);
  pinMode(PIN_BOOT_BUTTON, INPUT_PULLUP);

  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < 2000) {
    delay(10);
  }

  delay(150);
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_HZ);
  Wire.setTimeOut(50);
  delay(50);

  Preferences prefs;
  if (prefs.begin(NVS_NAMESPACE, true)) {
    g_buttonMask = prefs.getUChar(NVS_KEY_BTN_MASK, 0);
    g_nvsCommissioned = prefs.getBool(NVS_KEY_COMMISSIONED, false);
    prefs.end();
  }

  if (!g_nvsCommissioned || g_buttonMask == 0) {
    Serial.println("[BOOT] Scanning I2C bus (0x23-0x2A) for RGB buttons...");
    while ((g_buttonMask = scanButtons()) == 0) {
      Serial.println("[I2C] No buttons found; retrying in 5 s...");
      uint32_t waitStart = millis();
      while ((millis() - waitStart) < I2C_RETRY_INTERVAL_MS) {
        checkFactoryReset(millis());
        delay(20);
      }
    }
    saveNvs(g_buttonMask, false);
  }

  uint32_t switchFlags = MatterGenericSwitch::FEATURE_MOMENTARY |
                         MatterGenericSwitch::FEATURE_RELEASE |
                         MatterGenericSwitch::FEATURE_LONG_PRESS;

  // Pass 1: Register all Switches first so their Endpoint IDs are 1..N
  for (uint8_t bit = 0; bit < MAX_BUTTONS; ++bit) {
    if (!(g_buttonMask & (1U << bit))) {
      continue;
    }
    uint8_t idx = g_buttonCount++;
    ButtonSlot &b = g_buttons[idx];
    b.number = idx + 1;
    b.addr = BUTTON_ADDR_MIN + bit;
    b.connected = probeButton(b.addr);
    b.lastRetryMs = millis();

    b.sw.begin(switchFlags);
    b.sw.setTagList({{MatterTags::Number::NS, b.number},
                     MatterTags::Switches::createCustomTag(kButtonLabels[idx])});
    setEndpointLabel(b.sw, kButtonLabels[idx]);
  }

  // Pass 2: Register all Lights second so their Endpoint IDs are (N+1)..2N
  for (uint8_t idx = 0; idx < g_buttonCount; ++idx) {
    ButtonSlot &b = g_buttons[idx];
    b.light.begin(false, {0, 0, 254});
    b.light.setTagList({{MatterTags::Number::NS, b.number},
                        MatterTags::Switches::createCustomTag(kLightLabels[idx])});
    b.light.onChange(kLightCbs[idx]);
    setEndpointLabel(b.light, kLightLabels[idx]);

    Serial.printf("[BOOT] Button %u @ 0x%02X (%s) -> Switch EP %u, Light EP %u\n",
                  b.number, b.addr, b.connected ? "OK" : "MISSING",
                  b.sw.getEndPointId(), b.light.getEndPointId());
  }

  Matter.setVendorName("Make Rob Make");
  Matter.setProductName("Milieu Panel");
  Matter.setDeviceName("Milieu Panel");
  Matter.begin();

  if (g_nvsCommissioned && !Matter.isDeviceCommissioned()) {
    saveNvs(0, false);
    ESP.restart();
  }

  if (!Matter.isDeviceCommissioned()) {
    Serial.printf("\n=== Pairing Mode (Buttons: %u, Mask: 0x%02X) ===\n", g_buttonCount, g_buttonMask);
    Serial.printf("Manual Code: %s\n", Matter.getManualPairingCode().c_str());
    Serial.printf("QR Code URL: %s\n", Matter.getOnboardingQRCodeUrl().c_str());
    startSetupPortal();
    Serial.println();
  }
}

void loop() {
  uint32_t now = millis();
  checkFactoryReset(now);

  if (!g_nvsCommissioned && Matter.isDeviceCommissioned()) {
    stopSetupPortal();
    saveNvs(g_buttonMask, true);
    Serial.printf("[MATTER] Commissioned! Locked button mask 0x%02X in NVS.\n", g_buttonMask);
  }

  if (g_portalActive) {
    g_webServer.handleClient();
  }

  NodeState state = getNodeState();
  updateButtons(state, now);
  updateLeds(state, now);
  delay(2);
}
