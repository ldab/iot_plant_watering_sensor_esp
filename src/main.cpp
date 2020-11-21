/******************************************************************************
main.cpp
Connected, IoT, WiFi capacitance plant watering sensor based on ESP32
Leonardo Bispo
Nov, 2020
https://github.com/ldab/iot_plant_watering_sensor_esp
Distributed as-is; no warranty is given.
******************************************************************************/

#include <Arduino.h>

#include "secrets.h"

#include <Ticker.h>

#include <WiFi.h>
#include <PubSubClient.h>

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

#define C_SENSE         A4                //GPIO32 - IO32/ADC1_CH4
#define PWM             A17               //GPIO27, RTC_GPIO17
#define BUZZER          A18               //GPIO25, RTC_GPIO6

#define SLEEP_TIME_S    (8 * 60 * 60)     //seconds

// initialize the MQTT Client
WiFiClient espClient;
PubSubClient client(espClient);

// Timer instances
Ticker mqtt_rc;
Ticker temp_reader;

void setup_wifi()
{
  delay(10);

  if (WiFi.SSID() != wifi_ssid)
  {
    Serial.print("\nConnecting to ");
    Serial.println(wifi_ssid);

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
    Serial.print("PROBLEM!!");
  }
  else
  {
    randomSeed(micros());

    Serial.println("\nWiFi connected");
    Serial.println("IP address: ");
    Serial.println(WiFi.localIP());
  }
}

void callback(char* topic, byte* payload, uint32_t length)
{
  char _payload[length];

  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  for (uint32_t i = 0; i < length; i++)
  {
    _payload[i] = (char)payload[i];

    Serial.print(_payload[i]);
  }
  Serial.println();

  if( strcmp(topic, sub_topic) )
  {
    temp_setpoint = atof( _payload );
  }
}

void reconnect()
{
    Serial.print("Attempting MQTT connection...");

    String clientId = "ESP32-";
    clientId += WiFi.macAddress();

    // Attempt to connect
    if (client.connect(clientId.c_str(), mqtt_user, mqtt_pass))
    {
      Serial.println("connected");
      // Once connected, subscribe to topic
      client.subscribe( sub_topic );
    }
    else
    {
      Serial.print("failed, rc=");
      Serial.print(client.state());
    }
  
  client.connected();
}

void read_temp()
{   
  temp = 0;
  //temp = thermocouple.readCelsius();
  if( isnan(temp) )
  {
    Serial.println("Something wrong with thermocouple!");
  } 
  else 
  {
    Serial.print("C = ");
    Serial.println(temp);
  }
}

void chirp()
{
  // radio off to save power
  btStop();
  WiFi.mode( WIFI_OFF );

  //ligth sleep 0.8mA https://lastminuteengineers.com/esp32-sleep-modes-power-consumption/
  esp_sleep_enable_timer_wakeup( 666 );
  esp_light_sleep_start();
}

void read_moisture()
{
  uint8_t pwm_channel    = 0;
  uint8_t pwm_pin        = 0;
  double  pwn_freq       = 1000000; // 1Mhz
  uint8_t pwm_resolution = 8; //0 - 255

  ledcSetup(pwm_channel, pwn_freq, pwm_resolution);
  ledcAttachPin(pwm_pin, pwm_channel);
  ledcWrite(pwm_channel, ((2 ^ pwm_resolution) - 1) / 2);

  if ( false )
  {
    client.publish("topic", "DRY", true); //publish retained
    chirp();
    // TODO email or something else
  }
  else
  {
    client.publish("topic", "OK", true);
  }
}

void setup()
{
  Serial.begin(115200);

  while (!Serial) delay(1); // wait for Serial on Leonardo/Zero, etc

  Serial.println("MAX31855 test");

  // wait for MAX chip to stabilize
  delay(500);

  Serial.print("Initializing sensor...");

  // ...

  Serial.println("DONE.");

  setup_wifi();
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
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