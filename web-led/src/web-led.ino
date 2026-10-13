/*
 * web-led —— 网页控制台灯（ESP32-S3-DevKitC-1 N16R8，板载 WS2812 全彩灯）
 *
 * 功能：
 *   1. 局域网网页开关 LED
 *   2. 网页调光 0~100%
 *   3. 板载 BOOT 键也能切换开关
 *   4. 网页界面：品牌色（暖橙 #C2410C / 米白 #FAF8F4 / 暗棕 #1A1715）+ 日/夜间模式
 *
 * 板载灯：WS2812 全彩灯，接在 GPIO 21（亮起的颜色 = 品牌暖橙，见 BRAND_* 配置）。
 * WiFi 名密码在 secrets.h（本地凭据，不进仓库）。需要 Adafruit NeoPixel 库（见 README）。
 */

#include <WiFi.h>
#include <Adafruit_NeoPixel.h>
#include "secrets.h"          // WiFi 名密码（本地凭据，不进仓库）

// ================= 配置区（先改这里） =================
const int LED_PIN = 21;      // 板载 WS2812 全彩灯，GPIO 21
const int LED_COUNT = 1;     // 板载只有 1 颗灯

// 亮灯颜色：烤鹅暖橙 #C2410C
const uint8_t BRAND_R = 194;
const uint8_t BRAND_G = 65;
const uint8_t BRAND_B = 12;

const int BOOT_BUTTON_PIN = 0;   // 板载 BOOT 键（GPIO 0），按下切换 LED
// =====================================================

const int BOOT_DEBOUNCE_MS = 500;

bool ledOn = false;
int brightness = 100;        // 0~100%

Adafruit_NeoPixel pixel(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

WiFiServer server(80);

// 网页（品牌色 + 日/夜间模式）
// 注：本工具链不支持带定界符的原始字符串 R"XXX=( ... )XXX"，必须用无定界符写法 R"( ... )"。
const char PAGE[] = R"(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Siungo 台灯</title>
<style>
:root{
  --orange:#C2410C; --cream:#FAF8F4; --dark:#1A1715;
  --bg:var(--cream); --fg:var(--dark);
}
html.night{ --bg:var(--dark); --fg:var(--cream); }
body{ margin:0; font-family:"Noto Sans SC",system-ui,sans-serif; background:var(--bg);
      color:var(--fg); display:flex; align-items:center; justify-content:center;
      min-height:100vh; transition:background .3s,color .3s; }
.card{ text-align:center; }
h1{ font-family:"Noto Serif SC",serif; font-weight:600; margin:0 0 1.2em; }
.btn{ background:var(--orange); color:#fff; border:none; border-radius:999px;
      padding:.9em 2.6em; font-size:1.15rem; cursor:pointer; font-family:inherit;
      box-shadow:0 4px 14px rgba(194,65,12,.35); transition:transform .15s; }
.btn:active{ transform:scale(.96); }
.btn.off{ background:transparent; color:var(--fg); border:2px solid var(--fg); box-shadow:none; }
input[type=range]{ width:min(70vw,320px); accent-color:var(--orange); margin-top:1.6em; }
.pct{ font-size:1.3rem; margin-top:.5em; }
.mode{ position:absolute; top:1em; right:1em; background:none; border:1px solid var(--fg);
       color:var(--fg); border-radius:999px; padding:.4em 1.1em; cursor:pointer; font-family:inherit; }
</style>
</head>
<body>
<button class="mode" id="modeBtn">🌙 夜间</button>
<div class="card">
  <h1>Siungo 台灯</h1>
  <button class="btn off" id="powerBtn">开灯</button>
  <div class="pct" id="pct">100%</div>
  <input type="range" id="dim" min="0" max="100" value="100">
</div>
<script>
const $=id=>document.getElementById(id);
async function refresh(){
  const r=await fetch('/state'); const t=await r.text();   // "1,100"
  const [on,b]=t.split(',').map(Number);
  $('powerBtn').textContent = on ? '关灯' : '开灯';
  $('powerBtn').classList.toggle('off', !on);
  $('pct').textContent = b + '%'; $('dim').value = b;
}
$('powerBtn').onclick = async () => {
  await fetch($('powerBtn').classList.contains('off') ? '/on' : '/off');
  refresh();
};
$('dim').oninput = e => { fetch('/dim?v=' + e.target.value); $('pct').textContent = e.target.value + '%'; };
$('modeBtn').onclick = () => {
  document.documentElement.classList.toggle('night');
  const n = document.documentElement.classList.contains('night');
  $('modeBtn').textContent = n ? '☀️ 日间' : '🌙 夜间';
  localStorage.setItem('night', n ? '1' : '0');
};
if (localStorage.getItem('night') === '1') {
  document.documentElement.classList.add('night');
  $('modeBtn').textContent = '☀️ 日间';
}
refresh();
</script>
</body>
</html>
)";

void applyLed() {
  if (ledOn) {
    pixel.setBrightness((uint8_t)map(brightness, 0, 100, 0, 255));
    pixel.setPixelColor(0, BRAND_R, BRAND_G, BRAND_B);
  } else {
    pixel.setPixelColor(0, 0, 0, 0);
  }
  pixel.show();
}

void setup() {
  Serial.begin(115200);
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  pixel.begin();
  applyLed();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("连接 WiFi");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print('.'); }
  Serial.println();
  Serial.print("已连接！浏览器打开: http://");
  Serial.println(WiFi.localIP());
  server.begin();
}

unsigned long lastBtnPress = 0;

void loop() {
  // 板载 BOOT 键：按下切换开关（消抖）
  if (digitalRead(BOOT_BUTTON_PIN) == LOW && millis() - lastBtnPress > BOOT_DEBOUNCE_MS) {
    lastBtnPress = millis();
    ledOn = !ledOn;
    applyLed();
  }

  WiFiClient client = server.available();
  if (!client) return;

  String req = client.readStringUntil('\r');
  client.flush();

  if (req.indexOf("GET /on") >= 0) {
    ledOn = true;
  } else if (req.indexOf("GET /off") >= 0) {
    ledOn = false;
  } else if (req.indexOf("GET /dim") >= 0) {
    int i = req.indexOf("v=");
    if (i >= 0) brightness = constrain(req.substring(i + 2).toInt(), 0, 100);
  }
  applyLed();

  if (req.indexOf("GET /state") >= 0) {
    client.print("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n");
    client.print(String(ledOn ? 1 : 0) + "," + String(brightness));
  } else if (req.indexOf("GET / ") >= 0) {
    client.print("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n\r\n");
    client.print(PAGE);
  } else {
    client.print("HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n");
  }
  client.stop();
}
