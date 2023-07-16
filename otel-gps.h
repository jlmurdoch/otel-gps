// Wifi Headers
#if defined(ARDUINO_CHALLENGER_2040_WIFI_BLE_RP2040)
#include <WiFiEspAT.h>
#else
#include <WiFi.h>
#endif

/*
 * Nanopb protobuf implementation: https://jpa.kapsi.fi/nanopb/
 */
#include "pb.h"
#include "pb_encode.h"
#include "pb_common.h"
// Custom proto definitions
#include "metrics.pb.h"
#include "common.pb.h"
#include "resource.pb.h"

// Connection details and secrets
#include "creds.h"

// Make fixed
extern struct MetricMeta metricMeta[];

// GPS / NMEA
#define MAX_NMEA_MSG_BYTES 82
#define MAX_NMEA_BUFFER_BYTES 256
#define GPS_OUTPUT_FORMAT "$PMTK314,0,1,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0*28"

// FIFO
#define FIFO_SIZE 8 // Maximum for RP2040 (8 x 32bits)

// Dataset
#define METRIC_TYPES sizeof(metricMeta) / sizeof(MetricMeta)

// Accelerometer
#include <Wire.h>
#define MC3419_ADDR 0x4C
#define G_RANGE 2
