#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <Preferences.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"
#include <HardwareSerial.h>
#include <time.h>

// === OLED ===
#define OLED_ADDR 0x3C
Adafruit_SSD1306 display(128, 64, &Wire, -1);

// === SD Card ===
#define SD_CS_PIN 5
bool sdAvailable = false;

// === Buttons ===
#define BTN_UP 32
#define BTN_DOWN 33

// === WiFi ===
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

// === Firebase ===
#define API_KEY "YOUR_FIREBASE_WEB_API_KEY"
#define DATABASE_URL "https://YOUR-PROJECT-default-rtdb.REGION.firebasedatabase.app/"
#define USER_EMAIL "YOUR_FIREBASE_USER_EMAIL"
#define USER_PASSWORD "YOUR_FIREBASE_USER_PASSWORD"

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// === Device Info ===
String deviceID = "device01";
String deviceName = "Device 01";
unsigned long loggingInterval = 10000; // ms
unsigned long lastLog = 0;

// === PMS Sensor ===
HardwareSerial pmsSerial(2); // UART2 RX=16 TX=17
int latestPM1 = 0, latestPM25 = 0, latestPM10 = 0;
String latestTS = "N/A";

// === Preferences ===
Preferences prefs;

// === NTP ===
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 6 * 3600; // Bangladesh GMT+6
const int daylightOffset_sec = 0;

// === Remote check timing ===
unsigned long lastRemoteCheck = 0;
const unsigned long remoteCheckInterval = 10000UL; // 10s

// === Function Forward ===
bool readPMSframeAndParse(int &pm1, int &pm25, int &pm10, bool debugPrint=false);
void syncTimeWithNTP();
String getTimestamp();
void displayData();

void setup() {
  Serial.begin(115200);
  delay(100);

  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);

  // === PMS UART ===
  pmsSerial.begin(9600, SERIAL_8N1, 16, 17);

  // === OLED ===
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED init failed");
    while (1) delay(1000);
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("Air Quality Logger");
  display.display();

  // === SD Card ===
  sdAvailable = SD.begin(SD_CS_PIN);
  if (sdAvailable) {
    Serial.println("SD init OK");
    if (!SD.exists("/airlog.csv")) {
      File f = SD.open("/airlog.csv", FILE_WRITE);
      if (f) { f.println("DateTime,PM1.0,PM2.5,PM10"); f.close(); }
    }
  } else {
    Serial.println("SD init failed or card not present");
  }

  // === Load interval from prefs ===
  prefs.begin("airlog", true);
  unsigned long saved = prefs.getULong("interval", 0);
  prefs.end();
  if (saved >= 1000UL) {
    loggingInterval = saved;
    Serial.printf("Loaded interval from prefs: %lu ms\n", loggingInterval);
  }

  // === WiFi ===
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to WiFi");
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - wifiStart > 20000UL) {
      Serial.println("\nWiFi connect timeout");
      break;
    }
    Serial.print(".");
    delay(500);
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected!");
    // sync NTP time
    syncTimeWithNTP();
  } else {
    Serial.println("Proceeding without WiFi.");
  }

  // === Firebase setup ===
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  config.token_status_callback = tokenStatusCallback;
  auth.user.email = USER_EMAIL;
  auth.user.password = USER_PASSWORD;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  Serial.println("Firebase initialized.");

  // === Upload initial device info, persist name ===
  if (!Firebase.RTDB.getString(&fbdo, "/devices/" + deviceID + "/latest/name")) {
      if (Firebase.RTDB.setString(&fbdo, "/devices/" + deviceID + "/latest/name", deviceName))
          Serial.println("Uploaded default device name");
  } else {
      deviceName = fbdo.stringData();
      Serial.println("Loaded device name from Firebase: " + deviceName);
  }

  if (Firebase.RTDB.setInt(&fbdo, "/devices/" + deviceID + "/latest/interval", loggingInterval / 1000UL))
    Serial.println("Uploaded interval");
  else Serial.println("Failed to upload interval: " + fbdo.errorReason());
}

