/******************************************************************************
main.cpp
Connected, IoT, WiFi capacitance plant watering sensor based on ESP32
Leonardo Bispo
Nov, 2020
https://github.com/ldab/iot_plant_watering_sensor_esp32
Distributed as-is; no warranty is given.
******************************************************************************/

#include <Arduino.h>

#include "FS.h"
#include "FFat.h"

#include <driver/adc.h>
#include <esp_adc_cal.h>

#include "secrets.h"

#include <Ticker.h>
#include <Wire.h>
#include <WiFi.h>

#include "ArduinoHttpClient.h"
#include "PubSubClient.h"
#include "RTClib.h"

#ifndef DEVICE_NAME
#error Remember to define the Device Name
#elif not defined TO
#error Remember to set the email address
#endif

#ifdef VERBOSE
#define DBG(msg, ...)                                     \
  {                                                       \
    Serial.printf("[%ld] " msg, millis(), ##__VA_ARGS__); \
  }
#else
#define DBG(...)
#endif

// MQTT reconnection timeout in ms
#define MQTT_RC_TIMEOUT 5000

// Temperature reading timer in seconds
#define TEMP_TIMEOUT 1

#define C_SENSE A5  //GPIO33 - IO33/ADC1_CH5
#define BATT_ADC A4 //GPIO32 - IO32/ADC1_CH4
#define BATT_EN A11 //GPIO0
#define PWM A14     //GPIO13
#define BUZZER A15  //GPIO12
#define SDA_PIN A13 //GPIO15
#define SCL_PIN A10 //GPIO04

#define PWM_CHANNEL 0
#define BUZZER_CHANNEL 1
#define PWM_FREQUENCY 1000000L // 1MHz
#define PWM_RESOLUTION 8       // bits

// Update these with values suitable for your network.
const char *wifi_ssid = s_wifi_ssid;
const char *wifi_password = s_wifi_password;
const char *mqtt_server = s_mqtt_server;
const char *mqtt_user = s_mqtt_user;
const char *mqtt_pass = s_mqtt_pass;
uint16_t mqtt_port = s_mqtt_port;

const char *ha_server = "s_ha_server";
uint16_t ha_port = 1883;

const char *sub_topic = "chirp/water";
const char *moisture_topic = "/states/binary_sensor.esp_banana_moisture";
const char *batt_topic = "/states/binary_sensor.esp_banana_batt";

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
PubSubClient client(espClient);

// HTTP clients
HttpClient http_time(espClient, "worldclockapi.com");
HttpClient http_ha(espClient, ha_server, ha_port);
HttpClient http_mail(espClient, "emailserver.com");

// RTC instance
RTC_PCF8563 rtc;

int16_t capSensorThrs = -1;
int16_t capSensorSense = -1;
char iso8601date[] = "2000-01-01T00:00:00";

// Timer instances
Ticker mqtt_rc;
Ticker temp_reader;

void setup_wifi()
{
  delay(10);

  if (WiFi.SSID() != wifi_ssid)
  {
    DBG("Connecting to %s\n", wifi_ssid);

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true); // TODO maybe false as WiFI should start off

    int32_t wifi_channel = WiFi.channel();
    uint8_t wifi_bssid[6]; //{0xF8, 0xD1, 0x11, 0x24, 0xB3, 0x84};

    //WiFi.begin(wifi_ssid, wifi_password, wifi_channel, wifi_bssid);
    WiFi.begin(wifi_ssid, wifi_password);

    WiFi.persistent(true);
  }

  while (millis() < 10000) // TODO
  {
    int8_t wifi_result = WiFi.waitForConnectResult();
    if (wifi_result == WL_CONNECTED)
      break;
    DBG("\n WiFi connect failed: %d\n", wifi_result);
  }
  if (WiFi.status() == WL_CONNECTED)
  {
    randomSeed(micros());
    DBG("\nWiFi connected\n");
  }
}

void callback(char *topic, byte *payload, uint32_t length)
{
  char _payload[length];

  DBG("Message arrived [%s]", topic);

  for (uint32_t i = 0; i < length; i++)
  {
    _payload[i] = (char)payload[i];
    DBG("%c", _payload[i]);
  }
  DBG("\n");

  if (strcmp(topic, sub_topic))
  {
    // DO something
    // temp_setpoint = atof(_payload);
  }
}

