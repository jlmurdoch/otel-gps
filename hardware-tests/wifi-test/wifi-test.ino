/*
 * Code to test various WiFi combinations with SSL:
 * - Raspberry Pi Pico W (RP2040 - Infineon CYW43439)
 * - iLabs Challenger 2040 WiFi (RP2040 - ESP8266)
 * - Waveshare Pico ESP32-S3
 * - Adafruit QT Py ESP32-S2 
 */

#if defined(ARDUINO_CHALLENGER_2040_WIFI_BLE_RP2040)
#include <WiFiEspAT.h>
#else
#include <WiFi.h>
#include <WiFiClientSecure.h>
#endif

static const char *ssid = "myaccesspoint";
static const char *pass = "mysecurecreds"; 

#if defined(ARDUINO_CHALLENGER_2040_WIFI_BLE_RP2040)
void esp8266wifi() {
  pinMode(PIN_ESP_RST, OUTPUT);
  digitalWrite(PIN_ESP_RST, HIGH);
  pinMode(PIN_ESP_MODE, OUTPUT);
  digitalWrite(PIN_ESP_MODE, HIGH);

  // Reset
  digitalWrite(PIN_ESP_RST, LOW); 
  delay(1);
  digitalWrite(PIN_ESP_RST, HIGH); // End Reset
  
  // 115200 is the default speed
  Serial2.begin(115200);
  while(!Serial2.find("ready")) { delay(10); }

  Serial2.println(F("AT"));
  while(!Serial2.find("OK\r\n")) { delay(10); }
  
  // WiFiEspAT Begin
  WiFi.init(Serial2);

  /*
  If we didn't use WiFiEspAt, here what a manual start could look like:
    AT+CWSTOPSMART        // Get this out of memory
    AT+CWAUTOCONN=0       // Try to connect when powered on
    AT+CWMODE=1           // Station Mode
    AT+CWRECONNCFG=1,1000 // Reconnect at every second for 1000 times
    AT+CWJAP="ssid","psk",,,,,0 // Find the AP as fast as possible

  Last part is patched into WiFiEspAT.
  */

  if (WiFi.status() == WL_NO_MODULE) {
    while (true) {
      delay(1000);
      Serial.println(F("[VERBOSE] Hardware: No hardware found!"));
    }
  }
  // From WiFiEspAT's SetupPersistentConnection.ino
  // Quit an AP, wipe DNS and enable DHCP
  WiFi.endAP(); 
  WiFi.disconnect();
}
#endif

void setup() {
  Serial.begin(115200);

#if defined(ARDUINO_CHALLENGER_2040_WIFI_BLE_RP2040)
  esp8266wifi();
#endif

  Serial.println("Connecting to WiFi...");
  WiFi.begin(ssid, pass);
  while (WiFi.status() != WL_CONNECTED) {
    Serial.println("Connecting to WiFi...");
    delay(500);
  }
  Serial.println("Connected to WiFi");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.begin(ssid, pass);
    while (WiFi.status() != WL_CONNECTED) {
      Serial.println("Connecting to WiFi...");
      delay(500);
    }
  }

  Serial.println("Connecting to www.google.com over HTTPS...");
#if defined(ARDUINO_CHALLENGER_2040_WIFI_BLE_RP2040)
  WiFiClient client;
  if (!client.connectSSL("www.google.com", 443)) {
#else
  WiFiClientSecure client;
  client.setInsecure();
  if (!client.connect("www.google.com", 443)) {
#endif
    Serial.println(F("Connection failed to www.google.com"));
    delay(5000);
    return;
  }

  // Try to send the data
  if (!client.connected()) {
    Serial.println(F("POST Failed"));
  } else {
    client.println(F("GET / HTTP/1.1"));
    client.println(F("Host: www.google.com"));
    client.println();
    
    while (client.connected()) {
      if (client.available()) {
        char c = client.read();
        Serial.print(c);
      }
    }
    Serial.println();
  } 

  // Clean up and close out the connection
  client.flush();
  client.stop();

  delay(20000);
}
