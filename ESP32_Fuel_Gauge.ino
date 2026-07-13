// ESP32 Fuel Gauge with MQTT and FastLED
// Written by Noah Christian 2026 (C) 2026
//
// This code connects to a Wi-Fi network, subscribes to MQTT topics, and controls an LED strip based on battery state of charge (SoC) and charging status.
// It uses the FastLED library to manage the LED strip and the ArduinoMqttClient library for MQTT communication.
// Make sure to define your Wi-Fi credentials and MQTT broker details in the arduino_secrets.h file.
//
// arduino_secrets.h should contain the following definitions:
// #define SECRET_SSID "YourWiFiSSID"
// #define SECRET_PASS "YourWiFiPassword"
// #define HOME_ASSISTANT_IP "YourHomeAssistantIP"
// #define MQTT_USERNAME "YourMQTTUsername"
// #define MQTT_PASSWORD "YourMQTTPassword"
//
// The LED strip is defined to have 40 LEDs connected to pin 33. Adjust NUM_LEDS and DATA_PIN as needed for your setup.
// The code subscribes to three MQTT topics:
// 1. "V1.0/Home/Battery/SoC" - expects a float value representing the battery state of charge (0-100).
// 2. "V1.0/Home/Battery/Action" - expects a string value: "Charging", "Discharging", or "Idle".
// 3. "V1.0/Home/Battery/Brightness"  - expects a float value (0-100) to set the brightness of the LED strip.
// SoC is represented visually on the LED strip, with blue LEDs indicating the charge level. When charging, a yellow animation runs; when discharging, a purple animation runs. The brightness of the LED strip can be adjusted via MQTT.
// Make sure to install the required libraries: FastLED and ArduinoMqttClient.
// Note: This code is designed for the ESP32 platform and may require adjustments for other microcontrollers.
// Include necessary libraries
//
// LEDs are connected as a WS2812 strip, which requires a single data line. The FastLED library is used to control the LEDs, and the ArduinoMqttClient library is used for MQTT communication. The code handles Wi-Fi connection, MQTT subscription, and LED control based on received messages.
// Uses a 5V LED strip and connecting Vcc to the 5V pin, GND to the ground pin, and connect data to pin 33. No level shifting is used because 3.3V is sufficient for the data line.
//
// Upon the first run, the LED strip is off until messages are received from Home Assistant.
// Some values such as the number of LED's, colors, etc can be changed but are currently hardcoded.
//

#include <FastLED.h>
#include <WiFi.h>
#include "arduino_secrets.h"
#include <ArduinoMqttClient.h>

const char ssid[] = SECRET_SSID;
const char password[] = SECRET_PASS;

using namespace fl;
uint8_t verbosity = 255;
bool trace = true;

float f_bright = 20;
float last_bright = 0;

// Connection timeout in seconds
const unsigned long WIFI_TIMEOUT = 15;

//MQTT (Message Queuing Telemetry Transport)
//create the objects
WiFiClient wifiClient;
MqttClient mqttClient(wifiClient);

//define broker, port and topic
//arduino doesn't allow local namespaces
const char broker[] = HOME_ASSISTANT_IP; //not sure why not working name on local: "homeassistant";
int        port     = 1883;
//Version/Enterprise/Site/Area/Line/Device
const char subtopic1[]  = "V1.0/Home/Battery/SoC";
const char subtopic2[]  = "V1.0/Home/Battery/Action";
const char subtopic3[]  = "V1.0/Home/Battery/Brightness";

float f_SoC = 0.0;
float last_SoC_drawn = -1.0; //separate from f_SoC so f_SoC always stays a true 0-100 percentage
uint8_t lit_leds = 0; //count of LEDs currently lit blue, derived from f_SoC each redraw
int currentState = 0;//neither charging (1) or discharging (-1)
//end of Mosquitto

// How many leds in your strip?
#define NUM_LEDS 40 

// For led chips like Neopixels, which have a data line, ground, and power, you just
// need to define DATA_PIN.  For led chipsets that are SPI based (four wires - data, clock,
// ground, and power), like the LPD8806, define both DATA_PIN and CLOCK_PIN
#define DATA_PIN 33
//#define CLOCK_PIN 13

// Define the array of leds
CRGB leds[NUM_LEDS];