void reconnect()
{
  DBG("Attempting MQTT connection...\n");

  String clientId = "ESP32-";
  clientId += WiFi.macAddress();

  // Attempt to connect
  if (client.connect(clientId.c_str(), mqtt_user, mqtt_pass))
  {
    DBG("connected\n");
    // Once connected, subscribe to topic
    client.subscribe(sub_topic);
  }
  else
  {
    DBG("failed, rc=%d", client.state());
  }

  client.connected();
}

void inline static beep()
{
  ledcWrite(BUZZER_CHANNEL, ((uint32_t)pow(2, PWM_RESOLUTION) - 1) / 2);

  //digitalWrite(BUZZER, HIGH);
  delay(42);
  //digitalWrite(BUZZER, LOW);

  ledcWrite(BUZZER_CHANNEL, 0);
}

void static chirp(uint8_t times)
{
  /* TODO
    // radio off to save power
  btStop();
  WiFi.mode( WIFI_OFF );

  //ligth sleep 0.8mA https://lastminuteengineers.com/esp32-sleep-modes-power-consumption/
  esp_sleep_enable_timer_wakeup( 666 );
  esp_light_sleep_start();
  */

  while (times-- > 0)
  {
    beep();
    delay(40);
  }
}

int16_t read_moisture()
{
  int16_t _adc;

  ledcWrite(PWM_CHANNEL, ((2 ^ PWM_RESOLUTION) - 1) / 2);

  delay(100); // TODO test the time required to stabilize

  _adc = analogRead(C_SENSE);

  ledcWrite(PWM_CHANNEL, 0);

  return _adc;
}

void getInternetTime()
{
  uint8_t timezone = 1;
  uint8_t daysavetime = 1;

  configTime(3600 * timezone, daysavetime * 3600, "dk.pool.ntp.org", "0.pool.ntp.org", "1.pool.ntp.org");
  struct tm tmstruct;
  tmstruct.tm_year = 0;
  getLocalTime(&tmstruct, 5000); // TODO timeout
  sprintf(iso8601date, "%d-%02d-%02dT%02d:%02d:%02d\n", (tmstruct.tm_year) + 1900, (tmstruct.tm_mon) + 1,
          tmstruct.tm_mday, tmstruct.tm_hour, tmstruct.tm_min, tmstruct.tm_sec);
  DBG("\nNow is : %s\n", iso8601date);

  http_time.beginRequest();
  // http://worldclockapi.com/api/json/cet/now
  http_time.get("/api/json/cet/now");

  // or get from home assistant
  //String _bearer = "Bearer ";
  //_bearer.concat(S_ha_token);
  //http_time.sendHeader("Authorization", _bearer);

  http_time.endRequest();

  int statusCode = http_time.responseStatusCode();

  DBG("GET Status code: %d\n", statusCode);

  if (statusCode != 200)
  {
    //PROBLEM TODO
    //sleep
    return;
  }

  http_time.skipResponseHeaders();

  String response = http_time.responseBody();

  String search = "\"currentDateTime\":\"";
  int _index = response.indexOf(search);

  // "currentDateTime":"2020-11-22T22:34+01:00"
  String _iso8601date =
      response.substring(_index + search.length(),
                         _index + search.length() + 16);

  // RTC lib expect seconds, so insert 00 to avoid trouble <-> "2000-01-01T00:00:00"
  _iso8601date.toCharArray(iso8601date, strlen(iso8601date) - 2);
  iso8601date[16] = ':';
  iso8601date[17] = '0';
  iso8601date[18] = '0';
  DBG("time ISO from HTTP is %s\n", iso8601date);
}

void sendPost(bool dry, uint16_t battery, String payload)
{
  /*
  curl -X POST -H "Authorization: Bearer ABCDEFGH" \
    -H "Content-Type: application/json" \
    -d '{"state": "25"}' \
    http://localhost:8123/api/states/sensor.kitchen_temperature
  */

  String state = (dry) ? "on" : "off"; // "on" means moisture detected (wet), "off" means no moisture (dry)
  float batt = (float)battery / 1000.0;
  /*
  String postData = "{\"attributes\": {";
  //postData += "\"altitude\": " + alt + ", ";
  postData += "\"friendly_name\": \"" + (String)DEVICE_NAME + "\", ";
  postData += "\"battery\": "         + String(batt, 2) + "}, ";
  postData += "\"state\": \""         + state + "\"}";
*/

  String postData = "{\"state\": \"" + state + "\"}";

  http_ha.beginRequest();
  http_ha.post("/api/states/device_tracker.bike_test");
  http_ha.sendHeader(HTTP_HEADER_CONTENT_TYPE, "application/json");
  http_ha.sendHeader(HTTP_HEADER_CONTENT_LENGTH, postData.length());

  // or get from home assistant
  String _bearer = "Bearer ";
  //_bearer.concat(S_ha_token);
  http_ha.sendHeader("Authorization", _bearer);

  http_ha.endRequest();
  http_ha.print(postData);

  int statusCode = http_ha.responseStatusCode();

  DBG("GET Status code: %d\n", statusCode);

  if (statusCode != 200)
  {
    //PROBLEM TODO
    //sleep
    return;
  }
}

