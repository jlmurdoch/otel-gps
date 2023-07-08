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
