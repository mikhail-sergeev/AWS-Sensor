#include <Arduino.h>
#include "secrets.h"
#include <WiFiClientSecure.h>
#include <MQTT.h>
#include <ArduinoJson.h>
#include "WiFi.h"
#include <OneWire.h>
#include <DallasTemperature.h>

#define TIME_TO_SLEEP  10
#define BATCH 15    
#define MAX_SENSORS 3

#define ONE_WIRE_BUS 14
#define BUFFER 100*BATCH*MAX_SENSORS
#define uS_TO_S_FACTOR 1000000ULL  /* Conversion factor for micro seconds to seconds */


// The MQTT topics that this device should publish/subscribe
//#define AWS_IOT_PUBLISH_TOPIC_SENSORS   "$aws/rules/Sensor_BATCH"
#define AWS_IOT_PUBLISH_TOPIC_SENSORS   "esp32/pub"
#define AWS_IOT_PUBLISH_TOPIC_GET   "$aws/things/ESP32/shadow/get"
#define AWS_IOT_PUBLISH_TOPIC_UPDATE   "$aws/things/ESP32/shadow/update"
#define AWS_IOT_SUBSCRIBE_TOPIC "$aws/things/ESP32/shadow/get/accepted"

RTC_DATA_ATTR int bootCount = 0;
RTC_DATA_ATTR int currentmessage = 0;
RTC_DATA_ATTR float sensorvalue[BATCH*MAX_SENSORS];
RTC_DATA_ATTR int sensortimer[BATCH*MAX_SENSORS];
RTC_DATA_ATTR char sensorlist[MAX_SENSORS][20];
RTC_DATA_ATTR char sensornameext[BATCH*MAX_SENSORS][20];
RTC_DATA_ATTR int devicecount = 0;
RTC_DATA_ATTR int batch_size = BATCH;
RTC_DATA_ATTR int sleep_time = TIME_TO_SLEEP;

bool change = false;

OneWire oneWire1(ONE_WIRE_BUS);
DallasTemperature DS18B20_1(&oneWire1);



WiFiClientSecure net = WiFiClientSecure();
MQTTClient client = MQTTClient(BUFFER);

void force_sleep() {
  Serial.println("Force sleep");
  esp_sleep_enable_timer_wakeup(sleep_time * uS_TO_S_FACTOR);
  Serial.flush(); 
  esp_deep_sleep_start();
}

void messageHandler(String &topic, String &payload) {
  StaticJsonDocument<500> doc;
  change = false;
  deserializeJson(doc, payload);
  int b = doc["state"]["desired"]["batch"];
  int s = doc["state"]["desired"]["period"];
  bool valid = true;
  if(b != batch_size) change = true;
  if(s != sleep_time) change = true;
  if(b > BATCH) valid = false;
  if(b < 1) valid = false;
  if(valid) {
    batch_size = b;
    sleep_time = s;
    }
}

void connectAWS()
{
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.println("\nConnecting...");
  Serial.println(ESP.getFreeHeap());
  while (WiFi.status() != WL_CONNECTED){
    delay(500);
    if(esp_timer_get_time()/1000000 > 60) force_sleep();
  }
  // Configure WiFiClientSecure to use the AWS IoT device credentials
  net.setCACert(AWS_CERT_CA);
  net.setCertificate(AWS_CERT_CRT);
  net.setPrivateKey(AWS_CERT_PRIVATE);
  // Connect to the MQTT broker on the AWS endpoint we defined earlier
  client.begin(AWS_IOT_ENDPOINT, 8883, net);
  // Create a message handler
  client.onMessage(messageHandler);
  while (!client.connect(THINGNAME)) {
    delay(100);
    if(esp_timer_get_time()/1000000 > 60) force_sleep();
  }
  if(!client.connected()){
    Serial.println("AWS IoT Timeout!\n");
    return;
  }
  // Subscribe to a topic
  client.subscribe(AWS_IOT_SUBSCRIBE_TOPIC);
  Serial.println("Ok.");
}

void publishSensors() {
  DynamicJsonDocument doc(BUFFER);
  for(int i=0; i<currentmessage; i++) {
    doc[i]["id"] = sensornameext[i];
    doc[i]["value"] = sensorvalue[i];
    doc[i]["time"] = -(batch_size-sensortimer[i]-1)*sleep_time;  
  }
  char jsonBuffer[2048];
  serializeJson(doc, jsonBuffer); // print to client
  client.publish(AWS_IOT_PUBLISH_TOPIC_SENSORS, jsonBuffer); 
}

void sendStatus() {
  StaticJsonDocument<500> doc;
  doc["state"]["reported"]["batch"] = batch_size;
  doc["state"]["reported"]["period"] = sleep_time;
  char jsonBuffer[500];
  serializeJson(doc, jsonBuffer); 
  client.publish(AWS_IOT_PUBLISH_TOPIC_UPDATE, jsonBuffer);
  change = false;
}

void syncStatus() {
  client.publish(AWS_IOT_PUBLISH_TOPIC_GET, "{}");
  for (int i=0;i<10;i++) {
    delay(100);
    client.loop();
  }
  if(change) Serial.println("Status update");
  if(change) sendStatus();
}

void setup() {
  Serial.begin(115200);
  DS18B20_1.begin();
  if (devicecount == 0) {
    Serial.println("Initialize.");
    delay(1000);
    for(int j=0; j<MAX_SENSORS; j++) {
      char buffer [20];
      DeviceAddress tempsensor;
      if(DS18B20_1.getAddress(tempsensor, j)) {
        snprintf ( buffer, 20, "0x%x%x%x%x%x%x%x%x",tempsensor[0],tempsensor[1],tempsensor[2],tempsensor[3],tempsensor[4],tempsensor[5],tempsensor[6],tempsensor[7]);
        Serial.println(buffer);  
        memcpy(sensorlist[j],buffer,sizeof(buffer[0])*20);
        devicecount++;      
      }
    }
    Serial.print("Found ");
    Serial.print(devicecount, DEC);
    Serial.println(" devices.");
    connectAWS();
    syncStatus();
  }

  float s = 0;
  for(int j=0;j<10;j++) s+=analogRead(35);
  s = s/10;
  sensorvalue[currentmessage] = s/4095.0*6.864;
  sensortimer[currentmessage] = bootCount;
  memcpy(sensornameext[currentmessage],"VBAT",sizeof("VBAT"));
  ++currentmessage;

  for(int j=0; j<devicecount; j++) {
   float tempC;
   do {
     DS18B20_1.requestTemperatures(); 
     tempC = DS18B20_1.getTempCByIndex(j);
     delay(100);
   } while (tempC == 85.0 || tempC == (-127.0));
   memcpy(sensornameext[currentmessage],sensorlist[j],sizeof(sensorlist[j][0])*20);
   sensorvalue[currentmessage] = tempC;
   sensortimer[currentmessage] = bootCount;
   ++currentmessage;    
  }
  bootCount++;

  if(bootCount>=batch_size) {
    connectAWS();
    publishSensors();
    syncStatus();
    bootCount = 0;
    currentmessage = 0;
  }
  Serial.print("Wakeup time: ");
  Serial.println(esp_timer_get_time()/1000000.0);
  esp_sleep_enable_timer_wakeup(sleep_time * uS_TO_S_FACTOR);
  Serial.flush(); 
  esp_deep_sleep_start();
}

void loop() {
}