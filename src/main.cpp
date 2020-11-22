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
#define PWM             A14               //GPIO13
#define BUZZER          A15               //GPIO12
#define SDA_PIN         A13               //GPIO15
#define SCL_PIN         A10               //GPIO04

#define SLEEP_TIME_S    (8 * 60 * 60)     //seconds

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

int16_t capSensorThrs = -1;
char iso8601date[21]  = "";

// Timer instances
Ticker mqtt_rc;
Ticker temp_reader;

void setup_wifi()
{
  delay(10);

  if (WiFi.SSID() != wifi_ssid)
  {
    DBG("\nConnecting to %s\n", wifi_ssid);

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);

    int32_t wifi_channel  = WiFi.channel();
    uint8_t wifi_bssid[6];    //{0xF8, 0xD1, 0x11, 0x24, 0xB3, 0x84};

    WiFi.begin(wifi_ssid, wifi_password, wifi_channel, wifi_bssid );

    WiFi.persistent( true );
  }

  if (WiFi.waitForConnectResult() != WL_CONNECTED)
  {
    //TODO
    DBG("\n PROBLEM!! \n");
  }
  else
  {
    randomSeed(micros());

    DBG("\nWiFi connected\n");
    DBG("IP address: %X\n", WiFi.localIP());
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
    DBG("C = %d\n", temp);
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

  capSensorThrs = analogRead(C_SENSE);

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

  int _index = response.indexOf("\"currentDateTime\": ");

  // "currentDateTime": "2020-11-22T11:10Z",
  response.substring(_index + 20, _index + 20 + 17);
  // RTC lib expect seconds, so insert 00 to avoid trouble
  response += ":00Z";

  response.toCharArray(iso8601date, sizeof(iso8601date));  
}

void pinInit()
{
  pinMode(BUZZER, OUTPUT);

  ledcSetup(PWM_CHANNEL, PWM_FREQUENCY, PWM_RESOLUTION);
  ledcAttachPin(PWM, PWM_CHANNEL);
  
  pinMode(C_SENSE, INPUT);                // ADC -> not needed
  adc1_config_width( ADC_WIDTH_BIT_11 );  // Reduce ADC resolution due to reported noise on 12 bits
  adc1_config_channel_atten( ADC1_CHANNEL_5, ADC_ATTEN_DB_11 );   // -11dB attenuation (ADC_ATTEN_DB_11) gives full-scale voltage 3.6V

  esp_adc_cal_characteristics_t *adc_chars = new esp_adc_cal_characteristics_t;
  esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_11, 1098, adc_chars);  // !! TODO calibrate Vref

  analogSetCycles(8);  // default is 8 and seems OK
  analogSetSamples(1); // default is 1
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
}

int16_t readThrs(void)
{
  const char * path = "/threshold.txt";

  DBG("Reading file: %s\r\n", path);
  
  if(!FFat.begin())
  {
    DBG("FFat Mount Failed\n");
    return -1;
  }
  DBG("Total space: %10u\n", FFat.totalBytes());
  DBG("Free space: %10u\n", FFat.freeBytes());

  File file = FFat.open(path);
  if(!file || file.isDirectory())
  {
    DBG("- failed to open file for reading\n");
    return -1;
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

void deleteFile(void)
{
  const char * path = "/threshold.txt";
  
  DBG("Deleting file: %s\n", path);
  if(FFat.remove(path)){
      DBG("File deleted\n");
  } else {
      DBG("Delete failed\n");
  }
}

void writeFile(int16_t _t)
{
  deleteFile();

  const char * path = "/threshold.txt";

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
    // Measure GPIO in order to determine Vref GPIO_NUM_4 == SCL -> R7
    adc2_vref_to_gpio( GPIO_NUM_4 );
    delay(5000);
    abort();
  #endif

  capSensorThrs = readThrs();

  setup_wifi();
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);

  if (rtc.lostPower())
  {
    DBG("RTC is NOT initialized, let's set the time!\n");
    // When time needs to be set on a new device, or after a power loss, the
    // following line sets the RTC to the date & time this sketch was compiled
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    // This line sets the RTC with an explicit date & time, for example to set
    // January 21, 2014 at 3am you would call:
    // rtc.adjust(DateTime(2014, 1, 21, 3, 0, 0));
    //
    // Note: allow 2 seconds after inserting battery or applying external power
    // without battery before calling adjust(). This gives the PCF8523's
    // crystal oscillator time to stabilize. If you call adjust() very quickly
    // after the RTC is powered, lostPower() may still return true.

    rtc.adjust(DateTime(iso8601date));
  }

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