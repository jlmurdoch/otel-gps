#include <Wire.h>

#define MC3419 1

#ifdef MC3419
//MC3419
#define DEVICE 0x4C
#define ACCEL_ADDR 0x0D
#else
// MPU9250
#define DEVICE 0x68
#define ACCEL_ADDR 0x3B
#endif 

#define GRANGE 2.0f

void printVals(void) {
  size_t size = 6;
  uint8_t values[size] = {0};
  readI2CConsecutiveBytes(DEVICE, ACCEL_ADDR, values, 6);

#ifdef MC3419
  float x = ((int16_t) ((uint16_t) values[1] << 8 | values[0])) * GRANGE / 32767.5f;
  float y = ((int16_t) ((uint16_t) values[3] << 8 | values[2])) * GRANGE / 32767.5f;
  float z = ((int16_t) ((uint16_t) values[5] << 8 | values[4])) * GRANGE / 32767.5f;
#else
  // Needs calibration offsets on the MPU9250
  float x = ((int16_t) ((uint16_t) values[0] << 8 | values[1])) * GRANGE / 32767.5f * 0.9807f;
  float y = ((int16_t) ((uint16_t) values[2] << 8 | values[3])) * GRANGE / 32767.5f * 0.9807f;
  float z = ((int16_t) ((uint16_t) values[4] << 8 | values[5])) * GRANGE / 32767.5f * 0.9807f;
#endif

  Serial.print("X: ");
  Serial.print(x);

  Serial.print(", Y: ");
  Serial.print(y);

  Serial.print(", Z: ");
  Serial.println(z);
}

uint8_t readI2CSingleByte(uint8_t device, uint8_t addr) {
  uint8_t value = 0;
  Wire.beginTransmission(device);
  Wire.write(addr);
  Wire.endTransmission(false);

  Wire.requestFrom(device, (uint8_t)1);
  if(Wire.available())
    value = Wire.read();
  Wire.endTransmission(device); 
  return value;  
}

void readI2CConsecutiveBytes(uint8_t device, uint8_t addr, uint8_t *values, int size) {
  Wire.beginTransmission(device);
  Wire.write(addr);
  Wire.endTransmission(false);

  Wire.requestFrom(device, size, true);
  for (int x = 0; x < size; x++) {
    values[x] = Wire.read();
  }
  Wire.endTransmission(device); 
}

void writeI2CSingleByte(uint8_t device, uint8_t addr, uint8_t value) {
  Wire.beginTransmission(device);
  Wire.write(addr);
  Wire.write(value);
  Wire.endTransmission(device);
}

void setup() {
  // put your setup code here, to run once:
  Serial.begin(115200);

#ifdef ARDUINO_ARCH_ESP32
  // ESP32-S3-Pico pinout
  Wire.begin(17, 18);
#else
  Wire.begin();
#endif

  uint8_t device = DEVICE;

#ifdef MC3419
  // Mode: Standby
  writeI2CSingleByte(device, 0x07, 0x00);
  delay(10);
  
  // Interrupt: Disable
  writeI2CSingleByte(device, 0x06, 0x00);
  delay(10);

  // Sample Rate (pg 45) - 25Hz sampling
  writeI2CSingleByte(device, 0x08, 0x10);
  delay(10);

  // Range & Scale Control (pg 53) - 2g - Bugatti Veyron = 1.55g / F1 turns = 6.5g
  writeI2CSingleByte(device, 0x20, 0x01);
  delay(10);

  // Mode: Wake
  writeI2CSingleByte(device, 0x07, 0x01);
  delay(10);
#else
  // MPU9250 Reset
  writeI2CSingleByte(device, 0x6B, 0x80);
  delay(10);

  // Bypass Enable (pg 29) 
  writeI2CSingleByte(device, 0x37, 0x02);
  delay(10);

  // 2g range
  writeI2CSingleByte(device, 0x1C, 0x00);
  delay(10);

  // Low-pass filter - #6
  writeI2CSingleByte(device, 0x1D, 0x0E);
  delay(10); 
#endif 
}

void loop() {
  // put your main code here, to run repeatedly:
  printVals();

  delay(5000);
}
