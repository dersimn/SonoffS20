#include "default_config.h"
#include "handy_functions.h"

#include <ESP8266WiFi.h>

#include <GenericLog.h>         // https://github.com/dersimn/ArduinoUnifiedLog
#include <NamedLog.h>
#include <LogHandler.h>
#include <LogSerialModule.h>

#include <Thread.h>             // https://github.com/ivanseidel/ArduinoThread
#include <ThreadController.h>
#include <ThreadRunOnce.h>      // https://github.com/dersimn/ArduinoThreadRunOnce

#include <PubSubClient.h>       // https://github.com/knolleary/pubsubclient
#include <PubSubClientTools.h>  // https://github.com/dersimn/ArduinoPubSubClientTools

#include <ArduinoJson.h>

#include <ArduinoOTA.h>

#define BOARD_ID_PREFIX "SonoffS20_"
const String s = "";
const String ESP_ID = upperCaseStr(String(ESP.getChipId(), HEX));
const String BOARD_ID = s+BOARD_ID_PREFIX+ESP_ID;

WiFiClient        espClient;
PubSubClient      mqttClient(MQTT_SERVER, 1883, espClient);
PubSubClientTools mqtt(mqttClient);

LogHandler logHandler;
LogSerialModule serialModule(115200);

GenericLog Log    (logHandler);
NamedLog   LogWiFi(logHandler, "WiFi");
NamedLog   LogMqtt(logHandler, "MQTT");

ThreadController threadControl = ThreadController();
Thread threadWifi = Thread();
Thread threadMqtt = Thread();
Thread threadUptime = Thread();
Thread threadState = Thread();

bool isButtonPressed;
long lastUpdateMillis;

void setup() {
  logHandler.addModule(&serialModule);
  Log.info(s+"Initializing "+BOARD_ID);

  // -------------------------- App Important --------------------------
  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, LOW); // On during init

  pinMode(RELAIS_PIN, OUTPUT);

  pinMode(BUTTON_PIN, INPUT);
  attachInterrupt(BUTTON_PIN, handleKey, RISING);

  // -------------------------- Wifi --------------------------
  LogWiFi.info(s+"Connecting to SSID: "+WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.softAPdisconnect(true);
  WiFi.hostname(BOARD_ID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  threadWifi.onRun([](){
    static auto lastState = WiFi.status();

    if (WiFi.status() != WL_CONNECTED && lastState == WL_CONNECTED) {
      LogWiFi.warn("Connection lost");
      digitalWrite(STATUS_LED_PIN, LOW);
    } else if (WiFi.status() == WL_CONNECTED && lastState != WL_CONNECTED) {
      LogWiFi.info(s+"(Re)connected with IP: "+WiFi.localIP().toString() );
    }

    lastState = WiFi.status();
  });
  threadWifi.setInterval(MAINTENANCE_INTERVAL);
  threadControl.add(&threadWifi);

  // -------------------------- OTA --------------------------
  ArduinoOTA.setHostname(BOARD_ID.c_str());
  ArduinoOTA.begin();

  // -------------------------- MQTT --------------------------
  if (WiFi.status() == WL_CONNECTED) {
    mqttReconnect();
  }
  threadMqtt.onRun([](){
    static auto lastState = mqtt.connected();

    if (lastState && !mqtt.connected()) {
      LogMqtt(s+"Connection lost");
      digitalWrite(STATUS_LED_PIN, LOW);
    }
    if (WiFi.status() == WL_CONNECTED && !mqtt.connected()) {
      mqttReconnect();
    }

    lastState = mqtt.connected();
  });
  threadMqtt.setInterval(MAINTENANCE_INTERVAL);
  threadControl.add(&threadMqtt);

  // -------------------------- Uptime --------------------------
  threadUptime.onRun([](){
    StaticJsonDocument<500> doc;
    static uint64_t longterm_millis;
    static uint32_t last_millis;
    static uint32_t rollover_count;

    uint32_t this_millis = millis();
    longterm_millis += this_millis - last_millis;
    if (last_millis > this_millis) rollover_count++;
    last_millis = this_millis;

    doc["val"] = longterm_millis;
    doc["millis"] = this_millis;
    doc["rollover"] = rollover_count;

    mqtt.publish(s+BOARD_ID+"/maintenance/uptime", doc.as<String>());
  });
  threadUptime.setInterval(MAINTENANCE_INTERVAL);
  threadControl.add(&threadUptime);

  // -------------------------- App --------------------------
  mqtt.subscribe(s+BOARD_ID+"/set/onoff", [](String topic, String message){
    StaticJsonDocument<500> doc;
    auto error = deserializeJson(doc, message);
    if (error) {
      Log.error(s+"deserializeJson() failed with code "+error.c_str());
      return;
    }

    if ( doc.is<bool>() ) {
      val_set( doc.as<bool>() );
      return;
    }
    if ( doc.is<JsonObject>() ) {
      JsonObject rootObject = doc.as<JsonObject>();
      if ( rootObject.containsKey("val") ) {
        val_set( rootObject["val"].as<bool>() );
      }
    }
  });

  threadState.onRun([](){
    val_pub();
  });
  threadState.setInterval(MAINTENANCE_INTERVAL);
  threadControl.add(&threadState);

  Log.info("Setup done");
}

void loop() {
  threadControl.run();

  ArduinoOTA.handle();
  mqttClient.loop();

  if (isButtonPressed) {
    digitalWrite(RELAIS_PIN, !digitalRead(RELAIS_PIN));
    val_pub();
    
    isButtonPressed = false;
    lastUpdateMillis = millis();
  }
}

void val_set(bool newState) {
  digitalWrite(RELAIS_PIN, newState);
  val_pub();
}
void val_pub() {
  StaticJsonDocument<500> doc;
  
  doc["val"] = (bool)digitalRead(RELAIS_PIN);

  mqtt.publish(s+BOARD_ID+"/status/onoff", doc.as<String>(), true);
}

void mqttReconnect() {
  LogMqtt.info(s+ "Connecting to "+MQTT_SERVER);
  
  if (mqtt.connect(BOARD_ID, s+BOARD_ID+"/maintenance/online", 0, true, "false")) {
    LogMqtt.info(s+"Connected");
    mqtt.publish(s+BOARD_ID+"/maintenance/online", "true", true);

    LogMqtt.info(s+"(Re)Subscribed to "+mqtt.resubscribe()+" topics");
    digitalWrite(STATUS_LED_PIN, HIGH);
  } else {
    LogMqtt.error(s+"Connection failed with rc="+mqttClient.state());
    digitalWrite(STATUS_LED_PIN, LOW);
  }
}

void ICACHE_RAM_ATTR handleKey() {
  if (millis() - lastUpdateMillis > BUTTON_DEBOUNCE) {
    isButtonPressed = true;
  }
}