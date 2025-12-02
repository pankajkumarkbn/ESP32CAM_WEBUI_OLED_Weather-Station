#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <U8g2lib.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BMP280.h>
#include <DHT.h>

#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>  // synchronous HTTP server, lighter than AsyncWebServer

// ================= DISPLAY (SPI) =================
// OLED wired: CLK=1, MOSI=2, CS=15, DC=13, RST=12
U8G2_SSD1306_128X64_NONAME_F_4W_SW_SPI u8g2(
  U8G2_R0,
  /* clock=*/ 1,
  /* data=*/ 2,
  /* cs=*/ 15,
  /* dc=*/ 13,
  /* reset=*/ 12
);

// ================= SENSORS =======================
// DHT11 on GPIO14
#define DHTPIN  14
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

// BMP280 on I2C using custom pins SDA=0, SCL=16
#define I2C_SDA 0
#define I2C_SCL 16
Adafruit_BMP280 bmp;   // I2C

// ================= WIFI / MDNS ===================
const char* ssid     = "YOUR_SSID";
const char* password = "YOUR_PASSWORD";

WebServer server(80);

// ================= FORECAST STATE ================
float lastPressure = 1013.25;
unsigned long lastPressureSample = 0;
const unsigned long PRESSURE_SAMPLE_INTERVAL = 10UL * 60UL * 1000UL; // 10 min
const float SEA_LEVEL_HPA = 1013.25;

float pressureTrend = 0.0;
const float TREND_ALPHA = 0.4;

enum ForecastType {
  FORECAST_SUNNY,
  FORECAST_PARTLY_CLOUDY,
  FORECAST_CLOUDY,
  FORECAST_RAIN,
  FORECAST_STORM,
  FORECAST_UNKNOWN
};
ForecastType currentForecast = FORECAST_UNKNOWN;

uint8_t animFrame = 0;
unsigned long lastAnimUpdate = 0;
const unsigned long ANIM_INTERVAL = 180;

// latest readings (shared between OLED + web)
float tDHT = NAN, h = NAN;
float tBMP = NAN, p = NAN, alt = NAN;

// ============ FORECAST HEURISTIC =================
ForecastType calcForecast(float pressure, float temp, float hum, float trend) {
  if (isnan(pressure) || isnan(temp) || isnan(hum)) return FORECAST_UNKNOWN;

  bool veryHigh = pressure >= 1022.0;
  bool highP    = pressure >= 1015.0 && pressure < 1022.0;
  bool lowP     = pressure <= 1008.0;
  bool veryLow  = pressure <= 1000.0;

  bool rising   = trend > 0.6;
  bool falling  = trend < -0.6;
  bool steady   = !rising && !falling;

  if (veryLow && falling && hum > 75.0) return FORECAST_STORM;
  if (lowP && falling && hum > 70.0)    return FORECAST_RAIN;

  if ((lowP || veryLow) && hum > 65.0)  return FORECAST_RAIN;

  if ((highP || veryHigh) && rising && hum < 55.0) return FORECAST_SUNNY;
  if (highP && steady && hum < 60.0)               return FORECAST_PARTLY_CLOUDY;

  if (steady && hum > 60.0)      return FORECAST_CLOUDY;
  if (rising && hum > 65.0)      return FORECAST_CLOUDY;

  if (veryHigh && hum < 50.0)    return FORECAST_SUNNY;

  return FORECAST_PARTLY_CLOUDY;
}

const char* forecastToText(ForecastType f) {
  switch (f) {
    case FORECAST_SUNNY:          return "Sunny / Clear";
    case FORECAST_PARTLY_CLOUDY:  return "Partly Cloudy";
    case FORECAST_CLOUDY:         return "Cloudy";
    case FORECAST_RAIN:           return "Rain Likely";
    case FORECAST_STORM:          return "Storm Risk";
    default:                      return "No Forecast";
  }
}

const char* forecastToIcon(ForecastType f) {
  switch (f) {
    case FORECAST_SUNNY:          return "☀️";
    case FORECAST_PARTLY_CLOUDY:  return "⛅";
    case FORECAST_CLOUDY:         return "☁️";
    case FORECAST_RAIN:           return "🌧️";
    case FORECAST_STORM:          return "⛈️";
    default:                      return "❓";
  }
}

