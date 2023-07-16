/*
 * OpenTelemetry-compatible GPS
 * Author: jmurdoch
 */
#include "otel-gps.h"

/*
 * Tweakable definitions
 */
// Largest Protobuf size
#define MAX_PROTOBUF_BYTES 65534

// Transmission Options
#define DELAY_WAIT 1 // Generic Delay - unused

// Serial console output
// #define DEBUG 1 // This is noisy, as it will throw characters to show memory, protobuf values, etc
#define VERBOSE 1 // Less noisy, but not much introspective capability compared to DEBUG

// Get all the credentials out of creds.h
const char *ssid = WIFI_SSID;
const char *pass = WIFI_PASS;
const char *host = HOST;
const uint16_t port = PORT;
const char *uri = URI ;
const char *apikey = APIKEY;

// Otel Protobuf Payload
uint8_t pbufPayload[MAX_PROTOBUF_BYTES];
size_t pbufLength = 0;

/*
 * Core 0: Interim Metric Metadata Store
 */

// Dimensions of each metric
struct DimensionMeta {
  char *attrname;
  char *attrval;
};

#define METRIC_MAX_DIMS 3
// Each metric
struct MetricMeta {
  char *name;
  char *desc;
  char *unit;
  char *resname;
  char *resval;
  DimensionMeta dimMeta[METRIC_MAX_DIMS];
};

// Refactored to make counting easier
struct MetricMeta metricMeta[] = {
  // First number indicates the array to put the data - e.g Acceleration all can go into the same store
  // pos, shortname   description     unit   attributes
  { "position", "GPS Position", "degrees", "service.name", "jm-moto", { {"dim", "lat"}, {"dim", "lon"}, { 0, 0 } } },
  { "accel", "Acceleration", "g", "service.name", "jm-moto", { {"dim", "x"}, {"dim", "y"}, {"dim", "z"} } }
};

/*
 * Linked-list implementation is used extensively.
 *
 * The C Programming Language (2nd Edition, 1988) 
 * - Section 6.5 Self-referential Structures
 * - Section 6.7 Typedef
 */

/*
 * Core 1: Raw data holding area
 */
typedef struct Rawnode *Rawptr;

typedef struct Rawnode {
  double epoch;  // Double because we can do < 1 sec
  float lat;
  float lon;
  float x;
  float y;
  float z;
  Rawptr next;
} Rawnode;

// Where we pop() data from
Rawptr rawHead = NULL;
// Where we push() data to
Rawptr rawTail = NULL;

/*
 * Core 0: Metric Store for Protobuf conversion
 */
// Otel KeyValue Attributes (datapoint and resource)
typedef struct anode *Attrptr;
typedef struct anode {
  char *key;
  char *value;
  Attrptr next;
} Attrnode;

// Otel Datapoints
typedef struct mnode *Metricptr;
typedef struct mnode {
  uint64_t time;
  int type;
  union {
    int64_t as_int;
    double as_double;
  } value;
  Attrptr attr;
  Metricptr next;
} Metricnode;
// To enumerate the metric value types
enum { AS_INT, AS_DOUBLE };

// Otel Metrics Dataset (top-level metadata + datapoint list)
// FIXME - change back from 2
typedef struct dnode (*Dataarray)[METRIC_TYPES];
typedef struct dnode {
  char *name;
  char *desc;
  char *unit;
  Attrptr attr;
  Metricptr metricHead;  // For pop() in Protobuf generation
  Metricptr metricTail;  // For push() for raw collection
} Datanode;


// Helper function to count metric dimensions
// TODO: Write to a dynamic, global array in setup()? e.g. [2, 3]
int countMetricDims(int x) {
  int y = 0;

  // Count the populated dimensions
  for (y = 0; y< METRIC_MAX_DIMS; y++) {
    if(metricMeta[x].dimMeta[y].attrval == NULL)
      break;
  }

  return y;
}

/*
 * CORE 1 - Accelerometer, NMEA Processing and FIFO hand-off
 */

// This can be used to collect multiple registers without delay
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

// Used for setup
void writeI2CSingleByte(uint8_t device, uint8_t addr, uint8_t value) {
  Wire.beginTransmission(device);
  Wire.write(addr);
  Wire.write(value);
  Wire.endTransmission(device);

  return;
}

