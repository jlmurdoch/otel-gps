/*
 * Set a WS2812B LED color by (mis)using the SPI
 */
#include <SPI.h>

#ifdef ARDUINO_ARCH_ESP32
SPIClass SPI1(FSPI);
#endif

static const int spiClock = 2400000;

uint32_t makeSPIColorBin(uint8_t inval) {
  // 0b100100100100100100100100 - 8 x 0b100 (zero)
  uint32_t outval = 9586980; 

  // Go through each bit in inval
  for (int x = 0; x < 8; x++) {
    // Quaternary operation - set to 0b110 if true
    int y = (inval & (0x1 << x));
    outval += y * y * y * 2;
  }

  // Should have a byte in quaternary form
  return outval;
}

void sendSPIColor(uint32_t data) {
  // Send 24 bits - one color, MSB->LSB
  SPI1.transfer((data >> 16) & 0xFF);
  SPI1.transfer((data >> 8) & 0xFF);
  SPI1.transfer(data & 0xFF);
}

void setRGBLED(uint8_t red, uint8_t green, uint8_t blue) {
  // Open the SPI and send 24 bits / 72 bits raw data
  SPI1.beginTransaction(SPISettings(spiClock, MSBFIRST, SPI_MODE0));
  sendSPIColor(makeSPIColorBin(red)); 
  sendSPIColor(makeSPIColorBin(green));
  sendSPIColor(makeSPIColorBin(blue));
  SPI1.endTransaction();
}

void setup() {
  // Set up the SPI
#if defined(ARDUINO_CHALLENGER_2040_WIFI_BLE_RP2040)
  SPI1.setTX(NEOPIXEL); //pin 11
  SPI1.begin();
#endif
#ifdef ARDUINO_ARCH_ESP32
  SPI1.begin(1, 42, 21, 41); // pin 21
#endif

  // Set up serial
  Serial.begin(115200);

  //pinMode(SPI_SS, OUTPUT);
}

void loop() {
  // RGB values
  setRGBLED(0x10, 0x00, 0x00); // Red
  delay(500);
  setRGBLED(0x00, 0x10, 0x00); // Green
  delay(500);
  setRGBLED(0x00, 0x00, 0x10); // Blue
  delay(500);
  setRGBLED(0x10, 0x00, 0x10); // Magenta
  delay(500);
  setRGBLED(0x10, 0x10, 0x00); // Yellow
  delay(500);
  setRGBLED(0x00, 0x10, 0x10); // Cyan
  delay(500);
  setRGBLED(0x10, 0x10, 0x10); // White
  delay(500);
}