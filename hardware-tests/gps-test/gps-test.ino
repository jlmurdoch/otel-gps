/*
 * Test program for Serial GPS
 * Tested with:
 *   ESP32-S3 Dev Module (Waveshare ESP32-S3-Pico)
 *   Raspberry Pi Pico RP2040
 */

#ifdef ARDUINO_ARCH_ESP32
// ESP32
#include <HardwareSerial.h>
HardwareSerial GPSSerial(1);
#else
// RP2040
#define GPSSerial Serial1
#endif

#define ConsSerial Serial

void setup() {
  // make this baud rate fast enough to we aren't waiting on it
  ConsSerial.begin(115200);

  // wait for hardware serial to appear
  while (!ConsSerial) delay(10);
  
#ifdef ARDUINO_ARCH_ESP32
  // ESP32 needs GPIO specifics
  GPSSerial.begin(9600, SERIAL_8N1, 12, 11);
#else
  GPSSerial.begin(9600);
#endif
  
  ConsSerial.println("Serial Active");

  // Request only RMC and GGA messages
  GPSSerial.println("$PMTK314,0,1,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0*28");
  delay(10);
  // Request 10 sec updates
  GPSSerial.println("$PMTK220,10000*2F");
}


void loop() {
  if (GPSSerial.available()) {
    char nmeaMsg[83];
    size_t len = GPSSerial.readBytesUntil('\n', nmeaMsg, 83);
    nmeaMsg[len] = '\0';
    ConsSerial.println(nmeaMsg);

    // Debugging
    // ConsSerial.write(GPSSerial.read());
  }
}
