/*
 * Wireless
 */ 
// Generic WiFi
#include "creds.h"

#ifdef ARDUINO_CHALLENGER_2040_WIFI_BLE_RP2040
#include <WiFiEspAT.h>
#else
#include <WiFi.h>
#include <WiFiClientSecure.h>
#endif

// WiFi Provisioning
#ifdef ARDUINO_ARCH_ESP32
#include <esp_wifi.h>
#include <wifi_provisioning/scheme_ble.h>
#endif

/*
 * Data internals / FIFO
 */
#define FIFO_SIZE 8 // Maximum for RP2040 (8 x 32bits)

#ifdef ARDUINO_ARCH_ESP32
#include "queue.h"
#endif

// Dataset
#define METRIC_TYPES sizeof(metricMeta) / sizeof(MetricMeta)

// Make fixed
extern struct MetricMeta metricMeta[];

/*
 * Nanopb protobuf
 */
// Largest Protobuf size
#define MAX_PROTOBUF_BYTES 65534

#include "pb.h"
#include "pb_encode.h"
#include "pb_common.h"

/* 
 * Compiled OpenTelemetry Proto headers
 */
#include "metrics.pb.h"
#include "common.pb.h"
#include "resource.pb.h"

// Connection details and secrets
#include "creds.h"

/*
 * IMU
 */
#include <Wire.h>
#define G_RANGE 2

#if defined(ARDUINO_CHALLENGER_2040_WIFI_BLE_RP2040)
// Accelerometer built into Challenger 2040
#define MC3419 1
#endif

#ifdef MC3419
//MC3419
#define ACCEL_ADDR 0x4C
#define ACCEL_REG 0x0D
#else
// MPU9250
#define ACCEL_ADDR 0x68
#define ACCEL_REG 0x3B
#endif

/* 
 * Serial / UART
 */
#ifdef ARDUINO_ARCH_ESP32
// ESP32
#include <HardwareSerial.h>
#else
// RP2040
#define GPSSerial Serial1
#endif

#ifdef ARDUINO_CHALLENGER_2040_WIFI_BLE_RP2040
#define ESP8266Serial Serial2
#endif

// GPS / NMEA Parsing
#define MAX_NMEA_MSG_BYTES 82
#define MAX_NMEA_BUFFER_BYTES 256
#define GPS_OUTPUT_FORMAT "$PMTK314,0,1,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0*28"

/*
 * SPI / RGB LED
 */
#ifdef RGB_LED
// SPI can be (mis)used drive an RGB LED
#include <SPI.h>

// Safe Pico SPI settings, swap out GPIO2 for the RGB LED (GPIO21)
#define SPI_SCK 1
#define SPI_MOSI 21
#define SPI_SS 41
#define SPI_MISO 42

// Colors - not too intense
#define RGB_WHITE   0x10, 0x10, 0x10
#define RGB_RED     0x10, 0x00, 0x00
#define RGB_GREEN   0x00, 0x10, 0x00
#define RGB_BLUE    0x00, 0x00, 0x10
#define RGB_CYAN    0x00, 0x10, 0x10
#define RGB_YELLOW  0x10, 0x10, 0x00
#define RGB_MAGENTA 0x10, 0x00, 0x10
#endif