// ============ ICON DRAWING (OLED) ===============
void drawSunSmall(uint8_t x, uint8_t y) {
  u8g2.drawCircle(x, y, 5, U8G2_DRAW_ALL);
  u8g2.drawLine(x, y-8, x, y-5);
  u8g2.drawLine(x, y+5, x, y+8);
  u8g2.drawLine(x-8, y, x-5, y);
  u8g2.drawLine(x+5, y, x+8, y);
}

void drawCloudSmall(uint8_t x, uint8_t y) {
  u8g2.drawRBox(x, y, 22, 9, 3);
  u8g2.drawCircle(x+5,  y,   4, U8G2_DRAW_ALL);
  u8g2.drawCircle(x+15, y-1, 5, U8G2_DRAW_ALL);
}

void drawRainSmall(uint8_t x, uint8_t y, uint8_t frame) {
  uint8_t offset = frame % 6;
  for (uint8_t i = 0; i < 2; i++) {
    uint8_t dx = x + i * 6;
    uint8_t dy = y + (offset + i * 3) % 10;
    u8g2.drawLine(dx, dy, dx + 1, dy + 3);
  }
}

void drawStormSmall(uint8_t x, uint8_t y, uint8_t frame) {
  if (frame & 0x01) {
    u8g2.drawLine(x,   y,   x+3, y+4);
    u8g2.drawLine(x+3, y+4, x+1, y+4);
    u8g2.drawLine(x+1, y+4, x+5, y+10);
  }
}

void drawForecastIcon(ForecastType f, uint8_t frame) {
  uint8_t baseX = 0;
  uint8_t baseY = 18;

  switch (f) {
    case FORECAST_SUNNY:
      drawSunSmall(baseX + 10, baseY - 2);
      break;
    case FORECAST_PARTLY_CLOUDY:
      drawSunSmall(baseX + 8, baseY - 4);
      drawCloudSmall(baseX + 10, baseY);
      break;
    case FORECAST_CLOUDY:
      drawCloudSmall(baseX + 8, baseY);
      drawCloudSmall(baseX + 2, baseY + 4);
      break;
    case FORECAST_RAIN:
      drawCloudSmall(baseX + 10, baseY);
      drawRainSmall(baseX + 10, baseY + 8, frame);
      break;
    case FORECAST_STORM:
      drawCloudSmall(baseX + 10, baseY);
      drawStormSmall(baseX + 12, baseY + 8, frame);
      break;
    default:
      u8g2.setFont(u8g2_font_4x6_tf);
      u8g2.drawStr(2, 18, "NO");
      u8g2.drawStr(2, 26, "DATA");
      break;
  }
}

