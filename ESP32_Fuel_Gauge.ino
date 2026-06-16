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

unsigned long lastMillis = 0;

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

//set interval for sending messages (milliseconds)
const long interval = 5000;
unsigned long previousMillis = 0;

int count = 0;
float f_SoC = 0.0;
uint8_t blue =0;
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
  //TODO: because MQTT could send a huge string, this should be made to prevent overruns
  char tbuf[256]="";
  //char topic[256]="";
  int size=0;
	String topic = mqttClient.messageTopic();
  //strcpy(topic,mqttClient.messageTopic());
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
    while (mqttClient.available()) {
      tbuf[size]=(char)mqttClient.read();
      size++;    
    }
    tbuf[size]=NULL;
    if (verbosity > 4) Serial.print(String(tbuf));
    if (verbosity > 4) Serial.println();
    //set number based on input
    f_SoC = String(tbuf).toFloat();
    if (trace) {Serial.print("f_SoC = "); Serial.println(f_SoC,3);}
    //b_New_SoC = true;
  }
  if(topic.equals(subtopic2)){
    // use the Stream interface to print the contents
    while (mqttClient.available()) {
      tbuf[size]=(char)mqttClient.read();
      size++;    
    }
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
    while (mqttClient.available()) {
      tbuf[size]=(char)mqttClient.read();
      size++;    
    }
    tbuf[size]=NULL;
    if (verbosity > 4) Serial.print(String(tbuf));
    if (verbosity > 4) Serial.println();
    //set number based on input
    f_bright = String(tbuf).toFloat();
    if (trace) {Serial.print("f_bright = "); Serial.println(f_bright,3);}
    //b_New_SoC = true;
  }

} //onMqttMessage

//void fadeall() { for(int i = 0; i < NUM_LEDS; i++) { leds[i].nscale8(250); } }
unsigned long currentTime = millis();

void loop() {
	//uint8_t bar = random(100); 
	static uint8_t hue = 30;
	//Serial.print(bar);
	// First slide the led in one direction
	if (f_SoC != blue){
		blue = (uint8_t) 40 * f_SoC/100;
		for(int i = 0; i < NUM_LEDS; i++) {
			// Set the i'th led to red 
			leds[i] = CRGB::DarkRed;
		}
		//FastLED.show();//optional show? 
	
		for(int i = 0; i < blue; i++) {
			// Set the i'th led to red 
			leds[i] = CRGB::MidnightBlue;
		}
		FastLED.show();
		f_SoC = (float) blue;
	}
	if (currentState == 1){
		//discharging
		for(int j=0; j<1; j++){
			for(int i = 0; i < NUM_LEDS; i++) {
				// Set the i'th led to red 
				CRGB oldcolor = leds[i];
				//leds[i] = CHSV(hue++, 255, 255);
				leds[i] = CRGB::Yellow;
				FastLED.show();
				delay(20);
				leds[i]=oldcolor;
				FastLED.show();
				delay(20);
			}
			FastLED.show(); 
		}
	}

	if (currentState == -1){
		//charging
		for(int j=0; j<1; j++){
			for(int i = (NUM_LEDS-1); i >= 0; i--) {
				// Set the i'th led to red 
				CRGB oldcolor = leds[i];
				//leds[i] = CHSV(hue++, 255, 255);
				leds[i] = CRGB::Amethyst;
				FastLED.show();
				delay(20);
				leds[i]=oldcolor;
				FastLED.show();
				delay(20);
				
			}
			FastLED.show();
		}
	}

	unsigned long now = millis();
	currentTime = millis();
	//wait a half a second and poll while going
	while (now < (currentTime+500) ){
		mqttClient.poll();
		delay(10);
		now = millis();
	}
  if (f_bright != last_bright){
    FastLED.setBrightness((uint8_t) f_bright*2.55);
    last_bright = f_bright;
  }
	currentTime=millis();
	
} //loop

