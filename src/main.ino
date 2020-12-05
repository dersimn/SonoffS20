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

#define APP_PREFIX "SonoffS20"
const String s = "";
const String ESP_ID = upperCaseStr(String(ESP.getChipId(), HEX));
const String BOARD_ID = s+APP_PREFIX+"_"+ESP_ID;
const String MQTT_PREFIX = s+"dersimn/"+APP_PREFIX+"/"+ESP_ID;

WiFiClient        espClient;
PubSubClient      mqttClient(MQTT_SERVER, 1883, espClient);
PubSubClientTools mqtt(mqttClient);

LogHandler logHandler;
LogSerialModule serialModule(115200);

GenericLog Log    (logHandler);
NamedLog   LogWiFi(logHandler, "WiFi");
NamedLog   LogMqtt(logHandler, "MQTT");

ThreadController threadControl = ThreadController();
Thread threadMqtt = Thread();
Thread threadUptime = Thread();
Thread threadState = Thread();
ThreadRunOnce threadMqttRunOnce = ThreadRunOnce();

bool buttonToggle = true;
bool isButtonPressed;
long lastUpdateMillis;

void setup() {
  logHandler.addModule(&serialModule);
  Serial.println();
  Log.info(s+"Initializing "+BOARD_ID);
  Log.info(s+"Git HASH: "+GIT_HASH);
  Log.info(s+"Git Tag/Branch: "+GIT_TAG_OR_BRANCH);
  Log.info(s+"Build timestamp: "+BUILD_TIMESTAMP);

  // -------------------------- App Important --------------------------
  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, LOW); // On during init

  pinMode(RELAIS_PIN, OUTPUT);

  pinMode(BUTTON_PIN, INPUT);
  attachInterrupt(BUTTON_PIN, handleKey, RISING);

  // -------------------------- MQTT (1) --------------------------
  threadMqttRunOnce.onRun([](){
    if (WiFi.status() == WL_CONNECTED && !mqtt.connected()) {
      mqttReconnect();
    }
  });
  threadControl.add(&threadMqttRunOnce);

  // -------------------------- Wifi --------------------------
  LogWiFi.info(s+"Connecting to SSID: "+WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.softAPdisconnect(true);
  WiFi.hostname(BOARD_ID);

  static WiFiEventHandler gotIpEventHandler = WiFi.onStationModeGotIP([](const WiFiEventStationModeGotIP& event) {
    LogWiFi.info(s+"(Re)connected with IP: "+WiFi.localIP().toString());
    threadMqttRunOnce.setRunOnce(3000);
  });

  static WiFiEventHandler disconnectedEventHandler = WiFi.onStationModeDisconnected([](const WiFiEventStationModeDisconnected& event) {
    LogWiFi.warn("Connection lost");
    digitalWrite(STATUS_LED_PIN, LOW);
  });

  WiFi.begin(WIFI_SSID, WIFI_PASS);

  // -------------------------- OTA --------------------------
  ArduinoOTA.setHostname(BOARD_ID.c_str());
  ArduinoOTA.begin();

  // -------------------------- MQTT (2) --------------------------
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

    mqtt.publish(s+MQTT_PREFIX+"/maintenance/uptime", doc.as<String>());
  });
  threadUptime.setInterval(MAINTENANCE_INTERVAL);
  threadControl.add(&threadUptime);

  // -------------------------- App --------------------------
  mqtt.subscribe(s+MQTT_PREFIX+"/set/onoff", [](String topic, String message){
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

  mqtt.subscribe(s+MQTT_PREFIX+"/set/led", [](String topic, String message){
    StaticJsonDocument<500> doc;
    auto error = deserializeJson(doc, message);
    if (error) {
      Log.error(s+"deserializeJson() failed with code "+error.c_str());
      return;
    }

    if ( doc.is<bool>() ) {
      led_set( doc.as<bool>() );
      return;
    }
    if ( doc.is<JsonObject>() ) {
      JsonObject rootObject = doc.as<JsonObject>();
      if ( rootObject.containsKey("val") ) {
        led_set( rootObject["val"].as<bool>() );
      }
    }
  });

  mqtt.subscribe(s+MQTT_PREFIX+"/set/config", [](String topic, String message){
    StaticJsonDocument<500> doc;
    auto error = deserializeJson(doc, message);
    if (error) {
      Log.error(s+"deserializeJson() failed with code "+error.c_str());
      return;
    }

    if ( doc.is<JsonObject>() ) {
      JsonObject rootObject = doc.as<JsonObject>();

      if ( rootObject.containsKey("button-toggle") ) {
        buttonToggle = rootObject["button-toggle"].as<bool>();
      }

      conf_pub();
    }
  });

  threadState.onRun([](){
    val_pub();
    led_pub();
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
    btn_pub();

    if (buttonToggle) {
      digitalWrite(RELAIS_PIN, !digitalRead(RELAIS_PIN));
      val_pub();
    }
    
    isButtonPressed = false;
    lastUpdateMillis = millis();
  }
}

void btn_pub() {
  StaticJsonDocument<500> doc;
  
  doc["val"] = true;

  mqtt.publish(s+MQTT_PREFIX+"/status/button", doc.as<String>());
}

void conf_pub() {
  StaticJsonDocument<500> doc;
  
  doc["button-toggle"] = buttonToggle;

  mqtt.publish(s+MQTT_PREFIX+"/status/config", doc.as<String>(), true);
}

void led_set(bool newState) {
  digitalWrite(STATUS_LED_PIN, newState);
  led_pub();
}
void led_pub() {
  StaticJsonDocument<500> doc;
  
  doc["val"] = (bool)digitalRead(STATUS_LED_PIN);

  mqtt.publish(s+MQTT_PREFIX+"/status/led", doc.as<String>(), true);
}

void val_set(bool newState) {
  digitalWrite(RELAIS_PIN, newState);
  val_pub();
}
void val_pub() {
  StaticJsonDocument<500> doc;
  
  doc["val"] = (bool)digitalRead(RELAIS_PIN);

  mqtt.publish(s+MQTT_PREFIX+"/status/onoff", doc.as<String>(), true);
}

void mqttReconnect() {
  LogMqtt.info(s+ "Connecting to "+MQTT_SERVER);
  
  if (mqtt.connect(BOARD_ID, s+MQTT_PREFIX+"/online", 0, true, "false")) {
    LogMqtt.info(s+"Connected and (re)subscribed to "+mqtt.resubscribe()+" topic(s)");
    digitalWrite(STATUS_LED_PIN, HIGH);

    mqtt.publish(s+MQTT_PREFIX+"/online", "true", true);

    // Post static info once every (re)connect
    StaticJsonDocument<500> doc;

    doc["board_id"] = BOARD_ID;
    doc["build_hash"] = GIT_HASH;
    doc["build_tag"] = GIT_TAG_OR_BRANCH;
    doc["build_timestamp"] = BUILD_TIMESTAMP;
    doc["ip_address"] = WiFi.localIP().toString();

    mqtt.publish(s+MQTT_PREFIX+"/maintenance/info", doc.as<String>(), true);

    // Post current state
    val_pub();
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