void pinInit()
{
  pinMode(BUZZER, OUTPUT);

  pinMode(BATT_EN, OUTPUT_OPEN_DRAIN);

  pinMode(PWM, OUTPUT);
  ledcAttachPin(PWM, PWM_CHANNEL);
  ledcSetup(PWM_CHANNEL, PWM_FREQUENCY, PWM_RESOLUTION);

  ledcAttachPin(BUZZER, BUZZER_CHANNEL);
  ledcSetup(BUZZER_CHANNEL, PWM_FREQUENCY, PWM_RESOLUTION);

  pinMode(C_SENSE, INPUT);  // ADC -> not needed
  pinMode(BATT_ADC, INPUT); // ADC -> not needed

  adc1_config_width(ADC_WIDTH_BIT_11);                        // Reduce ADC resolution due to reported noise on 12 bits
  adc1_config_channel_atten(ADC1_CHANNEL_5, ADC_ATTEN_DB_11); // -11dB attenuation (ADC_ATTEN_DB_11) gives full-scale voltage 3.6V
  adc1_config_channel_atten(ADC1_CHANNEL_4, ADC_ATTEN_DB_11); // -11dB attenuation (ADC_ATTEN_DB_11) gives full-scale voltage 3.6V

  esp_adc_cal_characteristics_t *adc_chars =
      new esp_adc_cal_characteristics_t;
  esp_adc_cal_characterize(ADC_UNIT_1,
                           ADC_ATTEN_DB_11, ADC_WIDTH_BIT_11,
                           1098, adc_chars); // !! TODO calibrate Vref

  analogSetCycles(8);  // default is 8 and seems OK
  analogSetSamples(1); // default is 1
}

uint16_t getBattmilliVcc()
{
  uint8_t raw;
  digitalWrite(BATT_EN, LOW);
  delay(50); // TODO
  raw = analogRead(BATT_ADC);
  return ((raw * 3600 / 2 ^ 11) * 2);
}

void rtc_init()
{
  DBG("RTC init\n");

  Wire.begin(SDA_PIN, SCL_PIN, 100000L);
  if (!rtc.begin())
  {
    DBG("Couldn't find RTC\n");
    delay(100);
    // abort(); TODO
  }

  // Disable CLKOUT @ 200nA vs 550nA when active
  rtc.writeSqwPinMode(PCF8563_SquareWaveOFF);

  // When the RTC was stopped and stays connected to the battery, it has
  // to be restarted by clearing the STOP bit. Let's do this to ensure
  // the RTC is running.
  rtc.start();
}

void powerOff(DateTime now)
{
  WiFi.disconnect(true); // TODO check if false changes time

  if (now.hour() >= 18)
  {
    rtc.setAlarm(8, 00);
  }
  else
  {
    rtc.setAlarm(18, 00);
  }
  rtc.clearAlarm();
}

int16_t readThreshold(void)
{
  const char *path = "/threshold.txt";

  DBG("Reading file: %s\r\n", path);

  File file = FFat.open(path);
  if (!file || file.isDirectory())
  {
    DBG("- failed to open file for reading\n");
    return -2;
  }

  DBG("Read from file: ");

  uint8_t i = 0;
  char t[5] = "";

  while (file.available())
  {
    t[i] = file.read();
    DBG("%c", t[i]);
    i++;
  }
  DBG("\n");

  file.close();

  return atoi(t);
}

void deleteFile(const char *path)
{
  DBG("Deleting file: %s\n", path);
  if (FFat.remove(path))
  {
    DBG("File deleted\n");
  }
  else
  {
    DBG("Delete failed\n");
  }
}

