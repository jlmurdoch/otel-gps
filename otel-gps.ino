/*
 * OTLP-enabled GPS Unit
 * Author: jmurdoch
 *
 * Hardware:
 *  - iLabs Challenger RP2040 WiFi/BLE
 *    - ESP32-C3 Firmware upgraded beyond v2.3.0 for SSL (https://github.com/PontusO/esp-at)
 *  - Adafruit Ultimate GPS Feather
 *    - PA1616D - 99 channel GPS [MTK3333]
 *
 * Additional libraries used:
 *  - WiFiEspAt - https://github.com/JAndrassy/WiFiEspAT
 *  - Nanopb - https://jpa.kapsi.fi/nanopb/
 */
#include "otel-gps.h"

// Collection
#define MAX_NMEA_MSG 82
#define MAX_PROTOBUF 65534
#define MAX_NMEA_BUF 256
#define FIFO_PAYLOAD_SIZE 16  // Bytes = 4 x 32: 64 bit timestamp, 32bit lat, 32-bit long - maximum 8

// Protobuf Prep
#define METRIC_DIMS sizeof(metricMeta) / sizeof(MetricMeta) // How many metric-time-series do we have?
#define METRIC_TYPES 1  // How many unique metric types? (degrees, g's, etc) AKA countMetaTypes()

// Transmission
#define DELAY_WAIT 100 // Generic Delay
#define SSL 1

// Serial console output
// #define DEBUG 1
// #define VERBOSE 1

// Get all the credentials out of creds.h
const char *ssid = WIFI_SSID;
const char *pass = WIFI_PASS;
const char *host = HOST;
const uint16_t port = PORT;
const char *uri = URI ;
const char *apikey = APIKEY;

// Otel Protobuf Payload
uint8_t pbufPayload[MAX_PROTOBUF];
size_t pbufLength = 0;
bool pbufCached = 0;

/*
 * Linked-list implementation is used extensively.
 *
 * The C Programming Language (2nd Edition, 1988) 
 * - Section 6.5 Self-referential Structures
 * - Section 6.7 Typedef
 */

/*
 * Core 1: Raw NMEA data holding area
 */
typedef struct nmeanode *Nmeaptr;

typedef struct nmeanode {
  double epoch;  // Because we can do 10Hz / 0.1 sec
  float lat;
  float lon;
  Nmeaptr next;
} Nmeanode;

// Where we pop() data from
Nmeaptr nmeaHead = NULL;
// Where we push() data to
Nmeaptr nmeaTail = NULL;

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
typedef struct dnode (*Dataarray)[METRIC_TYPES];
typedef struct dnode {
  char *name;
  char *desc;
  char *unit;
  Attrptr attr;
  Metricptr metricHead;  // For pop() in Protobuf generation
  Metricptr metricTail;  // For push() for NMEA collection
} Datanode;

/*
 * Core 0: Interim Metric Metadata Store
 */
// Detail for each possible metric-time-series
struct MetricMeta {
  int pos;
  char *name;
  char *desc;
  char *unit;
  char *attrname;
  char *attrval;
  char *resname;
  char *resval;
};

// Fixed list of expected metric-time-series
struct MetricMeta metricMeta[] = {
  // First number indicates the array to put the data - e.g Acceleration all can go into the same store
  // pos, shortname   description     unit   attributes
  { 0, "position", "GPS Position", "degrees", "dim", "lat", "service.name", "jm-moto" },
  { 0, "position", "GPS Position", "degrees", "dim", "lon", "service.name", "jm-moto" }
  /*,
  { 1, "alt",       "GPS Altitude", "metres",  NULL, NULL },
  { 2, "accel",     "Acceleration", "g",       "dim", "x" },
  { 2, "accel",     "Acceleration", "g",       "dim", "y" }, 
  { 2, "accel",     "Acceleration", "g",       "dim", "z" }
  */
};

// Unused (for now), but will be useful when there's more metric types
int countMetaTypes(void) {
  int x, y, count;
  int size = sizeof(metricMeta) / sizeof(MetricMeta);

  for (x = 0; x < size; x++) {
    for (y = x + 1; y < size; y++) {
      if (strcmp(metricMeta[x].name, metricMeta[y].name) == 0)
        break;
    }
    if (y == size)
      count++;
  }

  return count;
}

/*
 * CORE 1 - NMEA Processing and FIFO hand-off
 */
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

float parseNmeaCoord(float dms, char dir) {
  /*
  Degree + Minutes: DDMM.MMMM 
   via (DD + 0.MMMMMM) * -1
  Decimal Minutes: DD.mmmmmm
  */
  if (dir > 80)
    return ((int)dms / 100 + (float)((int)(dms * 10000) % 1000000) / 600000) * -1;
  else
    return ((int)dms / 100 + (float)((int)(dms * 10000) % 1000000) / 600000);
}

