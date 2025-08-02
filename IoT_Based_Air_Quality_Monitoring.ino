#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_Sensor.h>
#include <DHT.h>
#include <DHT_U.h>
#include <WiFi.h>
#include <HTTPClient.h>

// ----------- WiFi & ThingSpeak --------------
const char* ssid = "Redmi Note 12";
const char* password = "sidratul";

String apiKey = "2KVDAXS4LK8XDTVT"; // Primary channel
String readAPIKey = "L9A34NKOTGMMZ33I";
String channelID = "3017369";

String secondaryApiKey = "9DNNYH9SX5T5MW7S"; // Secondary channel
String secondaryChannelID = "3018590";

// ----------- Pins --------------
#define DHTPIN 4
#define DHTTYPE DHT22
#define FAN_PIN 18
#define MQ135_PIN 34
#define MQ9_PIN 35
#define PMS_RX 16
#define PMS_TX 17

// ----------- LCD --------------
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ----------- Thresholds --------------
#define CO2_THRESHOLD 8000
#define CO_THRESHOLD 30
#define PM25_THRESHOLD 255
#define PM10_THRESHOLD 425

// ----------- Globals --------------
DHT dht(DHTPIN, DHTTYPE);

float mq9_base = 0;
float mq135_base = 0;

bool fanState = false;
bool desiredFanState = false;
bool fanChangePending = false;
unsigned long fanChangeRequestTime = 0;
const unsigned long fanDebounceDelay = 2000;

unsigned long lastPageSwitch = 0;
int currentPage = 0;
const unsigned long pageInterval = 5000;
const unsigned long FanInterval = 5000;
unsigned long lastPrint = 0;
const unsigned long printInterval = 100;

float CO_ppm = 0, CO2_ppm = 0;
float temperature = 0, humidity = 0;
int pm25 = 0, pm10 = 0;
float aqi_cat = 0;

int fanControlMode = 2; // 0 = OFF, 1 = ON, 2 = AUTO

// PMS struct
struct pms5003data {
  uint16_t framelen;
  uint16_t pm10_standard, pm25_standard, pm100_standard;
  uint16_t pm10_env, pm25_env, pm100_env;
  uint16_t particles_03um, particles_05um, particles_10um, particles_25um, particles_50um, particles_100um;
  uint16_t unused;
  uint16_t checksum;
};
pms5003data data;

// MQ calibration
#define RL_VALUE 1.0

float R0_CO2 = 17.84;   // R0 for CO₂, calibrate properly
float R0_NH3 = 0.132;    // Example R0 for Ammonia
float R0_Toluene = 1.268; // Example R0 for Toluene
float R0_Alcohol = 1.59; // Example R0 for Alcohol
float R0_Acetone = 2.196; // Example R0 for Alcohol
float R0_CO = 0.493; // Example R0 for Alcohol
float R0_LPG = 0.3806; // Example R0 for Alcohol
float R0_CH4 = 0.832; // Example R0 for Alcohol

unsigned long lastThinkSpeakUpdate = 0;
const unsigned long thinkSpeakInterval = 20000;
unsigned long lastFanUpdate = 0;

void setup() {
  Serial.begin(115200);
  dht.begin();
  pinMode(FAN_PIN, OUTPUT);
  digitalWrite(FAN_PIN, HIGH);
  Serial1.begin(9600, SERIAL_8N1, PMS_RX, PMS_TX);

  turnFanOff();

  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Air Monitor");
  lcd.setCursor(0, 1);
  lcd.print("Calibrating...");

  calibrateMQ();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Ready!");
  delay(2000);

  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected!");
}