// ============ WEB UI (HTML + JS) ================
const char INDEX_HTML[] PROGMEM = R"rawlite(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<title>ESP32-CAM Weather</title>
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<style>
  :root {
    --bg: #050816;
    --card: #0b1020;
    --accent: #36cfc9;
    --accent2: #ffb347;
    --text: #f5f5f5;
    --muted: #9ca3af;
  }
  * { box-sizing: border-box; margin:0; padding:0; }
  body {
    font-family: system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
    background: radial-gradient(circle at top, #111827 0, #020617 60%);
    color: var(--text);
    display:flex;
    align-items:center;
    justify-content:center;
    min-height:100vh;
  }
  .card {
    width: min(420px, 92vw);
    border-radius: 18px;
    padding: 18px 18px 16px;
    background: radial-gradient(circle at top left, #1f2937, #020617);
    box-shadow:
      0 20px 45px rgba(0,0,0,0.7),
      0 0 0 1px rgba(148,163,184,0.18);
    position:relative;
    overflow:hidden;
  }
  .card::before {
    content:"";
    position:absolute;
    inset:-30%;
    background:
      conic-gradient(from 200deg, rgba(54,207,201,0.16), transparent 35%, rgba(248,250,252,0.06), transparent 70%);
    opacity:0.8;
    animation: spin 22s linear infinite;
    mix-blend-mode:screen;
    pointer-events:none;
  }
  @keyframes spin {
    to { transform: rotate(360deg); }
  }
  .card-inner {
    position:relative;
    z-index:2;
  }
  header {
    display:flex;
    align-items:center;
    justify-content:space-between;
    margin-bottom:10px;
  }
  .title {
    display:flex;
    flex-direction:column;
    gap:2px;
  }
  .title h1 {
    font-size:16px;
    letter-spacing:0.08em;
    text-transform:uppercase;
    font-weight:600;
  }
  .title span {
    font-size:11px;
    color:var(--muted);
  }
  .status-pill {
    border-radius:999px;
    border:1px solid rgba(148,163,184,0.45);
    padding:4px 10px;
    font-size:11px;
    color:var(--muted);
    display:flex;
    align-items:center;
    gap:6px;
    backdrop-filter:blur(10px);
    background:rgba(15,23,42,0.8);
  }
  .status-dot {
    width:7px;
    height:7px;
    border-radius:999px;
    background: #22c55e;
    box-shadow:0 0 10px rgba(34,197,94,0.8);
  }
  .hero {
    display:flex;
    align-items:center;
    justify-content:space-between;
    margin: 10px 0 8px;
  }
  .hero-main {
    display:flex;
    align-items:center;
    gap:10px;
  }
  .hero-icon {
    width:48px;
    height:48px;
    border-radius:18px;
    background: radial-gradient(circle at 20% 0, #facc15, #ea580c);
    display:flex;
    align-items:center;
    justify-content:center;
    font-size:26px;
    box-shadow:
      0 0 25px rgba(250,204,21,0.6),
      0 0 0 1px rgba(250,250,250,0.15);
    animation: float 3.4s ease-in-out infinite;
  }
  @keyframes float {
    0%,100% { transform: translateY(0px); }
    50% { transform: translateY(-4px); }
  }
  .hero-text {
    display:flex;
    flex-direction:column;
    gap:2px;
  }
  .hero-text .temp {
    font-size:26px;
    font-weight:600;
  }
  .hero-text .sub {
    font-size:12px;
    color:var(--muted);
  }
  .hero-right {
    text-align:right;
    font-size:11px;
    color:var(--muted);
  }
  .chips {
    display:flex;
    flex-wrap:wrap;
    gap:6px;
    margin-bottom:10px;
  }
  .chip {
    border-radius:999px;
    border:1px solid rgba(148,163,184,0.4);
    padding:3px 9px;
    font-size:11px;
    color:var(--muted);
    display:flex;
    align-items:center;
    gap:5px;
    backdrop-filter:blur(10px);
    background:rgba(15,23,42,0.8);
  }
  .chip strong { color:var(--text); }
  .grid {
    display:grid;
    grid-template-columns:repeat(2, minmax(0,1fr));
    gap:8px;
  }
  .metric {
    border-radius:12px;
    border:1px solid rgba(15,23,42,0.9);
    background: radial-gradient(circle at top, rgba(39,39,42,0.7), rgba(2,6,23,0.9));
    padding:8px 9px;
  }
  .metric h3 {
    font-size:11px;
    color:var(--muted);
    margin-bottom:3px;
  }
  .metric .value {
    font-size:16px;
    font-weight:600;
  }
  .metric .meta {
    font-size:10px;
    color:var(--muted);
    margin-top:1px;
  }
  footer {
    margin-top:10px;
    display:flex;
    justify-content:space-between;
    align-items:center;
    font-size:11px;
    color:var(--muted);
  }
  .pill {
    border-radius:999px;
    padding:3px 9px;
    border:1px solid rgba(148,163,184,0.4);
    backdrop-filter:blur(10px);
    background:rgba(15,23,42,0.85);
    display:flex;
    align-items:center;
    gap:5px;
  }
</style>
</head>
<body>
<div class="card">
  <div class="card-inner">
    <header>
      <div class="title">
        <h1>ESP32‑CAM Weather</h1>
        <span id="status">Live indoor station</span>
      </div>
      <div class="status-pill">
        <div class="status-dot" id="dot"></div>
        <span id="forecastLabel">Connecting…</span>
      </div>
    </header>

    <section class="hero">
      <div class="hero-main">
        <div class="hero-icon" id="icon">⏳</div>
        <div class="hero-text">
          <div class="temp" id="temp-main">--.-°C</div>
          <div class="sub" id="temp-sub">DHT / BMP: --.-°C / --.-°C</div>
        </div>
      </div>
      <div class="hero-right">
        <div id="pressureChip">Pressure: ----.- hPa</div>
        <div id="altChip">Altitude: --- m</div>
      </div>
    </section>

    <div class="chips">
      <div class="chip">
        🌡️ <strong id="chip-dht">--.-°C</strong><span>DHT11</span>
      </div>
      <div class="chip">
        🌡️ <strong id="chip-bmp">--.-°C</strong><span>BMP280</span>
      </div>
      <div class="chip">
        💧 <strong id="chip-hum">-- %</strong><span>Humidity</span>
      </div>
      <div class="chip">
        📈 <strong id="chip-trend">steady</strong><span>Pressure trend</span>
      </div>
    </div>

    <div class="grid">
      <div class="metric">
        <h3>Humidity</h3>
        <div class="value" id="m-hum">-- %</div>
        <div class="meta">Relative</div>
      </div>
      <div class="metric">
        <h3>Pressure</h3>
        <div class="value" id="m-press">----.- hPa</div>
        <div class="meta">Sea‑level relative</div>
      </div>
      <div class="metric">
        <h3>Altitude</h3>
        <div class="value" id="m-alt">--- m</div>
        <div class="meta">Estimated from pressure</div>
      </div>
      <div class="metric">
        <h3>Forecast</h3>
        <div class="value" id="m-forecast">---</div>
        <div class="meta">Next hours (local)</div>
      </div>
    </div>

    <footer>
      <div class="pill">
        🔄 <span id="updated">Waiting for first update…</span>
      </div>
      <div class="pill">
        📡 <span id="host">esp32.local</span>
      </div>
    </footer>
  </div>
</div>

<script>
let lastForecast = "";

function classifyTrend(trend) {
  const t = parseFloat(trend);
  if (isNaN(t)) return "n/a";
  if (t > 0.8) return "rising";
  if (t < -0.8) return "falling";
  return "steady";
}

function updateUI(data) {
  const d = JSON.parse(data);

  document.getElementById("temp-main").textContent =
    d.tBMP !== "NaN" ? d.tBMP + "°C" : d.tDHT + "°C";

  document.getElementById("temp-sub").textContent =
    `DHT / BMP: ${d.tDHT}°C / ${d.tBMP}°C`;

  document.getElementById("chip-dht").textContent = d.tDHT + "°C";
  document.getElementById("chip-bmp").textContent = d.tBMP + "°C";
  document.getElementById("chip-hum").textContent = d.h + " %";

  document.getElementById("pressureChip").textContent =
    "Pressure: " + d.p + " hPa";
  document.getElementById("altChip").textContent =
    "Altitude: " + d.alt + " m";

  document.getElementById("m-hum").textContent = d.h + " %";
  document.getElementById("m-press").textContent = d.p + " hPa";
  document.getElementById("m-alt").textContent = d.alt + " m";

  document.getElementById("m-forecast").textContent = d.forecastText;
  document.getElementById("forecastLabel").textContent = d.forecastText;
  document.getElementById("icon").textContent = d.forecastIcon;

  const trendLabel = classifyTrend(d.trend);
  document.getElementById("chip-trend").textContent = trendLabel;

  const now = new Date();
  document.getElementById("updated").textContent =
    "Updated " + now.toLocaleTimeString();

  lastForecast = d.forecastText;
}

async function fetchData() {
  try {
    const res = await fetch("/api/readings");
    if (!res.ok) throw new Error("HTTP " + res.status);
    const text = await res.text();
    updateUI(text);

    document.getElementById("status").textContent = "Live from ESP32‑CAM";
    document.getElementById("dot").style.background = "#22c55e";
  } catch (e) {
    document.getElementById("status").textContent = "Disconnected";
    document.getElementById("dot").style.background = "#f97316";
  }
}

setInterval(fetchData, 2000);
fetchData();
</script>
</body>
</html>
)rawlite";

// ============ HTTP HANDLERS ======================
void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleApiReadings() {
  // JSON with simple formatting, keep it light
  String json = "{";
  json += "\"tDHT\":\"" + String(isnan(tDHT) ? NAN : tDHT, 1) + "\",";
  json += "\"tBMP\":\"" + String(isnan(tBMP) ? NAN : tBMP, 1) + "\",";
  json += "\"h\":\""    + String(isnan(h)    ? NAN : h, 0)    + "\",";
  json += "\"p\":\""    + String(isnan(p)    ? NAN : p, 1)    + "\",";
  json += "\"alt\":\""  + String(isnan(alt)  ? NAN : alt, 0)  + "\",";
  json += "\"trend\":\""+ String(pressureTrend, 2)            + "\",";
  json += "\"forecastText\":\"" + String(forecastToText(currentForecast)) + "\",";
  json += "\"forecastIcon\":\"" + String(forecastToIcon(currentForecast)) + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

// ================== SETUP ========================
void setup() {
  Serial.begin(115200);
  delay(200);

  // OLED init
  u8g2.begin();
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x8_tf);
  u8g2.drawStr(0, 8, "ESP32-CAM Weather");
  u8g2.drawStr(0, 18, "Init sensors...");
  u8g2.sendBuffer();

  // Sensors
  dht.begin();
  Wire.begin(I2C_SDA, I2C_SCL);
  delay(100);

  bool bmpOk = false;
  if (bmp.begin(0x76)) {
    bmpOk = true;
  } else if (bmp.begin(0x77)) {
    bmpOk = true;
  }

  if (bmpOk) {
    lastPressure = bmp.readPressure() / 100.0F;
  }
  lastPressureSample = millis();

  // WiFi
  WiFi.begin(ssid, password);
  u8g2.clearBuffer();
  u8g2.drawStr(0, 8, "Connecting WiFi...");
  u8g2.sendBuffer();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }

  // mDNS as esp32.local
  if (MDNS.begin("esp32")) {
    // ok
  }

  // HTTP routes
  server.on("/", handleRoot);
  server.on("/api/readings", handleApiReadings);
  server.onNotFound(handleNotFound);
  server.begin();
}

// ================== LOOP =========================
void loop() {
  static unsigned long lastSensorRead = 0;
  const unsigned long SENSOR_INTERVAL = 3000;

  unsigned long now = millis();

  server.handleClient();

  // ---- Sensor reads ----
  if (now - lastSensorRead >= SENSOR_INTERVAL) {
    lastSensorRead = now;

    float ht = dht.readTemperature();
    float hh = dht.readHumidity();
    if (!isnan(ht)) tDHT = ht;
    if (!isnan(hh)) h = hh;

    float pPa = bmp.readPressure();
    float tt  = bmp.readTemperature();
    if (!isnan(pPa)) p = pPa / 100.0F;
    if (!isnan(tt))  tBMP = tt;
    alt = bmp.readAltitude(SEA_LEVEL_HPA);

    // Trend + forecast
    if (!isnan(p) && (now - lastPressureSample >= PRESSURE_SAMPLE_INTERVAL)) {
      float rawDelta = p - lastPressure;
      float newTrend = rawDelta;
      pressureTrend = (1.0 - TREND_ALPHA) * pressureTrend + TREND_ALPHA * newTrend;
      lastPressure = p;
      lastPressureSample = now;
      currentForecast = calcForecast(p, tBMP, h, pressureTrend);
    }

    static bool firstForecastDone = false;
    if (!firstForecastDone && !isnan(p) && !isnan(tBMP) && !isnan(h)) {
      currentForecast = calcForecast(p, tBMP, h, 0.0);
      firstForecastDone = true;
    }
  }

  // ---- OLED animation ----
  if (now - lastAnimUpdate >= ANIM_INTERVAL) {
    lastAnimUpdate = now;
    animFrame++;
  }

  // ---- OLED UI ----
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x8_tf);
  u8g2.drawStr(0, 8, "ESP32-CAM Weather");

  drawForecastIcon(currentForecast, animFrame);

  char buf[32];

  u8g2.setCursor(40, 16);
  u8g2.print("DHT:");
  u8g2.setCursor(72, 16);
  if (!isnan(tDHT)) snprintf(buf, sizeof(buf), "%.1fC", tDHT);
  else strcpy(buf, "--.-C");
  u8g2.print(buf);

  u8g2.setCursor(40, 26);
  u8g2.print("BMP:");
  u8g2.setCursor(72, 26);
  if (!isnan(tBMP)) snprintf(buf, sizeof(buf), "%.1fC", tBMP);
  else strcpy(buf, "--.-C");
  u8g2.print(buf);

  u8g2.setCursor(40, 36);
  u8g2.print("Hum:");
  u8g2.setCursor(72, 36);
  if (!isnan(h)) snprintf(buf, sizeof(buf), "%.0f%%", h);
  else strcpy(buf, "--%");
  u8g2.print(buf);

  u8g2.setCursor(40, 46);
  u8g2.print("P:");
  u8g2.setCursor(72, 46);
  if (!isnan(p)) snprintf(buf, sizeof(buf), "%.1fh", p);
  else strcpy(buf, "----");
  u8g2.print(buf);

  u8g2.setCursor(40, 56);
  u8g2.print("Alt:");
  u8g2.setCursor(72, 56);
  if (!isnan(alt)) snprintf(buf, sizeof(buf), "%.0fm", alt);
  else strcpy(buf, "---m");
  u8g2.print(buf);

  u8g2.setCursor(0, 64);
  u8g2.print(forecastToText(currentForecast));

  u8g2.sendBuffer();
}
