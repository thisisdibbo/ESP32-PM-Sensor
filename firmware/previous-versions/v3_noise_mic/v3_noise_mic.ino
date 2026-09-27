#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <Preferences.h>
#include <HardwareSerial.h>
#include <TinyGPSPlus.h>
#include <RTClib.h>
#include <driver/i2s.h>
#include <math.h>

// ================= SD =================
#define SD_CS_PIN 5
File airLogFile;
bool sdAvailable = false;

// ================= PMS =================
#define PMS_RX 13
#define PMS_TX 14
HardwareSerial pmsSerial(2);

// ================= GPS =================
HardwareSerial gpsSerial(1);
TinyGPSPlus gps;

// ================= RTC =================
RTC_DS3231 rtc;

// ================= LOGGING =================
unsigned long loggingInterval = 5000;
unsigned long lastLog = 0;

// ================= INMP441 =================
#define I2S_WS   17
#define I2S_SCK  16
#define I2S_SD   34

#define AUDIO_SAMPLES 512
int32_t audioBuffer[AUDIO_SAMPLES];

float audioRMS = 0;
float audioDB  = 0;

// ================= PMS STABILITY =================
int last_pm25 = 0;

// ================= FUNCTIONS =================
String getTimestamp();
bool readPMS(int &pm1, int &pm25, int &pm10);

// ================= AUDIO =================
void computeAudio() {
  double sum = 0;

  for (int i = 0; i < AUDIO_SAMPLES; i++) {
    float s = audioBuffer[i] >> 14;
    sum += s * s;
  }

  audioRMS = sqrt(sum / AUDIO_SAMPLES);
  if (audioRMS < 1) audioRMS = 1;
  audioDB = 20.0 * log10(audioRMS);
}

// ================= I2S INIT =================
void setupI2SMic() {
  i2s_config_t config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = 16000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 4,
    .dma_buf_len = 1024,
    .use_apll = false
  };

  i2s_pin_config_t pins = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = -1,
    .data_in_num = I2S_SD
  };

  i2s_driver_install(I2S_NUM_0, &config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pins);
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  delay(1500);

  Wire.begin(21, 22);

  rtc.begin();

  // GPS
  gpsSerial.begin(9600, SERIAL_8N1, 4, 2);

  // PMS
  pmsSerial.begin(9600, SERIAL_8N1, PMS_RX, PMS_TX);
  delay(200);
  pmsSerial.setRxBufferSize(256);

  // SD
  sdAvailable = SD.begin(SD_CS_PIN);
  if (sdAvailable) {
    airLogFile = SD.open("/airlog.csv", FILE_APPEND);

    if (airLogFile.size() == 0) {
      airLogFile.println("Time,PM1,PM2.5,PM10,Lat,Lon,Speed,Sats,RMS,dB");
    }
  }

  // INMP441
  setupI2SMic();

  Serial.println("SYSTEM READY");
}

// ================= LOOP =================
void loop() {

  // ================= GPS FIRST PRIORITY =================
  while (gpsSerial.available()) {
    gps.encode(gpsSerial.read());
  }

  // ================= LOGGING =================
  if (millis() - lastLog >= loggingInterval) {
    lastLog = millis();

    int pm1, pm25, pm10;

    if (readPMS(pm1, pm25, pm10)) {

      // stability filter (removes spikes)
      pm25 = (pm25 + last_pm25) / 2;
      last_pm25 = pm25;

      // ================= AUDIO =================
      size_t bytesRead = 0;

      i2s_read(I2S_NUM_0,
               (void*)audioBuffer,
               sizeof(audioBuffer),
               &bytesRead,
               portMAX_DELAY);

      computeAudio();

      String timestamp = getTimestamp();

      Serial.println("------------------------");
      Serial.println(timestamp);

      Serial.printf("PM1=%d PM2.5=%d PM10=%d\n", pm1, pm25, pm10);

      Serial.printf("GPS: %.6f, %.6f | Speed %.2f | Sats %d\n",
        gps.location.lat(),
        gps.location.lng(),
        gps.speed.kmph(),
        gps.satellites.value());

      Serial.printf("Audio RMS: %.2f | dB: %.2f\n", audioRMS, audioDB);

      // ================= SD WRITE =================
      if (sdAvailable && airLogFile) {
        airLogFile.printf(
          "%s,%d,%d,%d,%.6f,%.6f,%.2f,%d,%.2f,%.2f\n",
          timestamp.c_str(),
          pm1, pm25, pm10,
          gps.location.lat(),
          gps.location.lng(),
          gps.speed.kmph(),
          gps.satellites.value(),
          audioRMS,
          audioDB
        );

        airLogFile.flush();
      }
    }
  }
}

// ================= PMS (FIXED CHECKSUM) =================
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

      uint16_t sum = 0;
      for (int i = 0; i < 30; i++) sum += buf[i];

      uint16_t chk = (buf[30] << 8) | buf[31];

      if (sum != chk) return false;  // reject corrupted frame

      pm1  = (buf[10] << 8) | buf[11];
      pm25 = (buf[12] << 8) | buf[13];
      pm10 = (buf[14] << 8) | buf[15];

      return true;
    }
  }
  return false;
}

// ================= TIME =================
String getTimestamp() {
  DateTime now = rtc.now();
  char buf[25];

  sprintf(buf, "%04d-%02d-%02d %02d:%02d:%02d",
          now.year(), now.month(), now.day(),
          now.hour(), now.minute(), now.second());

  return String(buf);
}