void writeThreshold(int16_t _t)
{
  const char *path = "/threshold.txt";

  //deleteFile(path); // not needed
  DBG("Writing file: %s\n", path);

  File file = FFat.open(path, FILE_WRITE);
  if (!file)
  {
    DBG("Failed to open file for writing\n");
    return;
  }
  if (file.print(_t))
  {
    DBG("File written\n");
  }
  else
  {
    DBG("Write failed\n");
  }
  file.close();
}

void setup()
{
#ifdef VERBOSE
  Serial.begin(115200);
  while (!Serial)
    delay(1); // wait for Serial on Leonardo/Zero, etc
#endif

#ifdef CALIBRATE
  // Measure GPIO in order to determine Vref GPIO_NUM_12 == BUZZER -> R11
  adc2_vref_to_gpio(GPIO_NUM_12);
  delay(5000);
  abort();
#endif

  if (!FFat.begin(true))
  {
    DBG("FFat Mount Failed\n");
    return; // TODO
  }
  DBG("Total space: %10u\n", FFat.totalBytes());
  DBG("Free space: %10u\n", FFat.freeBytes());

  rtc_init();

  DateTime now = rtc.now();
  DBG("RTC date is: %d-%d-%dT%d:%d:%d", now.year(), now.month(),
      now.day(), now.hour(), now.minute(), now.second());

  if (rtc.lostPower() || now.dayOfTheWeek() == 0)
  {
    DBG("RTC is NOT initialized, let's set the time!\n");
    DBG("OR is SUnday!\n");
    setup_wifi();
    getInternetTime();
    rtc.adjust(DateTime(iso8601date));
  }

  if (!rtc.alarmFired()) // Interrupt from button, wait and confirm
  {
    chirp(2);
    // only wait for the button press if reaches here quickly
    // otherwise it'd indicates internet connection
    if (millis() < 1000)
    {
      esp_sleep_enable_timer_wakeup(2000000L); // TODO decide time
      esp_light_sleep_start();
    }

    if (!rtc.alarmFired()) // sanity check -> if button is released, power OFF
    {
      chirp(2);
      capSensorSense = read_moisture();
      writeThreshold(capSensorSense);
      /* TODO
      getInternetTime();
      rtc.adjust(DateTime(iso8601date));
      rtc.setAlarm(18, 00);
*/
      chirp(5);
      powerOff(now);
    }
  }

  capSensorThrs = readThreshold();
  capSensorSense = read_moisture();
  uint16_t batt = getBattmilliVcc();

  // high is dry
  if (capSensorSense > capSensorThrs)
  {
    DBG("DRY\n");
    chirp(9);
    delay(350);
    chirp(1);
    delay(50);
    chirp(1);

    // Home Assistant -> on means moisture detected (wet), off means no moisture (dry)
    char topic[strlen("/states/binary_sensor.moisture") + strlen(DEVICE_NAME)];
    sprintf(topic, "/states/binary_sensor.%s_moisture", DEVICE_NAME);

    char payload[160];
    sprintf(payload, "{\"to\": \"%s\",\"state\": \"off\",\"attributes\": {\"friendly_name\": \"%s_moisture\",\"device_class\": \"moisture\"}}", TO, DEVICE_NAME);

    client.publish(topic, payload);
  }
  else if (batt < 2000)
  {
    DBG("Low battery\n");
    chirp(9);
    delay(350);
    chirp(1);
    delay(50);
    chirp(1);

    // Home Assistant -> on means low, off means normal
    char topic[strlen("/states/binary_sensor.batt") + strlen(DEVICE_NAME)];
    sprintf(topic, "/states/binary_sensor.%s_batt", DEVICE_NAME);

    char payload[160];
    sprintf(payload, "{\"to\": \"%s\",\"state\": \"on\",\"attributes\": {\"friendly_name\": \"%s_batt\",\"device_class\": \"battery\"}}", TO, DEVICE_NAME);

    client.publish(topic, payload);
  }

  powerOff(now);

  //client.setServer(mqtt_server, mqtt_port);
  //client.setCallback(callback);

  DBG("Should never get here+\n");
}

void loop()
{
  /*
  if (!client.connected())
  {
    temp_reader.detach();
    mqtt_rc.once_ms(MQTT_RC_TIMEOUT, reconnect);
  }
  else
  {
    //temp_reader.attach(TEMP_TIMEOUT, read_temp);
    client.loop();
  }
  */
}