void initAccelerometer(void) {
  /*
   * I2C Setup for Accelerometer
   */
  Wire.begin();

  // Mode: Standby
  writeI2CSingleByte(MC3419_ADDR, 0x07, 0x00);
  delay(10);

  // Disable Interrupts
  writeI2CSingleByte(MC3419_ADDR, 0x06, 0x00);
  delay(10);

  // Sample Rate (pg 45) - 25Hz sampling
  writeI2CSingleByte(MC3419_ADDR, 0x08, 0x10);
  delay(10);

  // Range & Scale Control (pg 53) - 2g - Bugatti Veyron = 1.55g / F1 turns = 6.5g
  writeI2CSingleByte(MC3419_ADDR, 0x20, 0x01);
  delay(10);

  // Mode: Wake
  writeI2CSingleByte(MC3419_ADDR, 0x07, 0x01);
  delay(10);
}

// Collect data from all 6 registers (3 x 16-bits)
void collectAccelerometer(Rawptr temp) {
  // Read x/y/z @ 16-bits each (6 * 8 bits)
  uint8_t values[6] = {0};
  readI2CConsecutiveBytes(MC3419_ADDR, 0x0D, values, 6);

  temp->x = (((int16_t) ((uint16_t) values[1] << 8 | values[0])) * (G_RANGE / 32767.5f));
  temp->y = (((int16_t) ((uint16_t) values[3] << 8 | values[2])) * (G_RANGE / 32767.5f));
  temp->z = (((int16_t) ((uint16_t) values[5] << 8 | values[4])) * (G_RANGE / 32767.5f));
}

// Convert HHMMSS.mmm to epoch secs
long parseNmeaTime(float time) {
  long epoch;

  // Hours
  epoch = (long)(time / 10000) * 3600;
  // Mins
  epoch += (long)(time / 100) % 100 * 60;
  // Secs
  epoch += (long)time % 100;
  // M secs
  // epoch += (uint16_t)(time * 1000) % 1000;
  return epoch;
}

