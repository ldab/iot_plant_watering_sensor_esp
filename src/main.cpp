/******************************************************************************
main.cpp
Connected, IoT, WiFi capacitance plant watering sensor based on ESP32
Leonardo Bispo
Nov, 2020
https://github.com/ldab/iot_plant_watering_sensor_esp
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
#include "HttpClient.h"
#include "PubSubClient.h"
#include "RTClib.h"

#ifdef VERBOSE
  #define DBG(msg, ...)     { Serial.printf("[%ld] " msg , millis(), ##__VA_ARGS__); }
#else
  #define DBG(...)
#endif

// MQTT reconnection timeout in ms
#define MQTT_RC_TIMEOUT 5000

// Temperature reading timer in seconds
#define TEMP_TIMEOUT    1

// Update these with values suitable for your network.
const char* wifi_ssid     = s_wifi_ssid;
const char* wifi_password = s_wifi_password;
const char* mqtt_server   = s_mqtt_server;
const char* mqtt_user     = s_mqtt_user;
const char* mqtt_pass     = s_mqtt_pass;
uint16_t    mqtt_port     = s_mqtt_port;

const char* sub_topic = "chirp/water";
double temp           = NAN;
float  temp_setpoint  = NAN;

#define C_SENSE         A5                //GPIO33 - IO33/ADC1_CH5
#define BATT_ADC        A4                //GPIO32 - IO32/ADC1_CH4
#define BATT_EN         A11               //GPIO0
#define PWM             A14               //GPIO13
#define BUZZER          A15               //GPIO12
#define SDA_PIN         A13               //GPIO15
#define SCL_PIN         A10               //GPIO04

#define PWM_CHANNEL     0
#define PWM_FREQUENCY   1000000L          // 1MHz
#define PWM_RESOLUTION  8                 // bits     

// initialize the MQTT Client
WiFiClient espClient;
PubSubClient client(espClient);

// HTTP clients
HttpClient http_time(espClient, "worldclockapi.com");
HttpClient http_mail(espClient, "emailserver.com");

// RTC instance
RTC_PCF8563 rtc;

int16_t capSensorThrs  = -1;
int16_t capSensorSense = -1;
char iso8601date[]     = "2000-01-01T00:00:00";

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
    WiFi.setAutoReconnect(true);

    int32_t wifi_channel  = WiFi.channel();
    uint8_t wifi_bssid[6];    //{0xF8, 0xD1, 0x11, 0x24, 0xB3, 0x84};

    WiFi.begin(wifi_ssid, wifi_password, wifi_channel, wifi_bssid );

    WiFi.persistent( true );
  }

  while ( millis() < 20000 )
  {
    int8_t wifi_result = WiFi.waitForConnectResult();
    if ( wifi_result == WL_CONNECTED) break;
    DBG("\n WiFi connect failed: %d\n", wifi_result);
  }
  if(WiFi.status() == WL_CONNECTED)
  {
    randomSeed(micros());
    DBG("\nWiFi connected\n");
  }
}

void callback(char* topic, byte* payload, uint32_t length)
{
  char _payload[length];

  DBG("Message arrived [%s]", topic);

  for (uint32_t i = 0; i < length; i++)
  {
    _payload[i] = (char)payload[i];

    DBG("%c", _payload[i]);
  }
  DBG("\n");

  if( strcmp(topic, sub_topic) )
  {
    temp_setpoint = atof( _payload );
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
    client.subscribe( sub_topic );
  }
  else
  {
    DBG("failed, rc=%d", client.state());
  }
  
  client.connected();
}

void read_temp()
{   
  temp = 0;
  //temp = thermocouple.readCelsius();
  if( isnan(temp) )
  {
    DBG("Something wrong with thermocouple!\n");
  } 
  else 
  {
    DBG("C = %f\n", temp);
  }
}

void inline static beep()
{
  digitalWrite(BUZZER, HIGH);
  delay(42);
  digitalWrite(BUZZER, LOW);
}

void static chirp(uint8_t times)
{
  /*
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

void read_moisture()
{

  ledcWrite(PWM_CHANNEL, ((2 ^ PWM_RESOLUTION) - 1) / 2);

  capSensorSense = analogRead(C_SENSE);

  ledcWrite(PWM_CHANNEL, 0);

  if ( false )
  {
    client.publish("topic", "DRY", true); //publish retained
    // TODO email or something else

    WiFi.mode(WIFI_OFF);

    chirp(9);
    delay(350);
    chirp(1);
    delay(50);
    chirp(1);
  }
  else
  {
    client.publish("topic", "OK", true);
  }
}

void getInternetTime()
{
  // http://worldclockapi.com/api/json/utc/now
  
  http_time.beginRequest();

  http_time.get("/api/json/cet/now");

  // or get from home assistant
  //String _bearer = "Bearer ";
  //_bearer.concat( S_ha_token );
  //http_time.sendHeader("Authorization", _bearer);

  http_time.endRequest();

  int statusCode = http_time.responseStatusCode();

  DBG("GET Status code: %d\n", statusCode);

  if (statusCode != 200)
  {
    //PROBLEM
    //sleep
    return;
  }
  
  http_time.skipResponseHeaders();

  String response = http_time.responseBody();

  String search = "\"currentDateTime\":\"";
  int _index = response.indexOf(search);

  // "currentDateTime":"2020-11-22T22:34+01:00"
  String _iso8601date = response.substring(_index + search.length(), _index + search.length() + 16);

  // RTC lib expect seconds, so insert 00 to avoid trouble <-> "2000-01-01T00:00:00"
  _iso8601date.toCharArray(iso8601date, strlen(iso8601date) - 2);
  iso8601date[16] = ':';
  iso8601date[17] = '0';
  iso8601date[18] = '0';
  DBG("time ISO from HTTP is %s\n", iso8601date);  
}

void pinInit()
{
  pinMode(BUZZER, OUTPUT);

  pinMode(BATT_EN, OUTPUT_OPEN_DRAIN);

  ledcSetup(PWM_CHANNEL, PWM_FREQUENCY, PWM_RESOLUTION);
  ledcAttachPin(PWM, PWM_CHANNEL);
  
  pinMode(C_SENSE, INPUT);                // ADC -> not needed
  pinMode(BATT_ADC, INPUT);                // ADC -> not needed
  adc1_config_width( ADC_WIDTH_BIT_11 );  // Reduce ADC resolution due to reported noise on 12 bits
  adc1_config_channel_atten( ADC1_CHANNEL_5, ADC_ATTEN_DB_11 );   // -11dB attenuation (ADC_ATTEN_DB_11) gives full-scale voltage 3.6V
  adc1_config_channel_atten( ADC1_CHANNEL_4, ADC_ATTEN_DB_11 );   // -11dB attenuation (ADC_ATTEN_DB_11) gives full-scale voltage 3.6V

  esp_adc_cal_characteristics_t *adc_chars = new esp_adc_cal_characteristics_t;
  esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_11, 1098, adc_chars);  // !! TODO calibrate Vref

  analogSetCycles(8);  // default is 8 and seems OK
  analogSetSamples(1); // default is 1
}

uint16_t getBattmilliVcc()
{
  uint8_t raw;
  digitalWrite(BATT_EN, LOW);
  delay(50); // ??
  raw = analogRead(BATT_ADC);
  return((raw * 3600 / 2^11) *2);
}

void RTC_init()
{
  DBG("RTC init\n");
  
  Wire.begin(SDA_PIN, SCL_PIN, 100000L);
  if (! rtc.begin())
  {
    DBG("Couldn't find RTC\n");
    delay(100);
    abort();
  }

  // Disable CLKOUT @ 200nA vs 550nA when active
  rtc.writeSqwPinMode( PCF8563_SquareWaveOFF );
}

int16_t readThrs(void)
{
  const char * path = "/threshold.txt";

  DBG("Reading file: %s\r\n", path);

  File file = FFat.open(path);
  if(!file || file.isDirectory())
  {
    DBG("- failed to open file for reading\n");
    return -2;
  }

  DBG("Read from file: ");
  
  uint8_t i = 0;
  char    t[5] = ""; 

  while(file.available())
  {
    t[i] = file.read();
    DBG("%c", t[i]);
    i++;
  }
  file.close();

  return atoi(t);
}

void deleteFile(const char * path)
{
  DBG("Deleting file: %s\n", path);
  if(FFat.remove(path)){
      DBG("File deleted\n");
  } else {
      DBG("Delete failed\n");
  }
}

void writeFile(int16_t _t)
{
  const char * path = "/threshold.txt";

  deleteFile(path);

  DBG("Writing file: %s\n", path);

  File file = FFat.open(path, FILE_WRITE);
  if(!file){
    DBG("Failed to open file for writing\n");
    return;
  }
  if(file.print(_t)){
    DBG("File written\n");
  } else {
    DBG("Write failed\n");
  }
  file.close();
}

void setup()
{
  #ifdef VERBOSE
    Serial.begin(115200);
    while (!Serial) delay(1); // wait for Serial on Leonardo/Zero, etc
  #endif

  #ifdef CALIBRATE
    // Measure GPIO in order to determine Vref GPIO_NUM_12 == BUZZER -> R11
    adc2_vref_to_gpio( GPIO_NUM_12 );
    delay(5000);
    abort();
  #endif

  if(!FFat.begin())
  {
    DBG("FFat Mount Failed\n");
    return;
  }
  DBG("Total space: %10u\n", FFat.totalBytes());
  DBG("Free space: %10u\n", FFat.freeBytes());

  if (rtc.lostPower())
  {
    DBG("RTC is NOT initialized, let's set the time!\n");
    setup_wifi();
    getInternetTime();
    rtc.adjust(DateTime(iso8601date));

    rtc.setAlarm(11, 00);
  }

  if( !rtc.alarmFired() )    // Interrupt from button, wait and confirm
  {
    esp_sleep_enable_timer_wakeup( 1000000L );
    esp_light_sleep_start();
    
    if( !rtc.alarmFired() )  // sanity check
    {
      chirp(2);
      read_moisture();
      writeFile(capSensorSense);

      getInternetTime();
      rtc.adjust(DateTime(iso8601date));

      rtc.setAlarm(18, 00);

      chirp(5);
    }
  }

  capSensorThrs = readThrs();

  if( capSensorThrs == -2 ) // First time, no Threshold loaded
  {
    read_moisture();
    writeFile(capSensorSense);

    getInternetTime();
    rtc.adjust(DateTime(iso8601date));
  }

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);

  // When the RTC was stopped and stays connected to the battery, it has
  // to be restarted by clearing the STOP bit. Let's do this to ensure
  // the RTC is running.
  rtc.start();
}

void loop()
{
  if( !client.connected() )
  {
    temp_reader.detach();
    mqtt_rc.once_ms( MQTT_RC_TIMEOUT, reconnect );
  }
  else
  {
    temp_reader.attach( TEMP_TIMEOUT, read_temp );
    client.loop();
  }

}