void connectToWiFi(void){
  Serial.println("\nConnecting to Wi-Fi...");
  WiFi.mode(WIFI_STA); // Station mode (connect to existing network)
  WiFi.begin(ssid, password);

  unsigned long startAttemptTime = millis();

  // Attempt to connect until timeout
  while (WiFi.status() != WL_CONNECTED &&
         millis() - startAttemptTime < WIFI_TIMEOUT * 1000) {
    Serial.print(".");
    delay(500);
  }

  // Check connection result
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ Connected to Wi-Fi!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\n❌ Failed to connect to Wi-Fi.");
    Serial.println("Check SSID/password or signal strength.");
  }
} //connectToWiFi

void setup() { 
	Serial.begin(115200);
	//needs a delay to get started
	delay(1000);
  while (!Serial) {
    ; // Wait for Serial to be ready
  }
	Serial.println("resetting");

	Serial.print("RESET");

	connectToWiFi();
//  WiFi.begin(ssid, password);


	if (verbosity > 0) {
    Serial.print("Attempting to connect to the MQTT broker: ");
    Serial.println(broker);
  }
  
  mqttClient.setUsernamePassword(MQTT_USERNAME, MQTT_PASSWORD);
  if (!mqttClient.connect(broker, port)) {
    if (verbosity > 0) Serial.print("MQTT connection failed! Error code = ");
    if (verbosity > 0) Serial.println(mqttClient.connectError());

    while (1);
  }

  if (verbosity > 0) Serial.println("You're connected to the MQTT broker!");
  
  // set the message receive callback
  mqttClient.onMessage(onMqttMessage);

  // Subscribe to a topic
  if (verbosity > 0) {
    Serial.print("Subscribing to topic: ");
    Serial.println(subtopic1);
    Serial.println();
  }
  // subscribe to a topic
  mqttClient.subscribe(subtopic1);

  // Subscribe to a topic
  if (verbosity > 0) {
    Serial.print("Subscribing to topic: ");
    Serial.println(subtopic2);
    Serial.println();
  }
  // // subscribe to a topic
  mqttClient.subscribe(subtopic2);

  // Subscribe to a topic
  if (verbosity > 0) {
    Serial.print("Subscribing to topic: ");
    Serial.println(subtopic3);
    Serial.println();
  }
  // // subscribe to a topic
  mqttClient.subscribe(subtopic3);

	FastLED.addLeds<WS2812,DATA_PIN,GRB>(leds,NUM_LEDS);
	FastLED.setBrightness(f_bright);
} //setup

void onMqttMessage(int messageSize) {
  char tbuf[256]="";
  int size=0;
	String topic = mqttClient.messageTopic();
  // we received a message, print out the topic and contents
  if (verbosity > 4) {
    Serial.print("Received a message with topic '");
    Serial.print(topic);
    Serial.print("', length ");
    Serial.print(messageSize);
    Serial.print(" bytes: ");
  }
  if(topic.equals(subtopic1)){ //batterylevel
    // use the Stream interface to print the contents
    while (mqttClient.available() && size < (int)sizeof(tbuf) - 1) {
      tbuf[size]=(char)mqttClient.read();
      size++;
    }
    while (mqttClient.available()) mqttClient.read(); //discard anything beyond tbuf's capacity so the next message stays in sync
    tbuf[size]=NULL;
    if (verbosity > 4) Serial.print(String(tbuf));
    if (verbosity > 4) Serial.println();
    //set number based on input, clamped so a malformed/out-of-range publish
    //  (e.g. "1000") can't overflow lit_leds past NUM_LEDS in loop()
    f_SoC = constrain(atof(tbuf), 0.0, 100.0); //atof avoids a heap-allocating String just to parse a float
    if (trace) {Serial.print("f_SoC = "); Serial.println(f_SoC,3);}
  }
  if(topic.equals(subtopic2)){
    // use the Stream interface to print the contents
    while (mqttClient.available() && size < (int)sizeof(tbuf) - 1) {
      tbuf[size]=(char)mqttClient.read();
      size++;
    }
    while (mqttClient.available()) mqttClient.read();
    tbuf[size]=NULL;
    if (verbosity > 4) Serial.print(String(tbuf));
    if (verbosity > 4) Serial.println();
    if (!strcmp(tbuf,"Discharging")){
			currentState = -1;
		} else if (!strcmp(tbuf,"Charging")){
			currentState = 1;
		} else if (!strcmp(tbuf,"Idle")){
			currentState = 0;
      if (trace) {Serial.print(currentState); Serial.println(" set\n");}
		}
  }
  if(topic.equals(subtopic3)){
    // use the Stream interface to print the contents
    while (mqttClient.available() && size < (int)sizeof(tbuf) - 1) {
      tbuf[size]=(char)mqttClient.read();
      size++;
    }
    while (mqttClient.available()) mqttClient.read();
    tbuf[size]=NULL;
    if (verbosity > 4) Serial.print(String(tbuf));
    if (verbosity > 4) Serial.println();
    //set number based on input
    f_bright = atof(tbuf); //atof avoids a heap-allocating String just to parse a float
    if (trace) {Serial.print("f_bright = "); Serial.println(f_bright,3);}
  }

} //onMqttMessage