// Convert DDMMYY to epoch secs
long parseNmeaDate(int date) {
  int mon = date / 100 % 100;
  int year = date % 100;

  int months[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
  long secs = 946684800;  // 2000-01-01 00:00:00
  // Add the days for each year before
  secs += year * 31536000;
  // Add missing leap year days (excluding this year)
  secs += (((year - 1) / 4) + 1) * 86400;
  // If right now is a leap year, add a day if after Feb
  if (!(year % 4) && (mon > 2)) {
    secs += 86400;
  }
  // Add previous month days
  for (int x = 0; x < (mon - 1); x++) {
    secs += months[x] * 86400;
  }
  // Add this months days
  secs += (date / 10000 - 1) * 86400;
  return secs;
}

// Convert DDDMM.MMMM,X (Degree + Minutes) to float (Decimal Minutes)
float parseNmeaCoord(float dms, char dir) {
  // Positive: E=0x45, N=0x4E
  // Negative: S=0x53, W=0x57
  if (dir > 0x50)
    return ((int)dms / 100 + (float)((int)(dms * 10000) % 1000000) / 600000) * -1;
  else
    return ((int)dms / 100 + (float)((int)(dms * 10000) % 1000000) / 600000);
}

// Parse GxRMC messages (Recommended minimum)
int parseMsgRmc(Rawptr temp, char *line) {
  // Incomplete: $GNRMC,220107.094,V,,,,,0.62,145.91,150723,,,N*56
  // Complete:   $GNRMC,220536.000,A,5100.2345,N,00100.2345,W,0.17,146.38,150723,,,A*63
  char *p = line;
  float ftemp;

  // Time (HH:MM:SS.ss)
  p = strchr(p, ',') + 1;
  temp->epoch = parseNmeaTime(atof(p));

  // Validity - not an 'A', abort
  p = strchr(p, ',') + 1;
  if (p[0] > 65) {
#ifdef DEBUG
    Serial.println(F("X"));
#endif
    return 1;
  }

  // Latitude
  p = strchr(p, ',') + 1;
  ftemp = atof(p);
  p = strchr(p, ',') + 1;
  temp->lat = parseNmeaCoord(ftemp, p[0]);

  // Longitude
  p = strchr(p, ',') + 1;
  ftemp = atof(p);
  p = strchr(p, ',') + 1;
  temp->lon = parseNmeaCoord(ftemp, p[0]);

  // Speed
  p = strchr(p, ',') + 1;

  // Course
  p = strchr(p, ',') + 1;

  // Date (YYMMDD)
  p = strchr(p, ',') + 1;
  temp->epoch += parseNmeaDate(atoi(p));

  temp->next = NULL;

  return 0;
}

// Parse GxGGA messages (Fixed data)
int parseMsgGga(Rawptr temp, char *line) {
  // Incomplete: $GNGGA,220307.092,,,,,0,0,,,M,,M,,*59
  // Complete:   $GNGGA,220423.000,5100.2345,N,00100.2345,W,1,04,3.34,100.0,M,47.4,M,,*61
  char *p = line;
  float ftemp;
  int itemp;

  // Time (HH:MM:SS.ss)
  p = strchr(p, ',') + 1;
  // Should be set by GxRMC
  // temp->epoch = parseNmeaTime(atof(p));

  // Latitude
  p = strchr(p, ',') + 1;
  ftemp = atof(p);
  p = strchr(p, ',') + 1;
  temp->lat = parseNmeaCoord(ftemp, p[0]);

  // Longitude
  p = strchr(p, ',') + 1;
  ftemp = atof(p);  
  p = strchr(p, ',') + 1;
  temp->lon = parseNmeaCoord(ftemp, p[0]);

  // Fix Indicator - more than zero is good
  p = strchr(p, ',') + 1;
  itemp = atoi(p);
  if (itemp == 0) {
#ifdef DEBUG
    Serial.println(F("X"));
#endif
    return 1;
  }

  // Satellites
  p = strchr(p, ',') + 1;

  // Horizontal Precision
  p = strchr(p, ',') + 1;

  // Altitude
  p = strchr(p, ',') + 1;
  // Not implemented... yet
  // temp->alt = atof(p);

  temp->next = NULL;

  return 0;
}

// Refactored
bool rawdataCollect(Rawptr temp) {
  char nmeaMsg[MAX_NMEA_MSG_BYTES];
  char header[7] = { 0 };
  size_t len = 0;
  int errors = 0;
  temp->epoch = 0;

  // Phase 1: Get at least one valid, usable GPS message
  while(Serial1.available()) {
    // Get a full line, replacing the stripped-out \n with \0
    len = Serial1.readBytesUntil('\n', nmeaMsg, MAX_NMEA_MSG_BYTES);
    nmeaMsg[len] = '\0';

    // Get the NMEA Sentence header
    strncpy(header, nmeaMsg, 6);
#ifdef VERBOSE
    Serial.print(F("[VERBOSE] GPS-NMEA: "));
    Serial.println(nmeaMsg);
#endif
      
    // Default Order: GNGGA,GPGSA,GLGSA,GNRMC,GNVTG
    if (!strcmp(header, "$GNGGA")) {
      errors += parseMsgGga(temp, nmeaMsg);
    } else if (!strcmp(header, "$GNRMC")) {
      errors += parseMsgRmc(temp, nmeaMsg);
    }
  }

  // Phase 2: If we get something usable, append other data
  if (errors || temp->epoch == 0) {
    return 0;
  } else {
    // Add accelerometer data
    collectAccelerometer(temp);
    return 1;
  }
}

// Pushes collected raw data onto the tail of the list
bool rawPushTail(Rawptr temp) {
  if (rawTail == NULL) {
    rawHead = rawTail = temp;
  } else {
    rawTail->next = temp;
    rawTail = temp;
  }

  return true;
}

// Takes collected raw data off the head of the list
bool rawPopHead(void) {
  // Check, just to avoid crashes
  if (rawHead != NULL) {
    Rawptr temp = rawHead;
    rawHead = rawHead->next;
    // Is it the last? If so, reset the tail pointer too
    if (rawHead == NULL) {
      rawTail = NULL;
    }
    free(temp);
  }
  return true;
}

/*
 * CORE 0 - FIFO collection, Data Sorting, Protobuf build, Wireless delivery
 */

// Initialise the data set with metadata alone
bool datasetInit(Dataarray array) {
  // Build out the list of metric types

  for (int x = 0; x < METRIC_TYPES; x++) {
    if ((*array)[x].name == NULL) {
      // Set up the metric metadata
      (*array)[x].name = metricMeta[x].name;
      (*array)[x].desc = metricMeta[x].desc;
      (*array)[x].unit = metricMeta[x].unit;

      // Resource attributes go here - if they exist
      if (metricMeta[x].resname != NULL) {
        Attrptr tempattr = (Attrptr) malloc(sizeof(Attrnode));
        tempattr->key = metricMeta[x].resname;
        tempattr->value = metricMeta[x].resval;
        tempattr->next = NULL;
        (*array)[x].attr = tempattr;
      }
    }
  }
  return true;
}

// Push a datapoint onto the end of the appropriate list
bool datapointPushTail(Dataarray array, int x, int y, double epoch, double value) {
  Metricptr temp = (Metricptr)malloc(sizeof(Metricnode));

  // Put all the data into a temp node
  temp->time = epoch;
  temp->type = AS_DOUBLE;
  temp->value.as_double = value;
  temp->attr = NULL;
  temp->next = NULL;

  // Add metric attributes
  if (metricMeta[x].dimMeta[y].attrname != NULL) {
    Attrptr tempattr = (Attrptr)malloc(sizeof(Attrnode));
    tempattr->key = metricMeta[x].dimMeta[y].attrname;
    tempattr->value = metricMeta[x].dimMeta[y].attrval;
    tempattr->next = NULL;
    temp->attr = tempattr;
  }

  // Add data to appropriate array 
  if ((*array)[x].metricTail == NULL) {
    // If empty both HEAD and TAIL are the same node
    (*array)[x].metricHead = (*array)[x].metricTail = temp;
  } else {
    // Keep HEAD pointing to that first node
    // Add node to TAIL->next, then move TAIL reference to it
    (*array)[x].metricTail->next = temp;
    (*array)[x].metricTail = temp;
  }

  return true;
}

// Push multiple datapoints into the dataset
bool datasetPush(Dataarray ptr, uint32_t buf[FIFO_SIZE]) {
  double epoch;
  float value;
  int z = 2; // start 2 x 32 bits in because of epoch data

  memcpy(&epoch, buf, 8);  // Copy epoch from first two 32bits

  // For each 32-bit metric, add to the appropriate array
  for (int x = 0; x < METRIC_TYPES; x++) {
    for (int y = 0; y < countMetricDims(x); y++) {
      // IDEA: It's possible to do bit-packing here to improve memory
      // Copy 32bit value
      memcpy(&value, buf + z, 4);
      datapointPushTail(ptr, x, y, epoch, value);
      z++; // keeps track of 32-bit position
    }
  }

  return true;
}

// Mass clean-up of the dataset for this cycle
bool datasetFree(Dataarray array) {
  // For each metric type...
  for (int a = 0; a < METRIC_TYPES; a++) {
    // If there's metrics, clear it out
    while ((*array)[a].metricHead != NULL) {
      // Take the first metric, save it
      Metricptr temp = (*array)[a].metricHead;
      // Next becomes first
      (*array)[a].metricHead = (*array)[a].metricHead->next;

      // Nothing in the list
      if ((*array)[a].metricHead == NULL) {
        // Just to be completely clean
        (*array)[a].metricTail = NULL;
      }

      // Free Datapoint Attribute(s)
      while (temp->attr != NULL) {
        Attrptr tempattr = temp->attr;
        temp->attr = temp->attr->next;
        free(tempattr);
      }
      // Free Datapoint
      free(temp);
    }

    // Free Resource Attribute(s)
    while ((*array)[a].attr != NULL) {
      Attrptr temp = (*array)[a].attr;
      (*array)[a].attr = (*array)[a].attr->next;
      free(temp);
    }
  }

  return true;
}

// Pushes data onto the FIFO from core1
bool fifoDispatch(Rawptr ptr) {
  uint32_t fifobuf[FIFO_SIZE];

  // IDEA: It's possible to do bit-packing here to improve memory
  memcpy(fifobuf, ptr, sizeof(fifobuf));
  
  // This HAS to be non-blocking to begin with, otherwise the GPS buffer will overflow and data corruption will occur
  if (rp2040.fifo.push_nb(fifobuf[0])) {
    // If the above succeeds, the FIFO is empty or in the process of being emptied by fifoCollect()
    for (int i = 1; i < FIFO_SIZE; i++)  
      rp2040.fifo.push(fifobuf[i]);
    
    // Remove this element if pushed
    rawPopHead();
  }
  
  return 0;
}

// Pick up data off the FIFO from core0
int fifoCollect(Dataarray array) {
  int datapoints = 0;
  uint32_t fifobuf[FIFO_SIZE] = { 0 };

  // Put in a limit here to keep the packet size small and fragment up larger backlogs? MAX_METRICS?
  while (rp2040.fifo.available() >= FIFO_SIZE) {
    // Collect an entire payload off the FIFO
    for (int i = 0; i < FIFO_SIZE; i++)
      fifobuf[i] = rp2040.fifo.pop();

    // Add bytes to overall Core 0 dataset
    datasetPush(array, fifobuf);
    datapoints++;
#ifdef DEBUG
    Serial.print(F("m")); 
#endif
  }

  return datapoints;
}

// Protobuf encoding of a string
bool encode_string(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
  const char *str = (const char *)(*arg);

  if (!pb_encode_tag_for_field(stream, field))
    return false;

  return pb_encode_string(stream, (const uint8_t *)str, strlen(str));
}

// Protobuf encoding of Key-Value pairs (Attributes in OpenTelemetry)
bool KeyValue_encode_attributes(pb_ostream_t *ostream, const pb_field_iter_t *field, void *const *arg) {
  Attrptr myAttr = (Attrnode *)(*arg);

  while (myAttr != NULL) {
    KeyValue keyvalue = {};

    keyvalue.key.arg = myAttr->key;
    keyvalue.key.funcs.encode = encode_string;

    keyvalue.has_value = true;
    keyvalue.value.which_value = AnyValue_string_value_tag;
    keyvalue.value.value.string_value.arg = myAttr->value;
    keyvalue.value.value.string_value.funcs.encode = encode_string;

    // Build the submessage tag
    if (!pb_encode_tag_for_field(ostream, field)) {
#ifdef VERBOSE
      const char *error = PB_GET_ERROR(ostream);
      Serial.print(F("[VERBOSE] Protobuf: KeyValue Tag -  "));
      Serial.println(error);
#endif
      return false;
    }

    // Build the submessage payload
    if (!pb_encode_submessage(ostream, KeyValue_fields, &keyvalue)) {
#ifdef VERBOSE
      const char *error = PB_GET_ERROR(ostream);
      Serial.print(F("[VERBOSE] Protobuf: KeyValue Msg - "));
      Serial.println(error);
#endif
      return false;
    }

    myAttr = myAttr->next;
  }

  return true;
}

// Protobuf encoding of a Gauge datapoint
bool Gauge_encode_data_points(pb_ostream_t *ostream, const pb_field_iter_t *field, void *const *arg) {
  Datanode *myMetric = (Datanode *)(*arg);

  Metricptr temp = myMetric->metricHead;

  while (temp != NULL) {
    NumberDataPoint data_point = {};

    // make the time nano-second granularity
    data_point.time_unix_nano = temp->time * 1000000000;
   
    // Two 64-bit number types in OpenTelemetry: Integers or Doubles
    if (temp->type == AS_INT) {
      data_point.which_value = NumberDataPoint_as_int_tag;
      data_point.value.as_int = temp->value.as_int;
    } else if (temp->type == AS_DOUBLE) {
      data_point.which_value = NumberDataPoint_as_double_tag;
      data_point.value.as_double = temp->value.as_double;
    }

    // Do we have attributes to assign to this datapoint?
    if (temp->attr != NULL) {
      data_point.attributes.arg = temp->attr;
      data_point.attributes.funcs.encode = KeyValue_encode_attributes;
    }

    // Any flags for this?
    data_point.flags = 0;

    // Build the submessage tag
    if (!pb_encode_tag_for_field(ostream, field)) {
#ifdef VERBOSE
      const char *error = PB_GET_ERROR(ostream);
      Serial.print(F("[VERBOSE] Protobuf: Gauge Tag -  "));
      Serial.println(error);
#endif
      return false;
    }

    // Build the submessage payload
    if (!pb_encode_submessage(ostream, NumberDataPoint_fields, &data_point)) {
#ifdef VERBOSE
      const char *error = PB_GET_ERROR(ostream);
      Serial.print(F("[VERBOSE] Protobuf: Gauge Msg -  "));
      Serial.println(error);
#endif
      return false;
    }
    temp = temp->next;
  }

  return true;
}

// Protobuf encoding of a Metric definition
bool ScopeMetrics_encode_metric(pb_ostream_t *ostream, const pb_field_iter_t *field, void *const *arg) {
  Datanode *myMetric = (Datanode *)(*arg);

  if (myMetric->metricHead != NULL) {
    Metric metric = {};

    metric.name.arg = myMetric->name;
    metric.name.funcs.encode = encode_string;
    metric.description.arg = myMetric->desc;
    metric.description.funcs.encode = encode_string;
    metric.unit.arg = myMetric->unit;
    metric.unit.funcs.encode = encode_string;

    metric.which_data = Metric_gauge_tag;
    metric.data.gauge.data_points.arg = myMetric;  
    metric.data.gauge.data_points.funcs.encode = Gauge_encode_data_points;

    // Build the submessage tag
    if (!pb_encode_tag_for_field(ostream, field)) {
#ifdef VERBOSE
      const char *error = PB_GET_ERROR(ostream);
      Serial.print(F("[VERBOSE] Protobuf: ScopeMetrics Tag -  "));
      Serial.println(error);
#endif
      return false;
    }

    // Build the submessage payload
    if (!pb_encode_submessage(ostream, Metric_fields, &metric)) {
#ifdef VERBOSE
      const char *error = PB_GET_ERROR(ostream);
      Serial.print(F("[VERBOSE] Protobuf: ScopeMetrics Msg - "));
      Serial.println(error);
#endif
      return false;
    }
  }

  return true;
}

// Protobuf encoding of a scope (passthrough - nothing much done here)
bool ResourceMetrics_encode_scope_metrics(pb_ostream_t *ostream, const pb_field_iter_t *field, void *const *arg) {
  Datanode *myMetric = (Datanode *)(*arg);
  if (myMetric != NULL) {
    ScopeMetrics scope_metrics = {};

    scope_metrics.metrics.arg = myMetric;
    scope_metrics.metrics.funcs.encode = ScopeMetrics_encode_metric;

    // Build the submessage tag
    if (!pb_encode_tag_for_field(ostream, field)) {
#ifdef VERBOSE
      const char *error = PB_GET_ERROR(ostream);
      Serial.print(F("[VERBOSE] Protobuf: ResourceMetrics Tag - "));
      Serial.println(error);
#endif
      return false;
    }

    // Build the submessage payload
    if (!pb_encode_submessage(ostream, ScopeMetrics_fields, &scope_metrics)) {
#ifdef VERBOSE
      const char *error = PB_GET_ERROR(ostream);
      Serial.print(F("[VERBOSE] Protobuf: ResourceMetrics Msg - "));
      Serial.println(error);
#endif
      return false;
    }
  }

  return true;
}

// Protobuf encoding of entire payload
bool MetricsData_encode_resource_metrics(pb_ostream_t *ostream, const pb_field_iter_t *field, void *const *arg) {
  /*
   * MetricsData ()
   *   +--ResourceMetrics (resource_metrics)
   *        +--Resource (resource)
   *             +--KeyValue (attributes)
   *        +--ScopeMetrics (scope_metrics)
   */
  Dataarray myDataPtr = (Dataarray)(*arg);

  if (myDataPtr != NULL) {
    ResourceMetrics resource_metrics = {};

    for (int a = 0; a < METRIC_TYPES; a++) {
      if ((*myDataPtr)[a].attr != NULL) {
        resource_metrics.has_resource = true;
        // Passing a pointer to the attribute linked-list
        resource_metrics.resource.attributes.arg = (*myDataPtr)[a].attr;
        resource_metrics.resource.attributes.funcs.encode = KeyValue_encode_attributes;
      }

      if ((*myDataPtr)[a].metricHead != NULL) {
        // Passing an array element, so need to send a reference
        resource_metrics.scope_metrics.arg = &(*myDataPtr)[a];
        resource_metrics.scope_metrics.funcs.encode = ResourceMetrics_encode_scope_metrics;
      }

      // Build the submessage tag
      if (!pb_encode_tag_for_field(ostream, field)) {
#ifdef VERBOSE
        const char *error = PB_GET_ERROR(ostream);
        Serial.print("[VERBOSE] Protobuf: MetricsData Tag - ");
        Serial.println(error);
#endif
        return false;
      }

      // Build the submessage payload
      if (!pb_encode_submessage(ostream, ResourceMetrics_fields, &resource_metrics)) {
#ifdef VERBOSE
        const char *error = PB_GET_ERROR(ostream);
        Serial.print("[VERBOSE] Protobuf: MetricsData Msg - ");
        Serial.println(error);
#endif
        return false;
      }
    }
  }
  return true;
}

bool buildProtobuf (Dataarray args) {
  MetricsData metricsdata = {};
  metricsdata.resource_metrics.arg = args;
  metricsdata.resource_metrics.funcs.encode = MetricsData_encode_resource_metrics;

  pb_ostream_t output = pb_ostream_from_buffer(pbufPayload, sizeof(pbufPayload));
  int pbufStatus = pb_encode(&output, MetricsData_fields, &metricsdata);
  pbufLength = output.bytes_written;

  // if there's a pbuf error, clear the buffer
  if (!pbufStatus) {
    pbufLength = 0;
#ifdef VERBOSE
    Serial.print(F("[VERBOSE] Protobuf: Main - "));
    Serial.println(PB_GET_ERROR(&output));
#endif
    return 0;
  }
  return 1;
}

// Function to send data to a HTTP(S)-based endpoint
int sendOTLP(uint8_t *buf, size_t bufsize) {
  // We can't use HTTPClient as it might clash with other WiFi libraries
  WiFiClient client;
  
  // Check to see if the WiFi is still alive
  if (WiFi.status() != WL_CONNECTED) {
#ifdef VERBOSE
    Serial.print(F("[VERBOSE] Hardware: ESP8266 AP Disconnected"));
    Serial.println(WiFi.status());
#endif
    joinWireless();
    return 1;
  }

  // Connect and handle failure
  digitalWrite(LED_BUILTIN, HIGH);
#ifdef SSL
  if (!client.connectSSL(host, port)) {
#else
  if (!client.connect(host, port)) {
#endif
#ifdef VERBOSE
    Serial.print(F("[VERBOSE] Delivery: Connection failed to "));
    Serial.println(String(host));
#endif

    digitalWrite(LED_BUILTIN, LOW);
    delay(200);
    digitalWrite(LED_BUILTIN, HIGH);
    delay(200);
    digitalWrite(LED_BUILTIN, LOW);
    return 2;
  }

  // Transmit the readings
  if (client.connected()) {
    client.print(F("POST "));
    client.print(uri);
    client.print(F(" HTTP/1.1\r\nHost: "));
    client.print(host);
    // client.print(F(":"));
    // client.print(String(port));
    client.print(F("\r\nContent-Type: application/x-protobuf\r\nX-SF-Token: "));
    client.print(apikey);
    client.print(F("\r\nContent-Length: "));
    client.print(String(bufsize));
    client.print(F("\r\n\r\n"));
    client.write(buf, bufsize);
    delay(100);
  } else {

#ifdef VERBOSE
    Serial.println(F("[VERBOSE]: Delivery: POST Failed"));
#endif
    digitalWrite(LED_BUILTIN, LOW);
    delay(200);
    digitalWrite(LED_BUILTIN, HIGH);
    delay(200);
    digitalWrite(LED_BUILTIN, LOW);
    return 3;
  }

// Works, but sometimes emits too much or nothing at all
#ifdef VERBOSE
  Serial.print(F("[VERBOSE] Result: "));
  while (client.available()) {
    char ch = static_cast<char>(client.read());
    Serial.print(ch);
  }
  Serial.println();
#endif

  // Flush and clear the connection
  client.flush();
  client.stop();
  digitalWrite(LED_BUILTIN, LOW);

  return 0;
}

// Used to connect to an access point
void joinWireless() {
  digitalWrite(LED_BUILTIN, HIGH);
#ifdef VERBOSE
  Serial.println(F("[VERBOSE] Hardware: ESP8266 Awaiting AP"));
#endif
  // If it's not connected we receive, keep trying
  while (WiFi.begin(ssid, pass) != WL_CONNECTED) {
    // Cleanly disconnect
    WiFi.disconnect();  
#ifdef VERBOSE
    Serial.println(F("[VERBOSE] Hardware: ESP8266 Awaiting AP"));
#endif
  }
#ifdef VERBOSE
  Serial.println(F("[VERBOSE] Hardware: ESP8266 Joined AP"));
#endif

  digitalWrite(LED_BUILTIN, LOW);
}

// Used to (re)initialise the WiFi hardware
void initWireless() {
#ifdef VERBOSE
  Serial.println(F("[VERBOSE] Hardware: ESP8266 Initialise"));
#endif

#if defined(ARDUINO_CHALLENGER_2040_WIFI_BLE_RP2040)
  // Special Startup for the ESP8266
  pinMode(PIN_ESP_RST, OUTPUT);
  digitalWrite(PIN_ESP_RST, HIGH);
  pinMode(PIN_ESP_MODE, OUTPUT);
  digitalWrite(PIN_ESP_MODE, HIGH);

  // Reset
  digitalWrite(PIN_ESP_RST, LOW); 
  delay(1);
  digitalWrite(PIN_ESP_RST, HIGH); // End Reset
  
  // Default speed - 15kb/sec across wifi
  Serial2.begin(115200);
  while(!Serial2.find("ready")) { delay(10); }

  Serial2.println(F("AT"));
  while(!Serial2.find("OK\r\n")) { delay(10); }

  /*
  // 8x Faster speed - 155kb/sec across wifi
  Serial2.println(F("AT+UART_CUR=115200,8,1,0,0"));
  delay(100);

  Serial2.begin(115200);
  while(!Serial2.find("ready")) { delay(10); }

  Serial2.println(F("AT"));
  while(!Serial2.find("OK\r\n")) { delay(10); }
  */

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
#ifdef VERBOSE
      Serial.println(F("[VERBOSE] Hardware: No hardware found!"));
#endif
    }
  }
  // From WiFiEspAT's SetupPersistentConnection.ino
  // Quit an AP, wipe DNS and enable DHCP
  WiFi.endAP(); 
  WiFi.disconnect();
#endif

#ifdef VERBOSE
  Serial.println(F("[VERBOSE] Hardware: ESP8266 Ready"));
#endif
}

