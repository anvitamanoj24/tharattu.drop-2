/*************************************************

         THARATTU.DROP()

         cry-aware cradle companion

  ESP32 + INMP441 + LCD + SERVO + SUPABASE SYNC

  Detection: sound-level threshold on INMP441.
  This is NOT machine-learning cry recognition —
  it detects sustained loud sound. Tune
  SOUND_THRESHOLD with real microphone readings.

  Flow:
    1. ESP32 detects sustained loud sound locally
    2. Servo starts rocking the cradle immediately
    3. Supabase INSERT fires in the background so
       the separate UI updates in realtime
    4. After 15 s the servo returns to center

  Safety:
    A hardware EMERGENCY STOP button on
    ESTOP_PIN halts the servo instantly via an
    interrupt. This is a software interlock only —
    always add an independent hardware power-cut
    mechanism before deploying on a real cradle.

*************************************************/

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP32Servo.h>
#include <driver/i2s.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>

// =============================================
// WI-FI CREDENTIALS
// Replace with your router or phone hotspot.
// The ESP32 needs internet to reach Supabase.
// If the connection fails it falls back to its
// own AP (Tharattu_AP) so the local page
// remains reachable.
// =============================================
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// Fallback AP (used when router Wi-Fi fails)
const char* AP_SSID       = "Tharattu_AP";
const char* AP_PASSWORD   = "password123";

// =============================================
// SUPABASE CREDENTIALS
// Project Settings → API → Project URL / anon key
// Do NOT use the service_role key here.
// =============================================
const char* SUPABASE_URL      = "https://YOUR_PROJECT.supabase.co";
const char* SUPABASE_ANON_KEY = "YOUR_ANON_KEY";

// =============================================
// LOCAL WEB SERVER
// Serves a minimal status page + /check endpoint.
// Reachable at the router IP in STA mode,
// or at 192.168.4.1 in AP fallback mode.
// =============================================
WebServer server(80);

const char LOCAL_PAGE[] = R"rawliteral(
<!DOCTYPE html><html><head>
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <style>
    body{background:#0d0d0f;color:#e8e8f0;font-family:sans-serif;
         text-align:center;padding:40px 16px;}
    h1{color:#7c6af7;}
    #s{font-size:1.4rem;font-weight:700;color:#4ade80;}
    #s.cry{color:#f76a8a;}
    #m{font-size:0.8rem;color:#7a7a8a;margin-top:8px;}
  </style>
</head><body>
  <h1>tharattu.drop()</h1>
  <p id="s">Listening…</p>
  <p id="m">sound level: —</p>
  <script>
    setInterval(()=>fetch('/check').then(r=>r.json()).then(d=>{
      const el=document.getElementById('s');
      document.getElementById('m').textContent='sound level: '+d.level;
      if(d.state==='TRIGGERED'){el.textContent='KUNJU KARAYUNNU! 🌙';el.className='cry';}
      else if(d.state==='ESTOP'){el.textContent='⛔ EMERGENCY STOP';el.className='cry';}
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
// EMERGENCY STOP
// Wire a normally-open push button between
// ESTOP_PIN and GND. The internal pull-up is
// enabled; pressing the button pulls the pin LOW.
// An interrupt halts the servo within microseconds.
// =============================================
#define ESTOP_PIN 34   // any input-only GPIO works

volatile bool eStop = false;

void IRAM_ATTR onEStop() {
  eStop = true;
}

// =============================================
// INMP441 I²S PINS
// =============================================
#define I2S_WS   25
#define I2S_SD   33
#define I2S_SCK  26
#define I2S_PORT I2S_NUM_0

// =============================================
// SOUND DETECTION TUNING
//
// SOUND_THRESHOLD  — mean absolute sample value
//   above which a frame counts as "loud".
//   Tune this with Serial monitor readings from
//   your room before deploying.
//
// CRY_FRAMES_NEEDED — how many consecutive loud
//   frames must occur before triggering.
//   At ~100 ms per loop tick this is ~0.5 s of
//   sustained noise.
//
// CRY_DECAY_MS — if the sound drops below the
//   threshold, the counter resets to zero after
//   this many milliseconds (strict window).
// =============================================
#define SOUND_THRESHOLD    800
#define CRY_FRAMES_NEEDED    5
#define CRY_DECAY_MS       300   // ms of quiet before counter resets

// =============================================
// RUNTIME STATE
// =============================================
int           cryCount       = 0;
unsigned long lastLoudTime   = 0;   // timestamp of last loud frame
bool          systemActive   = false;
bool          apFallback     = false;
int           lastSoundLevel = 0;
unsigned long lastLogTime    = 0;

// =============================================
// WIFI — STA with AP fallback
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
    apFallback = false;
    Serial.println("\nWi-Fi connected!");
    Serial.print("IP: "); Serial.println(WiFi.localIP());
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("WiFi OK");
    lcd.setCursor(0, 1); lcd.print(WiFi.localIP());
    delay(1500);
  } else {
    // ── Fallback: become an access point ──
    apFallback = true;
    Serial.println("\nRouter Wi-Fi failed — starting AP fallback.");
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    Serial.print("AP IP: "); Serial.println(WiFi.softAPIP());
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("AP: Tharattu_AP");
    lcd.setCursor(0, 1); lcd.print("192.168.4.1");
    delay(1500);
  }
}

// =============================================
// LOCAL WEB SERVER
// /check returns JSON so the page can show
// both state and the live sound level.
// =============================================
void setupWebServer() {
  server.on("/", []() {
    server.send(200, "text/html", LOCAL_PAGE);
  });

  server.on("/check", []() {
    String state = "IDLE";
    if (eStop)        state = "ESTOP";
    else if (systemActive) state = "TRIGGERED";

    String json = "{\"state\":\"" + state +
                  "\",\"level\":" + String(lastSoundLevel) + "}";
    server.send(200, "application/json", json);
  });

  server.begin();
  IPAddress ip = apFallback ? WiFi.softAPIP() : WiFi.localIP();
  Serial.print("Local server: http://"); Serial.println(ip);
}

// =============================================
// SUPABASE — fire-and-forget HTTP POST
// Called AFTER the servo starts rocking so the
// (potentially slow) HTTP round-trip does not
// delay the physical response.
// =============================================
bool sendCryToSupabase() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[Supabase] Not connected — skipping insert.");
    return false;
  }

  // 3-second timeout so a slow connection does
  // not block the loop for a dangerous period.
  HTTPClient http;
  http.setTimeout(3000);

  String url = String(SUPABASE_URL) + "/rest/v1/baby_events";
  http.begin(url);
  http.addHeader("Content-Type",  "application/json");
  http.addHeader("apikey",        SUPABASE_ANON_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_ANON_KEY);
  http.addHeader("Prefer",        "return=minimal");

  int code = http.POST("{\"event_type\":\"CRY\"}");

  Serial.print("[Supabase] POST → HTTP ");
  Serial.println(code);
  if (code != 201) {
    Serial.println("[Supabase] Warning: expected 201. Check URL, key, and RLS policy.");
  }

  http.end();
  return (code == 201);
}