//Non-blocking charge/discharge sweep animation state. Replaces the old delay()-based
//  version, which blocked loop() (and therefore mqttClient.poll()) for up to ~1.6s per
//  sweep -- during most of an active charge/discharge cycle, incoming MQTT messages just
//  sat unprocessed. Same visual result (same colors, same 20ms-per-LED pacing, same sweep
//  direction), but loop() now returns every pass instead of blocking inside it.
int anim_i = -1;          //current LED index mid-sweep; -1 = not running / needs (re)start
uint8_t anim_step = 0;    //0 = about to light anim_i, 1 = about to restore it and advance
int anim_dir_state = 0;   //currentState this sweep belongs to, so a charge<->discharge flip restarts cleanly
unsigned long anim_last_ms = 0;
const unsigned long ANIM_STEP_MS = 20; //matches the original delay(20) per phase

CRGB baseColorFor(int i) {
  return (i < lit_leds) ? CRGB::MidnightBlue : CRGB::DarkRed;
}

void loop() {
	// First slide the led in one direction
	if (f_SoC != last_SoC_drawn){
		lit_leds = (uint8_t) constrain(NUM_LEDS * f_SoC/100, 0, NUM_LEDS); //derives from NUM_LEDS (currently 40) instead of a hardcoded literal, so this stays correct if the strip length ever changes; constrain() is still defense in depth against f_SoC's clamp ever being bypassed
		for(int i = 0; i < NUM_LEDS; i++) {
			if (!(anim_step == 1 && i == anim_i)) leds[i] = baseColorFor(i); //leave a currently-highlighted LED alone; the animation restores it to the fresh base color on its next step
		}
		FastLED.show();
		last_SoC_drawn = f_SoC;
	}

	if (currentState == 1 || currentState == -1){
		if (anim_i == -1 || anim_dir_state != currentState){
			//(re)start the sweep from the correct end; restore any LED left mid-highlight by a direction flip
			if (anim_step == 1 && anim_i >= 0) { leds[anim_i] = baseColorFor(anim_i); FastLED.show(); }
			anim_i = (currentState == 1) ? 0 : NUM_LEDS - 1;
			anim_step = 0;
			anim_dir_state = currentState;
			anim_last_ms = millis();
		}
		if (millis() - anim_last_ms >= ANIM_STEP_MS){
			anim_last_ms = millis();
			if (anim_step == 0){
				//Yellow while charging, Amethyst while discharging
				leds[anim_i] = (currentState == 1) ? CRGB::Yellow : CRGB::Amethyst;
				FastLED.show();
				anim_step = 1;
			} else {
				leds[anim_i] = baseColorFor(anim_i);
				FastLED.show();
				anim_step = 0;
				anim_i += (currentState == 1) ? 1 : -1;
				if (anim_i >= NUM_LEDS) anim_i = 0;
				if (anim_i < 0) anim_i = NUM_LEDS - 1;
			}
		}
	} else if (anim_i != -1) {
		//went Idle mid-sweep: restore whatever LED was highlighted and stop animating
		if (anim_step == 1) { leds[anim_i] = baseColorFor(anim_i); FastLED.show(); }
		anim_i = -1;
	}

	mqttClient.poll();

	if (f_bright != last_bright){
		FastLED.setBrightness((uint8_t) f_bright*2.55);
		last_bright = f_bright;
	}

	delay(1); //small yield now that loop() is otherwise unthrottled
} //loop

