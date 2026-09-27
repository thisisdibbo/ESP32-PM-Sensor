#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <Preferences.h>
#include <HardwareSerial.h>
#include <TinyGPSPlus.h>
#include <RTClib.h>
#include <WiFi.h>
#include <time.h>

// ================= SD =================
#define SD_CS_PIN 5
File airLogFile;
bool sdAvailable = false;

// ================= BUTTONS =================
#define BTN_UP   32
#define BTN_DOWN 33

// ================= LEDs =================
#define LED_RED    25
#define LED_GREEN  26
#define LED_YELLOW 27

bool firstPMSuccess = false;
unsigned long lastGPSBlink = 0;

// ================= PMS =================
#define PMS_RX 13
#define PMS_TX 14
HardwareSerial pmsSerial(2);

// ================= GPS =================
HardwareSerial gpsSerial(1);
TinyGPSPlus gps;

// ================= RTC =================
RTC_DS3231 rtc;

// ================= WIFI / NTP =================
const char* ssid = "YOUR_WIFI_SSID";
const char* pass = "YOUR_WIFI_PASSWORD";
const char* ntpServer = "pool.ntp.org";
const long gmtOffset = 6 * 3600;

// ================= LOGGING =================
unsigned long loggingInterval = 5000; // default 5s
unsigned long lastLog = 0;

// ================= PREFS =================
Preferences prefs;

// ================= FUNCTIONS =================
void syncTimeWithNTP();
String getTimestamp();
bool readPMS(int &pm1, int &pm25, int &pm10);

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("ESP32 AIR LOGGER START");

  // Buttons
  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);

  // LEDs
  pinMode(LED_RED, OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_YELLOW, OUTPUT);
  digitalWrite(LED_RED, HIGH); // Power ON

  // RTC
  Wire.begin(21, 22);
  if (rtc.begin()) {
    syncTimeWithNTP();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }

  // UARTs
  pmsSerial.begin(9600, SERIAL_8N1, PMS_RX, PMS_TX);
  gpsSerial.begin(9600, SERIAL_8N1, 4, 2);

  // SD Card
  sdAvailable = SD.begin(SD_CS_PIN);
  if (sdAvailable) {
    airLogFile = SD.open("/airlog.csv", FILE_APPEND);
    if (airLogFile.size() == 0) {
      airLogFile.println("Timestamp,PM1,PM2.5,PM10,Latitude,Longitude,Speed(km/h),Satellites");
      airLogFile.flush();
    }
  }

  // Load saved interval from Preferences
  prefs.begin("airlog", true);
  unsigned long saved = prefs.getULong("interval", 0);
  prefs.end();
  if (saved >= 1000) loggingInterval = saved;

  Serial.println("System READY");
}

// ================= LOOP =================
void loop() {

  // ================= BUTTONS =================
  if (digitalRead(BTN_UP) == LOW) {
    loggingInterval += 5000;
    prefs.begin("airlog", false);
    prefs.putULong("interval", loggingInterval);
    prefs.end();
    Serial.print("Interval Increased: ");
    Serial.print(loggingInterval / 1000);
    Serial.println(" seconds");
    delay(300);
  }
  if (digitalRead(BTN_DOWN) == LOW && loggingInterval > 5000) {
    loggingInterval -= 5000;
    prefs.begin("airlog", false);
    prefs.putULong("interval", loggingInterval);
    prefs.end();
    Serial.print("Interval Decreased: ");
    Serial.print(loggingInterval / 1000);
    Serial.println(" seconds");
    delay(300);
  }

  // ================= GPS READ =================
  while (gpsSerial.available()) gps.encode(gpsSerial.read());

  if (gps.location.isValid() && millis() - lastGPSBlink > 5000) {
    lastGPSBlink = millis();
    digitalWrite(LED_YELLOW, HIGH);
    delay(100);
    digitalWrite(LED_YELLOW, LOW);
  }

  // ================= PERIODIC LOGGING =================
  if (millis() - lastLog >= loggingInterval) {
    lastLog = millis();

    int pm1, pm25, pm10;
    if (readPMS(pm1, pm25, pm10)) {

      // First PM success
      if (!firstPMSuccess) {
        digitalWrite(LED_RED, LOW);
        firstPMSuccess = true;
      }

      // Green LED blink
      digitalWrite(LED_GREEN, HIGH);
      delay(100);
      digitalWrite(LED_GREEN, LOW);

      // ===== TIMESTAMP =====
      String timestamp = getTimestamp();

      // ===== SERIAL OUTPUT =====
      Serial.println("----------------------------------");
      Serial.print("Time: "); Serial.println(timestamp);
      Serial.print("Logging Interval: ");
      Serial.print(loggingInterval / 1000);
      Serial.println(" seconds");
      Serial.printf("PM1=%d PM2.5=%d PM10=%d\n", pm1, pm25, pm10);
      Serial.printf("GPS: %.6f, %.6f | Speed %.2f km/h | Sat %d\n",
        gps.location.lat(),
        gps.location.lng(),
        gps.speed.kmph(),
        gps.satellites.value());

      // ===== SD CARD LOGGING =====
      if (sdAvailable && airLogFile) {
        airLogFile.printf("%s,%d,%d,%d,%.6f,%.6f,%.2f,%d\n",
          timestamp.c_str(),
          pm1, pm25, pm10,
          gps.location.lat(),
          gps.location.lng(),
          gps.speed.kmph(),
          gps.satellites.value());
        airLogFile.flush();
      }
    }
  }
}

// ================= PMS READ =================
bool readPMS(int &pm1, int &pm25, int &pm10) {
  static uint8_t buf[32];
  static uint8_t idx = 0;

  while (pmsSerial.available()) {
    uint8_t b = pmsSerial.read();

    if (idx == 0 && b != 0x42) continue;
    if (idx == 1 && b != 0x4D) { idx = 0; continue; }

    buf[idx++] = b;

    if (idx == 32) {
      idx = 0;
      pm1  = (buf[10] << 8) | buf[11];
      pm25 = (buf[12] << 8) | buf[13];
      pm10 = (buf[14] << 8) | buf[15];
      return true;
    }
  }
  return false;
}

// ================= NTP & RTC =================
void syncTimeWithNTP() {
  WiFi.begin(ssid, pass);
  if (WiFi.waitForConnectResult() == WL_CONNECTED) {
    configTime(gmtOffset, 0, ntpServer);
    struct tm t;
    if (getLocalTime(&t)) {
      rtc.adjust(DateTime(
        t.tm_year + 1900,
        t.tm_mon + 1,
        t.tm_mday,
        t.tm_hour,
        t.tm_min,
        t.tm_sec));
    }
  }
}

// ================= TIMESTAMP =================
String getTimestamp() {
  DateTime now = rtc.now();
  char buf[25];
  sprintf(buf, "%04d-%02d-%02d %02d:%02d:%02d",
          now.year(), now.month(), now.day(),
          now.hour(), now.minute(), now.second());
  return String(buf);
}
