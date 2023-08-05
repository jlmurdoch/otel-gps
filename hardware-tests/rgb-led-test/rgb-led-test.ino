/*
 * Set a WS2812B LED color by (mis)using the SPI
 */
#include <SPI.h>

// Safe Pico SPI settings, but use GPIO21 for the LED
#define SPI_SCK 1
#define SPI_MISO 42
#define SPI_MOSI 21 // swap out GPIO2 for GPIO21
#define SPI_SS 41

SPIClass *spi = NULL;
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

void sendSPIColor(SPIClass *spi, uint32_t data) {
  // Send 24 bits - one color, MSB->LSB
  spi->transfer((data >> 16) & 0xFF);
  spi->transfer((data >> 8) & 0xFF);
  spi->transfer(data & 0xFF);
}

void setRGBLED(uint8_t red, uint8_t green, uint8_t blue) {
  // Open the SPI and send 24 bits / 72 bits raw data
  spi->beginTransaction(SPISettings(spiClock, MSBFIRST, SPI_MODE0));
  sendSPIColor(spi, makeSPIColorBin(red)); 
  sendSPIColor(spi, makeSPIColorBin(green));
  sendSPIColor(spi, makeSPIColorBin(blue));
  spi->endTransaction();
}

void setup() {
  // Set up serial
  Serial.begin(115200);

  // Set up the SPI
  spi = new SPIClass(FSPI);
  spi->begin(SPI_SCK, SPI_MISO, SPI_MOSI, SPI_SS);
  pinMode(SPI_SS, OUTPUT);
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