void loop() {
  temperature = dht.readTemperature();
  humidity = dht.readHumidity();

  int adc135 = analogRead(MQ135_PIN);
  int adc9 = analogRead(MQ9_PIN);

  float v135 = adc135 * (3.3 / 4095.0);
  float v9 = adc9 * (3.3 / 4095.0);

  float e  = 2.7183;

  //float th_corr_135 = -0.45*(pow(e, -temperature/15)-pow(e, -4/3)) + (humidity - 65)*0.1/52;
  //float th_corr_9   = -0.325*(pow(e, -temperature/30)-pow(e, -20/30)) + (humidity - 65)*0.1/52;

  float RS135 = RL_VALUE * ((5 - v135) / v135);
  float RS9 = RL_VALUE * ((5 - v9) / v9);

  float rCO2 = RS135 / R0_CO2;
  float rNH3 = RS135 / R0_NH3;
  float rAlcohol = RS135 / R0_Alcohol;
  float rAcetone = RS135 / R0_Acetone;
  float rToluene = RS135 / R0_Toluene;

  float rCO = RS9 / R0_CO;
  float rCH4 = RS9 / R0_CH4;
  float rLPG = RS9 / R0_LPG;

  //float rCO2 = RS135 / R0_CO2 + th_corr_135;
  //float rNH3 = RS135 / R0_NH3 + th_corr_135;
  //float rAlcohol = RS135 / R0_Alcohol + th_corr_135;
  //float rAcetone = RS135 / R0_Acetone + th_corr_135;
  //float rToluene = RS135 / R0_Toluene + th_corr_135;

  //float rCO = RS9 / R0_CO + th_corr_9;
  //float rCH4 = RS9 / R0_CH4 + th_corr_9;
  //float rLPG = RS9 / R0_LPG + th_corr_9;

  CO_ppm = getPPM_CO(rCO);
  CO2_ppm = getPPM_CO2(rCO2);

  if (readPMSdata(&Serial1)) {
    pm25 = data.pm25_standard;
    pm10 = data.pm10_standard;
  }

  aqi_cat = getAQICategory(CO_ppm, pm25, pm10);

  if (millis() - lastPrint > printInterval) {
    lastPrint = millis();
    Serial.printf("T: %.1fC H: %.1f%% PM2.5: %d PM10: %d CO: %.2f CO2: %.2f CH4: %.2f NH3: %.2f Alcohol: %.2f Acetone: %.2f LPG: %.2f\n",
                  temperature, humidity, pm25, pm10, CO_ppm, CO2_ppm,
                  getPPM_CH4(rCH4), getPPM_NH3(rNH3), getPPM_alcohol(rAlcohol),
                  getPPM_acetone(rAcetone), getPPM_LPG(rLPG));
  }

  // -------- Fan Control Logic --------
  desiredFanState = (fanControlMode == 1) || (fanControlMode == 2 && (CO2_ppm > CO2_THRESHOLD || CO_ppm > CO_THRESHOLD || pm25 > PM25_THRESHOLD || pm10 > PM10_THRESHOLD));
  if (desiredFanState != fanState) {
    if (!fanChangePending) {
      fanChangePending = true;
      fanChangeRequestTime = millis();
    } else if (millis() - fanChangeRequestTime >= fanDebounceDelay) {
      desiredFanState ? turnFanOn() : turnFanOff();
      fanChangePending = false;
    }
  } else {
    fanChangePending = false;
  }

  // -------- LCD Switch --------
  if (millis() - lastPageSwitch > pageInterval) {
    currentPage = (currentPage + 1) % 3;
    lastPageSwitch = millis();
  }

  if (millis() - lastFanUpdate > FanInterval) {
    lastFanUpdate = millis();
    updateLCD(rNH3, rCH4);
  }

  // -------- Send to ThingSpeak --------
  if (millis() - lastThinkSpeakUpdate > thinkSpeakInterval) {
    lastThinkSpeakUpdate = millis();
    updateThinkSpeak(RS135, RS9);
    updateSecondaryChannel(rCH4, rAlcohol, rNH3, rAcetone, rLPG);
  }
}

void calibrateMQ() {
  long mq9_sum = 0, mq135_sum = 0;
  for (int i = 0; i < 100; i++) {
    mq9_sum += analogRead(MQ9_PIN);
    mq135_sum += analogRead(MQ135_PIN);
    delay(50);
  }
  mq9_base = mq9_sum / 100.0;
  mq135_base = mq135_sum / 100.0;
  Serial.printf("MQ-9 base: %.2f\nMQ-135 base: %.2f\n", mq9_base, mq135_base);
}

void updateLCD(float rNH3, float rCH4) {
  lcd.clear();
  if (currentPage == 0) {
    lcd.setCursor(0, 0);
    lcd.print("CO:"); lcd.print(CO_ppm, 0); lcd.print(" CO2:"); lcd.print(CO2_ppm, 0);
    lcd.setCursor(0, 1);
    lcd.print("NH3:"); lcd.print(getPPM_NH3(rNH3), 0); lcd.print(" CH4:"); lcd.print(getPPM_CH4(rCH4), 0);
  } else if (currentPage == 1) {
    lcd.setCursor(0, 0);
    lcd.print("PM2.5:"); lcd.print(pm25);
    lcd.setCursor(0, 1);
    lcd.print("PM10:"); lcd.print(pm10);
  } else {
    lcd.setCursor(0, 0);
    lcd.print("T:"); lcd.print(temperature, 1); lcd.print("C H:"); lcd.print(humidity, 0); lcd.print("%");
    lcd.setCursor(0, 1);
    lcd.print(fanState ? "Fan: ON " : "Fan: OFF ");
    lcd.print("AQI:"  ); lcd.print(aqi_cat);
  }
}