Nmeaptr parseMsgRmc(char *line) {
  char *p = line;
  float ftemp;

  Nmeaptr temp = (Nmeaptr)malloc(sizeof(Nmeanode));
  if (temp == NULL) { return NULL; }

  // Time (HH:MM:SS.ss)
  p = strchr(p, ',') + 1;
  temp->epoch = parseNmeaTime(atof(p));

  // Validity - not an 'A', abort
  p = strchr(p, ',') + 1;
  if (p[0] > 65) {
    free(temp);
#ifdef DEBUG
    Serial.println(F("X"));
#endif
    return NULL;
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

  return temp;
}

Nmeaptr parseMsgGga(char *line) {
  char *p = line;
  float ftemp;
  int itemp;

  Nmeaptr temp = (Nmeaptr)malloc(sizeof(Nmeanode));
  if (temp == NULL) { return NULL; }

  // Time (HH:MM:SS.ss)
  p = strchr(p, ',') + 1;
  temp->epoch = parseNmeaTime(atof(p));

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
    free(temp);
    return NULL;
  }

  // Satellites
  p = strchr(p, ',') + 1;

  temp->next = NULL;

  return temp;
}

Nmeaptr nmeaToMetrics(char *nmeaMsg) {
  Nmeaptr temp = NULL;
  char header[7] = { 0 };
  strncpy(header, nmeaMsg, 6);
  // Use first 6 chars to decide what to do
  // RMC: time, lat, lon, knots, course
  // GGA: time, lat, lon, alt
  if (!strcmp(header, "$GNRMC")) {
    temp = parseMsgRmc(nmeaMsg);
  } else if (!strcmp(header, "$GNGGA")) {
    temp = parseMsgGga(nmeaMsg);
  }
  return temp;
}

bool nmeaPushTail(Nmeaptr temp) {
  if (nmeaTail == NULL) {
    nmeaHead = nmeaTail = temp;
  } else {
    nmeaTail->next = temp;
    nmeaTail = temp;
  }

  return true;
}

bool nmeaPopHead(void) {
  // Kept in just incase the functions need to be cleaner
  if (nmeaHead != NULL) {
    Nmeaptr temp = nmeaHead;
    nmeaHead = nmeaHead->next;
    if (nmeaHead == NULL) {
      nmeaTail = NULL;
    }
    free(temp);
  }
  return true;
}

/*
 * CORE 0 - FIFO collection, Data Sorting, Protobuf build, Wireless delivery
 */
bool initDataset(Dataarray array) {
  // Build out the list of metrics
  for (int d = 0; d < METRIC_DIMS; d++) {
    if ((*array)[metricMeta[d].pos].name == NULL) {
      // Set up the metric metadata
      (*array)[metricMeta[d].pos].name = metricMeta[d].name;
      (*array)[metricMeta[d].pos].desc = metricMeta[d].desc;
      (*array)[metricMeta[d].pos].unit = metricMeta[d].unit;

      // Resource attributes go here - memory leak of 16 bytes
      if (metricMeta[d].resname != NULL) {
        Attrptr tempattr = (Attrptr) malloc(sizeof(Attrnode));
        tempattr->key = metricMeta[d].resname;
        tempattr->value = metricMeta[d].resval;
        tempattr->next = NULL;
        (*array)[metricMeta[d].pos].attr = tempattr;
      }
    }
  }
  return true;
}

bool datapointPushTail(Dataarray array, int d, double epoch, double value) {
  Metricptr temp = (Metricptr)malloc(sizeof(Metricnode));

  // Put all the data into a temp node
  temp->time = epoch;
  temp->type = AS_DOUBLE;
  temp->value.as_double = value;
  temp->attr = NULL;
  temp->next = NULL;

  // Add metric attributes
  if (metricMeta[d].attrname != NULL) {
    Attrptr tempattr = (Attrptr)malloc(sizeof(Attrnode));
    tempattr->key = metricMeta[d].attrname;
    tempattr->value = metricMeta[d].attrval;
    tempattr->next = NULL;
    temp->attr = tempattr;
  }

  // Add data to appropriate array (metricMeta[idx].pos)
  if ((*array)[metricMeta[d].pos].metricTail == NULL) {
    // If empty both HEAD and TAIL are the same node
    (*array)[metricMeta[d].pos].metricHead = (*array)[metricMeta[d].pos].metricTail = temp;
  } else {
    // Keep HEAD pointing to that first node
    // Add node to TAIL->next, then move TAIL reference to it
    (*array)[metricMeta[d].pos].metricTail->next = temp;
    (*array)[metricMeta[d].pos].metricTail = temp;
  }

  return true;
}

