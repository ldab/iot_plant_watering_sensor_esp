/******************************************************************************
main.cpp
Connected, IoT, WiFi capacitance plant watering sensor based on ESP32
Leonardo Bispo
Nov, 2020
https://github.com/ldab/iot_plant_watering_sensor_esp32
Distributed as-is; no warranty is given.
******************************************************************************/

#include <Arduino.h>
#include <Ticker.h>
#include <WiFi.h>
#include <Wire.h>
#include <driver/adc.h>
#include <esp_adc_cal.h>

#include "ArduinoHttpClient.h"
#include "AsyncMqttClient.h"
#include "FFat.h"
#include "FS.h"
#include "soc/rtc_cntl_reg.h"  //disable brownout problems
#include "soc/soc.h"           //disable brownout problems
//#include "PubSubClient.h"
#include "RTClib.h"
#include "RTTTL.h"
#include "secrets.h"

#ifndef DEVICE_NAME
#error Remember to define the Device Name
#elif not defined TO
#error Remember to set the email address
#endif

#ifdef VERBOSE
#define DBG(msg, ...) \
  { Serial.printf("[%lu] " msg, millis(), ##__VA_ARGS__); }
#else
#define DBG(...)
#endif

// MQTT reconnection timeout in ms
#define MQTT_RC_TIMEOUT 5000

// Temperature reading timer in seconds
#define TEMP_TIMEOUT 1

#define C_SENSE A5   // GPIO33 - IO33/ADC1_CH5
#define BATT_ADC A4  // GPIO32 - IO32/ADC1_CH4
#define BATT_EN A11  // GPIO0
#define PWM A14      // GPIO13
#define BUZZER A15   // GPIO12
#define SDA_PIN A13  // GPIO15
#define SCL_PIN A10  // GPIO04

#define PWM_CHANNEL 0
#define BUZZER_CHANNEL 1
#define PWM_FREQUENCY 1000000L  // 1MHz
#define PWM_RESOLUTION 8        // bits

// Update these with values suitable for your network.
const char *wifi_ssid = s_wifi_ssid;
const char *wifi_password = s_wifi_password;
const char *mqtt_server = s_mqtt_server;
const char *mqtt_user = s_mqtt_user;
const char *mqtt_pass = s_mqtt_pass;
uint16_t mqtt_port = s_mqtt_port;

/*
{
    "to": "email",
    "state": "off",
    "attributes": {
        "friendly_name": "ESP_BANANA_moisture",
        "device_class": "moisture"
    }
}
*/

// initialize the MQTT Client
WiFiClient espClient;
// PubSubClient client(espClient);
AsyncMqttClient mqttClient;
bool published = false;

// RTC instance
RTC_PCF8563 rtc;

int16_t capSensorThrs = -1;
int16_t capSensorSense = -1;
char iso8601date[] = "2000-01-01T00:00:00";

void WiFiEvent(WiFiEvent_t event) {
  DBG("[WiFi-event] event: %d\n", event);

  switch (event) {
    case SYSTEM_EVENT_STA_CONNECTED:
      DBG("Connected to access point\n");
      break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
      DBG("Disconnected from WiFi access point\n");
      break;
    case SYSTEM_EVENT_STA_GOT_IP:
      DBG("Obtained IP address: ");
      Serial.println(WiFi.localIP());
      break;
    default:
      break;
  }
}

void setup_wifi() {
  delay(10);

  char id[32];
  sprintf(id, "%s-%s", DEVICE_NAME, WiFi.macAddress().c_str());
  DBG("Device name is %s\n", id);

  WiFi.onEvent(WiFiEvent);

  if (WiFi.SSID() != wifi_ssid) {
    DBG("Connecting to %s\n", wifi_ssid);

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);  // TODO maybe false as WiFI should start off

    // int32_t wifi_channel = WiFi.channel();
    // uint8_t wifi_bssid[6]; //{0xF8, 0xD1, 0x11, 0x24, 0xB3, 0x84};

    // https://github.com/espressif/arduino-esp32/issues/2537
    WiFi.config(
        INADDR_NONE, INADDR_NONE,
        INADDR_NONE);  // call is only a workaround for bug in WiFi class

    // WiFi.begin(wifi_ssid, wifi_password, wifi_channel, wifi_bssid);
    WiFi.begin(wifi_ssid, wifi_password);

    WiFi.setHostname(id);

    WiFi.persistent(true);  // TODO maybe false as WiFI should start off
  }

  while (millis() < 10000)  // TODO
  {
    int8_t wifi_result = WiFi.waitForConnectResult();
    if (wifi_result == WL_CONNECTED) break;
    DBG(" WiFi connect failed: %d\n", wifi_result);
  }
  if (WiFi.status() == WL_CONNECTED) {
    randomSeed(micros());
    DBG(" WiFi connected\n");

    mqttClient.setServer(mqtt_server, mqtt_port);
    mqttClient.setCredentials(mqtt_user, mqtt_pass);
    mqttClient.setClientId(id);
    DBG("Connecting to MQTT Server %s\n", mqtt_server);
    mqttClient.connect();
  } else {
    // TODO ?Sleep?
  }

  while (!mqttClient.connected() && millis() < 10000) {
    delay(50);
  }
}

void onMqttConnect(bool sessionPresent) { DBG("Connected to MQTT\n"); }

void onMqttPublish(uint16_t packetId) {
  DBG("Publish acknowledged.\n");
  DBG("  packetId: %d\n", packetId);
  published = true;
}

void inline static beep() {
  ledcWriteTone(BUZZER_CHANNEL, 4000L);
  ledcWrite(BUZZER_CHANNEL, 120);

  // digitalWrite(BUZZER, HIGH);
  delay(42);
  // digitalWrite(BUZZER, LOW);

  ledcWrite(BUZZER_CHANNEL, 0);
}

void static chirp(uint8_t times) {
  /* TODO
    // radio off to save power
  btStop();
  WiFi.mode( WIFI_OFF );

  //ligth sleep 0.8mA
  https://lastminuteengineers.com/esp32-sleep-modes-power-consumption/
  esp_sleep_enable_timer_wakeup( 666 );
  esp_light_sleep_start();
  */

  while (times-- > 0) {
    beep();
    delay(40);
  }
}

int16_t read_moisture() {
  int16_t _adc;

  ledcWriteTone(PWM_CHANNEL, PWM_FREQUENCY);
  ledcWrite(PWM_CHANNEL, 120);

  DBG("PWM C SENSE\n");
  delay(250);  // TODO test the time required to stabilize

  _adc = adc1_get_raw(ADC1_CHANNEL_5);

  ledcWrite(PWM_CHANNEL, 0);

  DBG("Read ADC for capacitive sensor: %d\n", _adc);

  return _adc;
}

void getInternetTime() {
  uint8_t timezone = 1;
  uint8_t daysavetime = 1;

  configTime(3600 * timezone, daysavetime * 3600, "dk.pool.ntp.org",
             "0.pool.ntp.org", "1.pool.ntp.org");
  struct tm tmstruct;
  tmstruct.tm_year = 0;
  getLocalTime(&tmstruct, 5000);  // TODO timeout
  sprintf(iso8601date, "%d-%02d-%02dT%02d:%02d:%02d\n",
          (tmstruct.tm_year) + 1900, (tmstruct.tm_mon) + 1, tmstruct.tm_mday,
          tmstruct.tm_hour, tmstruct.tm_min, tmstruct.tm_sec);
  DBG("Now is : %s\n", iso8601date);
}

void pinInit() {
  pinMode(BATT_EN, OUTPUT);
  digitalWrite(BATT_EN, HIGH);

  ledcSetup(PWM_CHANNEL, PWM_FREQUENCY, PWM_RESOLUTION);
  ledcAttachPin(PWM, PWM_CHANNEL);

  ledcSetup(BUZZER_CHANNEL, PWM_FREQUENCY, PWM_RESOLUTION);
  ledcAttachPin(BUZZER, BUZZER_CHANNEL);

  adc1_config_width(ADC_WIDTH_BIT_11);  // Reduce ADC resolution due to reported
                                        // noise on 12 bits
  adc1_config_channel_atten(
      ADC1_CHANNEL_5, ADC_ATTEN_DB_11);  // 11dB attenuation (ADC_ATTEN_DB_11)
                                         // gives full-scale voltage 3.5V
  adc1_config_channel_atten(
      ADC1_CHANNEL_4, ADC_ATTEN_DB_6);  // 6dB attenuation (ADC_ATTEN_DB_6)
                                        // gives full-scale voltage 2.2V
  /*
    esp_adc_cal_characteristics_t *adc_chars = new
    esp_adc_cal_characteristics_t; esp_adc_cal_characterize(ADC_UNIT_1,
    ADC_ATTEN_DB_11, ADC_WIDTH_BIT_11, 1098, adc_chars);  // !! TODO calibrate
    Vref
  */
}

uint16_t getBattmilliVcc() {
  digitalWrite(BATT_EN, LOW);

  delay(100);  // TODO

  uint16_t raw = adc1_get_raw(ADC1_CHANNEL_4);  // analogRead(BATT_ADC);
  uint16_t battmilliVcc = (raw * 2200 / 2047 * 2);

  DBG("Measure ADC Battery, raw: %d, %dmV\n", raw, battmilliVcc);

  digitalWrite(BATT_EN, HIGH);

  return battmilliVcc;
}

void rtc_init() {
  DBG("RTC init\n");

  Wire.begin(SDA_PIN, SCL_PIN, 100000L);
  if (!rtc.begin()) {
    DBG("Couldn't find RTC\n");
    // abort(); TODO
  }

  // Disable CLKOUT @ 200nA vs 550nA when active
  rtc.writeSqwPinMode(PCF8563_SquareWaveOFF);

  // When the RTC was stopped and stays connected to the battery, it has
  // to be restarted by clearing the STOP bit. Let's do this to ensure
  // the RTC is running.
  rtc.start();
}

void powerOff(DateTime now) {
  WiFi.disconnect(true);  // TODO check if false changes time
                          /*
                            if (now.hour() >= 18) {
                              DBG("Set wake to 8AM\n");
                              rtc.setAlarm(8, 00);
                            } else {
                              DBG("Set wake to 6PM\n");
                              rtc.setAlarm(18, 00);
                            }
                          */

  rtc.setAlarm(DateTime(now.unixtime() + 60), PCF8563_Alarm_Daily);

  DBG("Clear alarm flag, disabling LDO\n");

  rtc.clearAlarm();
}

int16_t readThreshold(void) {
  const char *path = "/threshold.txt";

  DBG("Reading file: %s\r\n", path);

  File file = FFat.open(path);
  if (!file || file.isDirectory()) {
    DBG("- failed to open file for reading\n");
    return -2;
  }

  DBG("Read from file: ");

  uint8_t i = 0;
  char t[5] = "";

  while (file.available()) {
    t[i] = file.read();
    DBG("%c", t[i]);
    i++;
  }
  DBG("\n");

  file.close();

  return atoi(t);
}

void deleteFile(const char *path) {
  DBG("Deleting file: %s\n", path);
  if (FFat.remove(path)) {
    DBG("File deleted\n");
  } else {
    DBG("Delete failed\n");
  }
}

void writeThreshold(int16_t _t) {
  const char *path = "/threshold.txt";

  // deleteFile(path); // not needed
  DBG("Writing file: %s\n", path);

  File file = FFat.open(path, FILE_WRITE);
  if (!file) {
    DBG("Failed to open file for writing\n");
    return;
  }
  if (file.print(_t)) {
    DBG("File written\n");
  } else {
    DBG("Write failed\n");
  }
  file.close();
}

void setup() {
#ifdef VERBOSE
  Serial.begin(115200);
  while (!Serial) delay(1);  // wait for Serial on Leonardo/Zero, etc
#endif

#ifdef CALIBRATE
  // Measure GPIO in order to determine Vref to gpio 25 or 26 or 27
  adc2_vref_to_gpio(GPIO_NUM_25);
  delay(5000);
  abort();
#endif

  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);  // disable brownout detector

  pinInit();

  mqttClient.onConnect(onMqttConnect);
  mqttClient.onPublish(onMqttPublish);

  if (!FFat.begin(true)) {
    DBG("FFat Mount Failed\n");
    return;  // TODO
  }
  DBG("Total space: %10u\n", FFat.totalBytes());
  DBG("Free space: %10u\n", FFat.freeBytes());

  rtc_init();

  DateTime now = rtc.now();
  DBG("RTC date is: %d-%d-%dT%d:%d:%d\n", now.year(), now.month(), now.day(),
      now.hour(), now.minute(), now.second());

  if (rtc.lostPower() || now.dayOfTheWeek() == 0) {
    DBG("RTC is NOT initialized, let's set the time!\n");
    DBG("OR is Sunday!\n");
    setup_wifi();
    getInternetTime();
    rtc.adjust(DateTime(iso8601date));
  }

  if (!rtc.alarmFired())  // Interrupt from button, wait and confirm
  {
    chirp(2);
    DBG("CFG Button pressed, wait 2 sec\n");

    // only wait for the button press if reaches here quickly
    // otherwise it'd indicates internet connection
    if (millis() < 1000) {
      esp_sleep_enable_timer_wakeup(2000000L);  // TODO decide time
      esp_light_sleep_start();
    }

    if (!rtc.alarmFired())  // sanity check -> if button is released, power OFF
    {
      chirp(2);
      capSensorSense = read_moisture();
      writeThreshold(capSensorSense);
      /* TODO, check time when button is pressed
      getInternetTime();
      rtc.adjust(DateTime(iso8601date));
      rtc.setAlarm(18, 00);
      */

      // play(BUZZER_CHANNEL, Skala);

      chirp(5);
      powerOff(now);

      // if button is kept pressed, sleep
      esp_sleep_enable_timer_wakeup(2000000L);  // TODO decide time
      esp_deep_sleep_start();
    }
  }

  capSensorThrs = readThreshold();
  capSensorSense = read_moisture();
  uint16_t batt = getBattmilliVcc();

  DBG("Threshold is %d\n", capSensorThrs);

  // high is dry
  if (capSensorSense > capSensorThrs) {
    setup_wifi();

    DBG("DRY\n");

    // play(BUZZER_CHANNEL, Urgent);

    chirp(9);
    delay(350);
    chirp(1);
    delay(50);
    chirp(1);

    // Home Assistant -> on means moisture detected (wet), off means no moisture
    // (dry)
    char topic[strlen("/states/binary_sensor._moisture") + strlen(DEVICE_NAME) +
               1];
    snprintf(topic, sizeof(topic), "/states/binary_sensor.%s_moisture",
             DEVICE_NAME);

    char payload[160];
    snprintf(
        payload, sizeof(payload),
        "{\"to\": \"%s\",\"state\": \"off\",\"attributes\": "
        "{\"friendly_name\": \"%s_moisture\",\"device_class\": \"moisture\"}}",
        TO, DEVICE_NAME);

    // client.publish(topic, payload);
    mqttClient.publish(topic, 2, false, payload);

    while (published == false && millis() < 10000L) {
      delay(50);
    }
    published = false;
  } else if (batt < 2200) {  // TODO https://data.energizer.com/PDFs/CR2_EU.pdf
    DBG("Low battery\n");

    setup_wifi();

    chirp(9);
    delay(350);
    chirp(1);
    delay(50);
    chirp(1);

    // Home Assistant -> on means low, off means normal
    char topic[strlen("/states/binary_sensor._batt") + strlen(DEVICE_NAME) + 1];
    snprintf(topic, sizeof(topic), "/states/binary_sensor.%s_batt",
             DEVICE_NAME);

    char payload[160] = "";
    snprintf(payload, sizeof(payload),
             "{\"to\": \"%s\",\"state\": \"on\",\"attributes\": "
             "{\"friendly_name\": \"%s_batt\",\"device_class\": \"battery\"}}",
             TO, DEVICE_NAME);

    // client.publish(topic, payload);
    mqttClient.publish(topic, 2, false, payload);

    while (published == false && millis() < 10000L) {
      delay(50);
    }
  }

  powerOff(now);

  DBG("Should never get here\n");
}

void loop() {}