void loop() {
  // === Button interval change (local) ===
  if (digitalRead(BTN_UP) == LOW) {
    loggingInterval += 5000UL;
    prefs.begin("airlog", false);
    prefs.putULong("interval", loggingInterval);
    prefs.end();
    Firebase.RTDB.setInt(&fbdo, "/devices/" + deviceID + "/latest/interval", loggingInterval / 1000UL);
    Serial.printf("Local interval increased: %lu ms\n", loggingInterval);
    delay(300);
  }
  if (digitalRead(BTN_DOWN) == LOW) {
    if (loggingInterval > 5000UL) loggingInterval -= 5000UL;
    prefs.begin("airlog", false);
    prefs.putULong("interval", loggingInterval);
    prefs.end();
    Firebase.RTDB.setInt(&fbdo, "/devices/" + deviceID + "/latest/interval", loggingInterval / 1000UL);
    Serial.printf("Local interval decreased: %lu ms\n", loggingInterval);
    delay(300);
  }

  // === Remote Firebase check every remoteCheckInterval ===
  if (millis() - lastRemoteCheck > remoteCheckInterval) {
    lastRemoteCheck = millis();

    // Interval
    if (Firebase.RTDB.getInt(&fbdo, "/devices/" + deviceID + "/latest/interval")) {
      long remoteSec = fbdo.intData();
      unsigned long remoteMs = (unsigned long)remoteSec * 1000UL;
      if (remoteMs >= 5000UL && remoteMs != loggingInterval) {
        loggingInterval = remoteMs;
        prefs.begin("airlog", false);
        prefs.putULong("interval", loggingInterval);
        prefs.end();
        Serial.printf("Remote interval applied: %lu ms\n", loggingInterval);
      }
    }

    // Name
    if (Firebase.RTDB.getString(&fbdo, "/devices/" + deviceID + "/latest/name")) {
      String remoteName = fbdo.stringData();
      if (remoteName != deviceName) {
        deviceName = remoteName;
        Serial.println("Remote name applied: " + deviceName);
      }
    }
  }

  // === Logging PMS ===
  if (millis() - lastLog >= loggingInterval) {
    lastLog = millis();
    int p1 = 0, p25 = 0, p10 = 0;
    if (readPMSframeAndParse(p1, p25, p10, true)) {
      latestPM1 = p1;
      latestPM25 = p25;
      latestPM10 = p10;

      // NTP timestamp
      latestTS = getTimestamp();

      // OLED Display
      displayData();

      // SD Logging
      if (sdAvailable) {
        File f = SD.open("/airlog.csv", FILE_APPEND);
        if (f) {
          f.print(latestTS); f.print(",");
          f.print(latestPM1); f.print(",");
          f.print(latestPM25); f.print(",");
          f.println(latestPM10);
          f.close();
        }
      }

      // === Firebase upload ===
      String basePath = "/devices/" + deviceID + "/";

      // latest node
      FirebaseJson latestData;
      latestData.set("PM1", latestPM1);
      latestData.set("PM25", latestPM25);
      latestData.set("PM10", latestPM10);
      latestData.set("timestamp", latestTS);
      latestData.set("name", deviceName);
      latestData.set("interval", loggingInterval / 1000UL);
      Firebase.RTDB.setJSON(&fbdo, basePath + "latest", &latestData);

      // timestamped log
      String logPath = basePath + "log_" + latestTS;
      FirebaseJson logData;
      logData.set("PM1", latestPM1);
      logData.set("PM25", latestPM25);
      logData.set("PM10", latestPM10);
      logData.set("timestamp", latestTS);
      logData.set("name", deviceName);
      logData.set("interval", loggingInterval / 1000UL);
      Firebase.RTDB.setJSON(&fbdo, logPath, &logData);

      Serial.println("📤 Uploaded latest + log entry");
    } else {
      Serial.println("⚠️ PMS read failed - will retry at next interval");
    }
  }

  delay(10);
}

// === PMS Parsing (robust sync) ===
bool readPMSframeAndParse(int &pm1, int &pm25, int &pm10, bool debugPrint) {
  unsigned long start = millis();
  uint8_t buf[32];

  while (millis() - start < 2000UL) {
    if (pmsSerial.available()) {
      int b = pmsSerial.read();
      if (b == 0x42) {
        if (pmsSerial.available() < 1) {
          unsigned long waitStart = millis();
          while (pmsSerial.available() < 1 && millis() - waitStart < 200) delay(2);
        }
        int next = pmsSerial.peek();
        if (next == 0x4D) {
          buf[0] = 0x42;
          buf[1] = pmsSerial.read();
          int got = pmsSerial.readBytes(buf + 2, 30);
          if (got != 30) continue;
          uint16_t sum = 0;
          for (int i = 0; i < 30; i++) sum += buf[i];
          uint16_t chk = ((uint16_t)buf[30] << 8) | buf[31];
          if (sum == chk) {
            pm1 = (buf[10] << 8) | buf[11];
            pm25 = (buf[12] << 8) | buf[13];
            pm10 = (buf[14] << 8) | buf[15];
            if (debugPrint) {
              Serial.printf("✅ PM1.0=%d  PM2.5=%d  PM10=%d\n", pm1, pm25, pm10);
            }
            return true;
          } else {
            Serial.println("⚠️ PMS checksum mismatch, discarding frame");
            continue;
          }
        }
      }
    }
    delay(2);
  }
  return false;
}

// === NTP time sync ===
void syncTimeWithNTP() {
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  struct tm timeinfo;
  Serial.println("Syncing time with NTP...");
  if (getLocalTime(&timeinfo)) {
    Serial.println("NTP time acquired");
  } else {
    Serial.println("NTP sync failed");
  }
}

// === Get current timestamp string ===
String getTimestamp() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "N/A";
  char buf[32];
  sprintf(buf, "%04d-%02d-%02dT%02d:%02d:%02d",
          timeinfo.tm_year + 1900,
          timeinfo.tm_mon + 1,
          timeinfo.tm_mday,
          timeinfo.tm_hour,
          timeinfo.tm_min,
          timeinfo.tm_sec);
  return String(buf);
}

// === OLED update ===
void displayData() {
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println(deviceName);
  display.println("https://thisisdibbo.github.io/PM_monitor/");
  display.print("PM1.0: "); display.println(latestPM1);
  display.print("PM2.5: "); display.println(latestPM25);
  display.print("PM10 : "); display.println(latestPM10);
  display.println(latestTS);
  display.print("Int: "); display.print(loggingInterval / 1000UL); display.println(" s");
  display.display();
}