void turnFanOn() {
  digitalWrite(FAN_PIN, LOW);
  fanState = true;
  Serial.println("Fan ON");
}

void turnFanOff() {
  digitalWrite(FAN_PIN, HIGH);
  fanState = false;
  Serial.println("Fan OFF");
}

boolean readPMSdata(Stream *s) {
  if (!s->available() || s->peek() != 0x42 || s->available() < 32) return false;
  uint8_t buffer[32]; uint16_t sum = 0;
  s->readBytes(buffer, 32);
  for (uint8_t i = 0; i < 30; i++) sum += buffer[i];
  uint16_t buffer_u16[15];
  for (uint8_t i = 0; i < 15; i++) {
    buffer_u16[i] = buffer[2 + i * 2 + 1];
    buffer_u16[i] += (buffer[2 + i * 2] << 8);
  }
  memcpy((void *)&data, (void *)buffer_u16, 30);
  return (sum == data.checksum);
}
float getAQICategory(float co, int pm25, int pm10) {
  float cat;
  if(co >= 30.5|| pm25 >= 225|| pm10 >= 425) cat = 5.5;
  else if(co >= 15.5 || pm25 >= 125 || pm10 >= 355) cat = 4.5;
  else if(co >= 12.5|| pm25 >= 55 || pm10 >= 255 ) cat = 3.5;
  else if(co >= 9.5|| pm25 >= 35 || pm10 >= 155 ) cat = 2.5;
  else if(co >= 4.5|| pm25 >= 9  || pm10 >= 55 ) cat = 1.5;
  else cat = 0.5;
  return cat;
}

void updateThinkSpeak(float RS135, float RS9) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    String url = "http://api.thingspeak.com/update?api_key=" + apiKey +
                 "&field1=" + String(temperature, 1) +
                 "&field2=" + String(humidity, 1) +
                 "&field3=" + String(CO_ppm, 1) +
                 "&field4=" + String(CO2_ppm, 1) +
                 "&field5=" + String(pm25) +
                 "&field6=" + String(pm10) +
                 "&field7=" + String(getPPM_CH4(RS9 / R0_CH4), 1);
    http.begin(url);
    http.GET();
    http.end();

    String readURL = "http://api.thingspeak.com/channels/" + channelID + "/fields/8/last.txt?api_key=" + readAPIKey;
    http.begin(readURL);
    int httpCode = http.GET();
    if (httpCode > 0) {
      String payload = http.getString();
      fanControlMode = payload.toInt();
    }
    http.end();
  }
}

void updateSecondaryChannel(float rCH4, float rAlcohol, float rNH3, float rAcetone, float rLPG) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    String url = "http://api.thingspeak.com/update?api_key=" + secondaryApiKey +
                
                 "&field1=" + String(getPPM_alcohol(rAlcohol), 1) +
                 "&field2=" + String(getPPM_NH3(rNH3), 1) +
                 "&field3=" + String(getPPM_acetone(rAcetone), 1) +
                 "&field4=" + String(getPPM_LPG(rLPG), 1) +
                 "&field5=" + String(aqi_cat, 1);

    http.begin(url);
    http.GET();
    http.end();
  }
}

// ----- GAS conversion functions -----
float getPPM_CO2(float r) { return pow(10, (log10(r) - 0.72) / -0.36); }
float getPPM_NH3(float r) { return pow(10, (log10(r) - 1.137) / -0.5686); }
float getPPM_CO(float r) { return pow(10, (log10(r) - 1.307) / -0.468); }
float getPPM_CH4(float r) { return pow(10, (log10(r) - 1.206) / -0.317); }
float getPPM_toluene(float r) { return pow(10, (log10(r) - 0.505) / -0.301); }
float getPPM_alcohol(float r) { return pow(10, (log10(r) - 0.603) / -0.3245); }
float getPPM_acetone(float r) { return pow(10, (log10(r) - 0.477) / -0.301); }
float getPPM_LPG(float r) { return pow(10, (log10(r) - 1.29) / -0.43); }
