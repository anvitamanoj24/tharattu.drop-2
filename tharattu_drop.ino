/*************************************************

         THARATTU.DROP()

         cry-aware cradle companion

  ESP32 + INMP441 + LCD + SERVO + SUPABASE SYNC

  Cry is detected locally → servo rocks the cradle
  immediately (no internet latency), then a Supabase
  INSERT fires so the separate UI updates in realtime.

*************************************************/

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP32Servo.h>
#include <driver/i2s.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>

// =============================================
// WI-FI — use a network with internet access
// so the ESP32 can reach Supabase.
// A phone hotspot works perfectly.
// =============================================
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// =============================================
// SUPABASE CREDENTIALS
// Project Settings → API → Project URL / anon key
// Do NOT use the service_role key here.
// =============================================
const char* SUPABASE_URL     = "https://YOUR_PROJECT.supabase.co";
const char* SUPABASE_ANON_KEY = "YOUR_ANON_KEY";

// =============================================
// LOCAL WEB SERVER (optional — keeps the
// embedded phone UI working on the local AP
// even when the ESP32 is also on a real network)
// =============================================
WebServer server(80);

// Minimal status page served over the local IP
const char LOCAL_PAGE[] = R"rawliteral(
<!DOCTYPE html><html><head>
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <style>
    body{background:#0d0d0f;color:#e8e8f0;font-family:sans-serif;
         text-align:center;padding:40px 16px;}
    h1{color:#7c6af7;}
    #s{font-size:1.4rem;font-weight:700;color:#4ade80;}
    #s.cry{color:#f76a8a;}
  </style>
</head><body>
  <h1>tharattu.drop()</h1>
  <p id="s">Listening…</p>
  <script>
    setInterval(()=>fetch('/check').then(r=>r.text()).then(t=>{
      const el=document.getElementById('s');
      if(t==='TRIGGERED'){el.textContent='KUNJU KARAYUNNU! 🌙';el.className='cry';}
      else{el.textContent='Listening…';el.className='';}
    }).catch(()=>{}),500);
  </script>
</body></html>
)rawliteral";

// =============================================
// LCD  (try 0x3F if screen stays blank)
// =============================================
LiquidCrystal_I2C lcd(0x27, 16, 2);

// =============================================
// SERVO
// =============================================
Servo myServo;
#define SERVO_PIN    27
#define SERVO_CENTER 90
#define SERVO_LEFT   60
#define SERVO_RIGHT  120

// =============================================
// INMP441 I²S PINS
// =============================================
#define I2S_WS   25
#define I2S_SD   33
#define I2S_SCK  26
#define I2S_PORT I2S_NUM_0

// =============================================
// SOUND DETECTION TUNING
// =============================================
#define SOUND_THRESHOLD    800   // raise if too sensitive
#define CRY_COUNT_REQUIRED   5   // consecutive loud frames needed

// =============================================
// RUNTIME STATE
// =============================================
int  cryCount    = 0;
bool systemActive = false;
unsigned long lastLogTime = 0;

// =============================================
// WIFI CONNECT
// =============================================
void connectWiFi() {
  Serial.print("Connecting to Wi-Fi: ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Connecting WiFi");

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWi-Fi connected!");
    Serial.print("IP: "); Serial.println(WiFi.localIP());
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("WiFi OK");
    lcd.setCursor(0, 1); lcd.print(WiFi.localIP());
    delay(1500);
  } else {
    Serial.println("\nWi-Fi FAILED — running offline.");
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("WiFi FAILED");
    lcd.setCursor(0, 1); lcd.print("Offline mode");
    delay(1500);
  }
}

// =============================================
// LOCAL WEB SERVER  (status + /check endpoint)
// =============================================
void setupWebServer() {
  server.on("/", []() {
    server.send(200, "text/html", LOCAL_PAGE);
  });

  server.on("/check", []() {
    server.send(200, "text/plain", systemActive ? "TRIGGERED" : "IDLE");
  });

  server.begin();
  Serial.print("Local server: http://");
  Serial.println(WiFi.localIP());
}

// =============================================
// SUPABASE  — INSERT a CRY event
// Returns true on HTTP 201, false otherwise.
// =============================================
bool sendCryToSupabase() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[Supabase] Wi-Fi not connected — skipping insert.");
    return false;
  }

  HTTPClient http;
  String url = String(SUPABASE_URL) + "/rest/v1/baby_events";

  http.begin(url);
  http.addHeader("Content-Type",  "application/json");
  http.addHeader("apikey",        SUPABASE_ANON_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_ANON_KEY);
  http.addHeader("Prefer",        "return=minimal");

  int code = http.POST("{\"event_type\":\"CRY\"}");

  Serial.print("[Supabase] POST → HTTP ");
  Serial.println(code);

  http.end();
  return (code == 201);
}