void setup1() {
  /*
   * GPS - this is only specific to RP2040
   */
  delay(2000);
  Serial1.setFIFOSize(MAX_NMEA_BUFFER_BYTES);
  Serial1.begin(9600);
  delay(10);

  /*
    Cold Reset:
    $PMTK103*30<CR><LF>

    len = Serial1.readBytesUntil('\n', nmeaMsg, MAX_NMEA_MSG);
    
    Reset output:
    $PMTK011,MTKGPS*08<CR><LF>
    $PMTK010,002*2D<CR<LF>
  */

  // Both RMC + GGA
  Serial1.println(F(GPS_OUTPUT_FORMAT));
  delay(10);
  // Check for ACK : $PMTK001,314,3*36


  /*
   * Other Data Sources in Core 1
   */ 
  initAccelerometer();
}

void loop1() {
  /*
   * Core 1: Collect data, parse it, push onto FIFO for Core 0 to pick up
   */

  /*
   * Phase 1: Pickup from GPS
   * - GPS data determines if we do anything, hence wait for Serial1
   * - Create a raw data point in memory
   * - Collect GPS and other data points
   */
  if (Serial1.available()) {
    Rawptr temp = (Rawptr) malloc(sizeof(Rawnode));

    if (rawdataCollect(temp)) {
      rawPushTail(temp);
#ifdef DEBUG
      Serial.print(F("g")); 
#endif 
    } else {
      free(temp);
    }
  } 

  /*
   * Phase 2: Delivery to core0
   * - Process data held
   * - Avoid while() so GPS can be monitored 
   */
  if (rawHead != NULL) {
    fifoDispatch(rawHead);
  }
}

void setup() {
  // Put on the LED until we are in the main loop
  pinMode(LED_BUILTIN, OUTPUT);

  // Switch the Serial console on
  Serial.begin(115200);

  // Wireless start
  initWireless();
  joinWireless();
}

void loop() {
  /*
   * Core 0: Metric prep, collection of data from core 1, convert to protobuf, send over HTTP(S)
   */

  // Phase 1: If we don't already have data to send, go and collect
  if (pbufLength == 0) {
    // House pre-protobuf metric type, initialised
    Datanode myData[METRIC_TYPES] = {{ 0 }};
    // Pointer to the array
    Dataarray myDataPtr = &myData;

    // Initialise the dataset - using metric metadata
    datasetInit(myDataPtr);

    // Pick up data from Core 1
    if (fifoCollect(myDataPtr)) 
      buildProtobuf(myDataPtr);

    // Clear the dataset
    datasetFree(myDataPtr);
  }
   
  // Phase 2: If we have data now, send it
  if (pbufLength > 0) {
    if(sendOTLP(pbufPayload, pbufLength) == 0)
      pbufLength = 0;
  }
}