bool datasetPush(Dataarray ptr, uint32_t buf[4]) {
  double epoch;
  float value;

  memcpy(&epoch, buf, 8);  // Copy epoch

  // For each 32-bit metric, add to the appropriate array
  for (int a = 0; a < METRIC_DIMS; a++) {
    // Copy 32bit value, skipping epoch (2 x 32bit)
    memcpy(&value, buf + 2 + a, 4);
    datapointPushTail(ptr, a, epoch, value);
  }

  return true;
}


bool freeDataset(Dataarray array) {
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

int fifoCollect(Dataarray array) {
  int datapoints = 0;
  uint32_t *fifoout = (uint32_t *)malloc(sizeof(uint32_t));
  // TODO: Change all cases of 4 to FIFO_PAYLOAD_SIZE/4?
  // TODO: Do we make this blocking to ensure a full packet is taken?
  uint32_t fifobuf[4];
  while (rp2040.fifo.available() >= 4) {
    for (int i = 0; i < 4; i++) {
      if (rp2040.fifo.pop_nb(fifoout)) {
        fifobuf[i] = *fifoout;
      } else {
#ifdef DEBUG
        Serial.print(F("F")); 
#endif
      }
    }

    // Add bytes to overall Core 0 dataset
    datasetPush(array, fifobuf);
    datapoints++;
#ifdef DEBUG
    Serial.print(F("m")); 
#endif
  }
  free(fifoout);

  return datapoints;
}

bool encode_string(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
  const char *str = (const char *)(*arg);

  if (!pb_encode_tag_for_field(stream, field))
    return false;

  return pb_encode_string(stream, (const uint8_t *)str, strlen(str));
}

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

bool Gauge_encode_data_points(pb_ostream_t *ostream, const pb_field_iter_t *field, void *const *arg) {
  Datanode *myMetric = (Datanode *)(*arg);

  Metricptr temp = myMetric->metricHead;

  while (temp != NULL) {
    NumberDataPoint data_point = {};

    data_point.time_unix_nano = temp->time * 1000000000;
    
    if (temp->type == AS_INT) {
      data_point.which_value = NumberDataPoint_as_int_tag;
      data_point.value.as_int = temp->value.as_int;
    } else if (temp->type == AS_DOUBLE) {
      data_point.which_value = NumberDataPoint_as_double_tag;
      data_point.value.as_double = temp->value.as_double;
    }

    if (temp->attr != NULL) {
      data_point.attributes.arg = temp->attr;
      data_point.attributes.funcs.encode = KeyValue_encode_attributes;
    }

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
#ifdef SSL
  if (!client.connectSSL(host, port)) {
#else
  if (!client.connect(host, port)) {
#endif
#ifdef VERBOSE
    Serial.print(F("[VERBOSE] Delivery: Connection failed to "));
    Serial.println(String(host));
#endif
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
  } else {

#ifdef VERBOSE
    Serial.println(F("[VERBOSE]: Delivery: POST Failed"));
#endif
    return 3;
  }

#ifdef VERBOSE
  while (client.available()) {
    char ch = static_cast<char>(client.read());
    Serial.print(ch);
  }
  Serial.println();
#endif

  // Flush and clear the connection
  client.flush();
  client.stop();

  return 0;
}

void joinWireless() {
#ifdef VERBOSE
  Serial.println(F("[VERBOSE] Hardware: ESP8266 Joining AP"));
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
  Serial.println(F("[VERBOSE] Hardware: ESP8266 Connected to AP"));
#endif
}

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
  
  Serial2.begin(115200); // Optimal, but can go higher
  while(!Serial2.find("ready")) { delay(10); }

  // Sanity Check - See if terminal is responding
  Serial2.println("AT");
  while(!Serial2.find("OK\r\n")) { delay(10); }

  /*
  What an actual, manual start looks like:
    AT+CWSTOPSMART        // Get this out of memory
    AT+CWAUTOCONN=0       // Try to connect when powered on
    AT+CWMODE=1           // Station Mode
    AT+CWRECONNCFG=1,1000 // Reconnect at every second for 1000 times
    AT+CWJAP="ssid","psk",,,,,0 // Find the AP as fast as possible

  Last part is hacked into WiFiEspAT.
  */

  // WiFiEspAT Begin
  WiFi.init(Serial2);

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
  // GPS - this is only specific to RP2040
  Serial1.setFIFOSize(MAX_NMEA_BUF);
  Serial1.begin(9600);

  // Wait for availability
  while (!Serial1) delay(100);

  // RMC - Recommended Minimum (Time, Lat, Lon, Speed, Course, Date)
  Serial1.println("$PMTK314,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0*29");

  // GGA - GPS Fix (Time, Lat, Lon, Altitude) <-- NO DATE, ONLY TIME
  // Serial1.println("$PMTK314,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0*29");

  // Once every 10 secs, Default: 1 per sec
  // delay(100);
  // Serial1.println("$PMTK220,10000*2F");

  // Clear the GPS serial of any initialising output
  while (Serial1.available()) { Serial1.read(); }
}

void loop1() {
  // Collect GPS data and/or push data to FIFO - one per cycle

  /*
   * Phase 1: Pickup from GPS
   */
  if (Serial1.available()) {
    // TODO: Edge-case - garbage handling?
    // NMEA 0183 message is no more than 82 bytes, ending with LF ('\n')
    char nmeaMsg[MAX_NMEA_MSG];
    size_t len = Serial1.readBytesUntil('\n', nmeaMsg, MAX_NMEA_MSG);

    // LF ('\n') is stripped from end, so replace with \0 to terminate string
    nmeaMsg[len] = '\0';

    // Parse the data and append to the list
    Nmeaptr temp = nmeaToMetrics(nmeaMsg);
    
    // Push the data if it's returned
    if (temp != NULL) {
      nmeaPushTail(temp);
#ifdef DEBUG
      Serial.print(F("g")); 
#endif
    } 
  }

  /*
   * Phase 2: Delivery to core0
   */ 
  // Did we get data from the GPS?
  if (nmeaHead != NULL) {
    uint32_t fifobuf[FIFO_PAYLOAD_SIZE / 4];
    memcpy(fifobuf, nmeaHead, sizeof(fifobuf));
    if (rp2040.fifo.push_nb(fifobuf[0])) {
      // Room in FIFO
      for (int i = 1; i < FIFO_PAYLOAD_SIZE / 4; i++)
        rp2040.fifo.push(fifobuf[i]);
      // Once in FIFO, clear FIFO
      nmeaPopHead();
#ifdef DEBUG
      Serial.print(F("f")); // DEBUG
#endif
    }
  }
}

void setup() {
  // Put on the LED until we are in the main loop
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);

  // Switch the Serial console on
  Serial.begin(115200);

  // Wireless start
  initWireless();
  joinWireless();
}

void loop() {
  // Wifi management & Protobuf conversion

  // If we don't already have data to send, go and collect
  if (pbufLength == 0) {
    int pbufStatus;

    // Array which will house the pre-protobuf data, initialised
    Datanode myData[METRIC_TYPES] = {{ 0 }};
    // Pointer to the array
    Dataarray myDataPtr = &myData;

    // Initialise the dataset - using metric metadata
    initDataset(myDataPtr);

    // Pick up data from Core 1
    if (fifoCollect(myDataPtr)) {
      // Build out the protobuf into pbufPayload
      MetricsData metricsdata = {};
      metricsdata.resource_metrics.arg = myDataPtr;
      metricsdata.resource_metrics.funcs.encode = MetricsData_encode_resource_metrics;
      pb_ostream_t output = pb_ostream_from_buffer(pbufPayload, sizeof(pbufPayload));
      pbufStatus = pb_encode(&output, MetricsData_fields, &metricsdata);
      pbufLength = output.bytes_written;

#ifdef VERBOSE
      Serial.print(F("[VERBOSE] Protobuf: Size = "));
      Serial.println(pbufLength);
#endif

      // if there's a pbuf error, clear the buffer
      if (!pbufStatus) {
#ifdef VERBOSE
        Serial.print(F("[VERBOSE] Protobuf: Main - "));
        Serial.println(PB_GET_ERROR(&output));
#endif
        pbufLength = 0;
      }
    } 
    // Clear the dataset - either it converted or it didn't
    freeDataset(myDataPtr);
  }
   
  // If we have data, send it
  if (pbufLength > 100) {
    int reasonCode = sendOTLP(pbufPayload, pbufLength);
    if (!reasonCode) {
      pbufLength = 0;
      digitalWrite(LED_BUILTIN, HIGH);
#ifdef VERBOSE
      Serial.println(F("[VERBOSE] Delivery: Success"));
#endif

    } else {
      // Failed to send - blink twice
      digitalWrite(LED_BUILTIN, HIGH);
      delay(100);
      digitalWrite(LED_BUILTIN, LOW);
      delay(100);
      digitalWrite(LED_BUILTIN, HIGH);
#ifdef VERBOSE
      Serial.print(F("[VERBOSE] Delivery: Failed to send: "));
      Serial.println(reasonCode);
#endif
    }
  } else {
    // Throw away the data if it is too small
    pbufLength = 0;
  }
  // Sleep then switch off the LED
  delay(DELAY_WAIT);
  digitalWrite(LED_BUILTIN, LOW);
}