// =============================================
// INMP441 MICROPHONE SETUP
// =============================================
void setupMicrophone() {
  i2s_config_t cfg = {
    .mode              = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate       = 16000,
    .bits_per_sample   = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format    = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = (i2s_comm_format_t)I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags  = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count     = 8,
    .dma_buf_len       = 64,
    .use_apll          = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk        = 0
  };
  i2s_driver_install(I2S_PORT, &cfg, 0, NULL);

  i2s_pin_config_t pins = {
    .bck_io_num   = I2S_SCK,
    .ws_io_num    = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num  = I2S_SD
  };
  i2s_set_pin(I2S_PORT, &pins);
  i2s_start(I2S_PORT);
}

// =============================================
// SOUND LEVEL  (mean absolute value of frame)
// =============================================
int getSoundLevel() {
  int32_t samples[128];
  size_t  bytesRead = 0;

  i2s_read(I2S_PORT, &samples, sizeof(samples), &bytesRead, pdMS_TO_TICKS(100));

  int n = bytesRead / sizeof(int32_t);
  if (n <= 0) return 0;

  long long total = 0;
  for (int i = 0; i < n; i++) {
    total += labs(samples[i] >> 14);
  }
  return (int)(total / n);
}

// =============================================
// LCD HELPERS
// =============================================
void showListening() {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("THARATTU.DROP");
  lcd.setCursor(0, 1); lcd.print("Listening...");
}

void showCryDetected() {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("KUNJU");
  lcd.setCursor(0, 1); lcd.print("KARAYUNNU!");
}

void showRocking() {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("THARATTU.DROP");
  lcd.setCursor(0, 1); lcd.print("Rocking...");
}

// =============================================
// SERVO — one full left-right sweep
// Calls server.handleClient() mid-sweep so the
// local web server stays responsive.
// =============================================
void rockServo() {
  for (int p = SERVO_LEFT; p <= SERVO_RIGHT; p += 2) {
    myServo.write(p);
    delay(25);
    server.handleClient();
  }
  for (int p = SERVO_RIGHT; p >= SERVO_LEFT; p -= 2) {
    myServo.write(p);
    delay(25);
    server.handleClient();
  }
}

// =============================================
// MAIN CRY RESPONSE
//
// Order of operations:
//   1. Set systemActive so the /check endpoint
//      and Supabase both report TRIGGERED
//   2. Fire Supabase INSERT (non-blocking if
//      Wi-Fi is up; skipped gracefully if not)
//   3. Show LCD alert
//   4. Rock the cradle for 15 s
//   5. Reset
// =============================================
void activateTharattu() {
  systemActive = true;

  Serial.println(">>> KUNJU KARAYUNNU! <<<");

  // ── Notify Supabase so the separate UI fires ──
  sendCryToSupabase();

  // ── Local response ──
  showCryDetected();

  // Brief pause so the UI can display the alert
  // before the servo starts (keeps Serial clean)
  unsigned long t = millis();
  while (millis() - t < 2000) {
    server.handleClient();
    delay(1);
  }

  Serial.println("THARATTU.DROP ACTIVATED — rocking for 15 s");
  showRocking();

  unsigned long rockStart = millis();
  while (millis() - rockStart < 15000) {
    rockServo();
  }

  // Return to centre and reset state
  myServo.write(SERVO_CENTER);
  systemActive = false;
  cryCount     = 0;

  // Short cooldown before listening again
  t = millis();
  while (millis() - t < 1000) {
    server.handleClient();
    delay(1);
  }

  showListening();
  Serial.println("Listening...");
}

// =============================================
// SETUP
// =============================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== THARATTU.DROP() STARTING ===");

  // LCD
  delay(50);
  Wire.begin(21, 22);
  lcd.init(); delay(50);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("THARATTU.DROP");
  lcd.setCursor(0, 1); lcd.print("Booting...");

  // Wi-Fi (must come before web server)
  connectWiFi();

  // Local web server
  setupWebServer();

  // Servo
  myServo.setPeriodHertz(50);
  myServo.attach(SERVO_PIN, 500, 2400);
  myServo.write(SERVO_CENTER);

  // Microphone
  setupMicrophone();

  // Short settling time
  unsigned long boot = millis();
  while (millis() - boot < 1500) {
    server.handleClient();
    delay(1);
  }

  showListening();

  Serial.println("=== READY ===");
  Serial.print("Local UI : http://"); Serial.println(WiFi.localIP());
  Serial.println("Supabase : realtime events will push to the separate UI");
}

// =============================================
// LOOP
// =============================================
void loop() {
  server.handleClient();

  int level = getSoundLevel();

  // Gated logging — one line per second
  if (millis() - lastLogTime > 1000) {
    Serial.print("Sound: "); Serial.println(level);
    lastLogTime = millis();
  }

  // Debounced cry detection
  if (level > SOUND_THRESHOLD) {
    cryCount++;
  } else {
    if (cryCount > 0) cryCount--;
  }

  if (cryCount >= CRY_COUNT_REQUIRED && !systemActive) {
    activateTharattu();
  }

  delay(100);
}