// =============================================
// INMP441 MICROPHONE SETUP
// Returns false if any driver call fails.
// =============================================
bool setupMicrophone() {
  i2s_config_t cfg = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate          = 16000,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = (i2s_comm_format_t)I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count        = 8,
    .dma_buf_len          = 64,
    .use_apll             = false,
    .tx_desc_auto_clear   = false,
    .fixed_mclk           = 0
  };

  esp_err_t err = i2s_driver_install(I2S_PORT, &cfg, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("[I2S] driver_install failed: %s\n", esp_err_to_name(err));
    return false;
  }

  i2s_pin_config_t pins = {
    .bck_io_num   = I2S_SCK,
    .ws_io_num    = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num  = I2S_SD
  };

  err = i2s_set_pin(I2S_PORT, &pins);
  if (err != ESP_OK) {
    Serial.printf("[I2S] set_pin failed: %s\n", esp_err_to_name(err));
    return false;
  }

  err = i2s_start(I2S_PORT);
  if (err != ESP_OK) {
    Serial.printf("[I2S] start failed: %s\n", esp_err_to_name(err));
    return false;
  }

  return true;
}

// =============================================
// SOUND LEVEL — mean absolute value of frame
// =============================================
int getSoundLevel() {
  int32_t samples[128];
  size_t  bytesRead = 0;

  i2s_read(I2S_PORT, &samples, sizeof(samples), &bytesRead, pdMS_TO_TICKS(100));

  int n = (int)(bytesRead / sizeof(int32_t));
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

void showEStop() {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("!! E-STOP !!");
  lcd.setCursor(0, 1); lcd.print("Reset to resume");
}

// =============================================
// SERVO — one full sweep, checks eStop each step
// Returns true normally, false if halted by eStop.
// =============================================
bool rockServo() {
  for (int p = SERVO_LEFT; p <= SERVO_RIGHT; p += 2) {
    if (eStop) return false;
    myServo.write(p);
    delay(25);
    server.handleClient();
  }
  for (int p = SERVO_RIGHT; p >= SERVO_LEFT; p -= 2) {
    if (eStop) return false;
    myServo.write(p);
    delay(25);
    server.handleClient();
  }
  return true;
}

// =============================================
// HALT — called when eStop fires
// =============================================
void haltServo() {
  myServo.write(SERVO_CENTER);
  systemActive = false;
  cryCount     = 0;
  showEStop();
  Serial.println("!!! EMERGENCY STOP — servo halted !!!");

  // Hold here, polling the web server, until the
  // device is power-cycled or physically reset.
  // This prevents accidental restart after an eStop.
  while (true) {
    server.handleClient();
    delay(10);
  }
}

// =============================================
// CRY RESPONSE
//
// Order of operations:
//   1. Set systemActive (local /check responds immediately)
//   2. Update LCD and start servo rocking
//   3. Send Supabase INSERT after rocking begins
//      so the HTTP call cannot delay the physical response
//   4. Rock for 15 s, checking eStop each sweep
//   5. Reset state
// =============================================
void activateTharattu() {
  systemActive = true;
  Serial.println(">>> KUNJU KARAYUNNU! <<<");

  showCryDetected();

  // 2-second alert window — servo not moving yet,
  // web server stays responsive
  unsigned long alertStart = millis();
  while (millis() - alertStart < 2000) {
    if (eStop) { haltServo(); return; }
    server.handleClient();
    delay(1);
  }

  showRocking();
  Serial.println("THARATTU.DROP ACTIVATED — rocking for 15 s");

  // ── Start rocking, THEN notify Supabase ──
  // One sweep first so the cradle responds before
  // the HTTP request goes out.
  if (!rockServo()) { haltServo(); return; }

  // Fire Supabase in the same task (synchronous),
  // but with a hard 3-second timeout so it cannot
  // block the loop for longer than that.
  sendCryToSupabase();

  // Continue rocking for the remainder of 15 s
  unsigned long rockStart = millis();
  while (millis() - rockStart < 13000) {   // 15 s total: 1 sweep + 1 s alert + ~1 s HTTP
    if (eStop) { haltServo(); return; }
    if (!rockServo()) { haltServo(); return; }
  }

  // Return to centre and reset
  myServo.write(SERVO_CENTER);
  systemActive = false;
  cryCount     = 0;
  lastLoudTime = 0;

  unsigned long cooldown = millis();
  while (millis() - cooldown < 1000) {
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

  // ── Emergency stop pin ──
  pinMode(ESTOP_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(ESTOP_PIN), onEStop, FALLING);

  // ── LCD ──
  delay(50);
  Wire.begin(21, 22);
  lcd.init(); delay(50);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("THARATTU.DROP");
  lcd.setCursor(0, 1); lcd.print("Booting...");

  // ── Wi-Fi (STA → AP fallback) ──
  connectWiFi();

  // ── Local web server ──
  setupWebServer();

  // ── Servo ──
  myServo.setPeriodHertz(50);
  myServo.attach(SERVO_PIN, 500, 2400);
  myServo.write(SERVO_CENTER);

  // ── Microphone ──
  bool micOk = setupMicrophone();
  if (!micOk) {
    Serial.println("[FATAL] Microphone init failed. Check wiring. Halting.");
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("MIC INIT FAILED");
    lcd.setCursor(0, 1); lcd.print("Check wiring!");
    // Hold indefinitely — do not proceed without a working mic
    while (true) {
      server.handleClient();
      delay(100);
    }
  }

  // ── Settling time ──
  unsigned long boot = millis();
  while (millis() - boot < 1500) {
    server.handleClient();
    delay(1);
  }

  showListening();

  Serial.println("=== READY ===");
  IPAddress ip = apFallback ? WiFi.softAPIP() : WiFi.localIP();
  Serial.print("Local UI : http://"); Serial.println(ip);
  if (!apFallback) {
    Serial.println("Supabase : realtime events will push to the separate UI");
  } else {
    Serial.println("Supabase : OFFLINE — AP fallback active, no cloud sync");
  }
  Serial.println("E-Stop   : press button on GPIO " + String(ESTOP_PIN) + " to halt servo");
}

// =============================================
// LOOP
// =============================================
void loop() {
  // Always check eStop first
  if (eStop) { haltServo(); return; }

  server.handleClient();

  lastSoundLevel = getSoundLevel();

  // Gated Serial logging — one line per second
  if (millis() - lastLogTime > 1000) {
    Serial.print("Sound: "); Serial.println(lastSoundLevel);
    lastLogTime = millis();
  }

  // ── Strict consecutive-frame cry detection ──
  // The counter only increments on loud frames.
  // If sound drops below threshold for more than
  // CRY_DECAY_MS the counter resets to zero,
  // requiring a fresh run of loud frames.
  if (lastSoundLevel > SOUND_THRESHOLD) {
    cryCount++;
    lastLoudTime = millis();
  } else {
    if (millis() - lastLoudTime > CRY_DECAY_MS) {
      cryCount = 0;   // quiet long enough — full reset
    }
    // Within the decay window: hold count steady
    // (neither increment nor decrement)
  }

  if (cryCount >= CRY_FRAMES_NEEDED && !systemActive) {
    activateTharattu();
  }

  delay(100);
}
