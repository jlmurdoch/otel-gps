# OpenTelemetry-compatible GPS Device

## Introduction

This is an attempt at an OpenTelemetry-compatible GPS device that collects positional / kinetic metrics and sends them to an OpenTelemetry Protocol endpoint, with the potential to add more metrics (altitude, acceleration, etc).

:information_source: From a developer-perspective, this is a hobby / experiment to learn more about OpenTelemetry and its inner-workings and is not representative of production implementation, official guidance, etc.

![Adafruit/iLabs on left, Waveshare on right](images/both_devices.jpg)
![Example route from Pontyprydd to Alresford](images/example_route.jpg)


## How it works

In basic technical terms, the device performs as follows:
 - On loop1():
   - Reads data from a GPS unit / other devices
   - Stores the data in core 1 memory
   - Loads the data onto a FIFO
   - Clears the data from memory when data is popped off the FIFO
 - On loop():
   - Sets up serial console, wireless
   - Pops the data from the FIFO
   - Stores the data in core 0 memory
   - Writes metadata to an OpenTelemetry metrics protobuf
   - Appends the collected data into an OpenTelemetry metrics protobuf
   - Attempts to connect to an OpenTelemetry HTTPS endpoint
   - Sends the protobuf using OTLP (Protobuf)
   - Clears all data from memory when sent

## Release Information
- 2023-07-08 - First Release
  - Just GPS positional data
- 2023-07-16 - Major Updates
  - Rework FIFO, metadata handling, raw data collection
  - Add accelerometer (MC3419) data collection and metrics
  - Clean up loop(), setup() procedures for both cores
  - Comments on areas scoped for reworking
  - Remove bugs seen when buffering / caching
- 2023-07-24 - Improvements & Alternative Hardware
  - Add support for ESP32 architecture
  - Support ESP32-S3 PSRAM for collection storage
  - Add non-ESP8266 WiFi support (ESP32, Infineon)
  - Add MPU9250 accelerometer support 
  - GPS & ESP8266 serial definitions for clarity
  - Remove warnings for static metadata structure
  - HTTP "200 OK" check to ensure data was delivered
- 2023-08-05 - Usability Enhancements
  - Remove simple LED support - impossible to convey state
  - Implement SPI-driven RGB LED support to indicate status levels
  - Added [ESP32 Bluetooth WiFi Provisioning](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/provisioning/wifi_provisioning.html) on startup to set SSID/passphrase
- 2023-09-17 - Optimisation & Clean-up
  - RGB LED support for both Waveshare (RGB) and iLabs (GRB) devices
  - Limit protobuf size to <8kB (40 secs of readings)
  - Avoid initialising protobuf preparatory data structure unless FIFO full
  - Memory usage reporting

## Known issues / Pending Improvements
- One contiguous source file - will broken up in the future
- MPU9250 accelerometer uncorrected
- Metric dimensions statically set to 3
- ESP32 GPS core/task memory untuned (65kB for now)
- GPS Serial testing / verification
- Protobuf payload verification
- ESP32-S3 Wifi optimisation (e.g. hidden AP) if possible
- ESP32-S3 Bluetooth setup of service.name

## Hardware 
The following hardware can be utilised, however through development and testing, an ESP32-S3 is recommended as it has superior caching potential for unstable connections, more storage for functionality and lots of wireless features.

### Raspberry Pi Pico form factor
 - *Recommended:* [Waveshare ESP32-S3 Pico](https://www.waveshare.com/wiki/ESP32-S3-Pico)
   - Memory: 512kB SRAM (188kB free) + 2MB PSRAM *~10 hours of caching*
   - Storage: 16MB
   - Feature: Bluetooth WiFi Provisioning (uses a lot SRAM)
 - *Alternative:* [Raspberry Pi Pico W](https://www.raspberrypi.com/documentation/microcontrollers/raspberry-pi-pico.html)
   - Memory: 264kB SRAM :warning: *40kB free = ~10 mins of caching*
   - Storage: 2MB
 - With a [Waveshare Pico GPS L76B](https://www.waveshare.com/wiki/Pico-GPS-L76B)
   - :information_source: Uses an external antenna
 - With a [Waveshare Pico 10DoF IMU](https://www.waveshare.com/wiki/Pico-10DOF-IMU)
   - Accelerometer, gyroscope, magnetometer, pressure and temperature sensors
![Waveshare stack with optional micro UPS](images/waveshare.jpg)


### Adafruit Feather form factor
 - *Extremely compact:* [iLabs Challenger RP2040 WiFi/BLE with 16-bit Accelerometer](https://ilabs.se/product/challenger-rp2040-wifi-ble-mkii-with-chip-antenna-and-16bit-accelerometer/)
   - Memory: 408kB SRAM :warning: *240kB free = ~60 mins of caching*
   - Storage: 4MB
   - :information_source: Using upgraded ESP32-C3 firmware (v2+) for SSL support on the ESP8266
 - With a [Adafruit Ultimate GPS Featherwing](https://learn.adafruit.com/adafruit-ultimate-gps-featherwing/overview)
   - Good for testing, as it has a surface-mounted ceramic antenna
   - :information_source: Mount **above** the iLabs Challenger using short headers for small form-factor and to use surface-mount receiver
![Waveshare stack with optional micro UPS](images/ilabs_and_adafruit_gps.jpg)

## Software
During execution, the following software libraries are possibly used:
 - [arduino-esp32](https://docs.espressif.com/projects/arduino-esp32/en/latest/)
   - For microcontrollers based on the EspressIf ESP32-S3 SoC
 - [arduino-pico](https://arduino-pico.readthedocs.io/en/latest/index.html)
   - For microcontrollers based on the Raspberry Pi RP2040
 - [Nanopb](https://jpa.kapsi.fi/nanopb/)
   - Applied to metrics, common and resources OpenTelemetry proto
   - Gives the ability to make small, compact, dynamic payloads
 - [WiFiEspAT](https://github.com/JAndrassy/WiFiEspAT)
   - For ESP8266 wireless
   - AT firmware version 2.4.0+ support (needed for SSL)
   - Needs to be patched to fast-scan when joining

## Challenger RP2040 ESP-C3 firmware updates
This is performed to obtain native SSL support with a Challenger RP2040 Wifi (so SSL doesn't need to be implemented within the code).

*CAUTION*: No responsibility or liability is assumed if these steps end up bricking your hardware:

```
Get the Challenger RP2040 USB updater for the ESP32:
$ git clone https://github.com/PontusO/RP2040USB2Serial

* Load up this in the Arduino IDE
* Change 'Tools' -> 'USB Stack' -> 'Adafruit TinyUSB'
* Verify and Upload to the device

Get esptool to take a backup of the current firmware:
$ git clone https://github.com/PontusO/esptool
$ cd esptool/

Check the device is connected and reachable:
$ ./esptool.py --port /dev/ttyACM0 --baud 115200 flash_id

Dump the contents of the flash as a backup and check it:
$ ./esptool.py --port /dev/ttyACM0 --baud 2000000 read_flash 0 ALL esp-c3-2.3.0-backup.bin
$ ./esptool.py --port /dev/ttyACM0 --baud 115200 verify_flash --diff yes 0 flashcontents.bin

Build new firmware using esp-at:
$ cd ..
$ git clone --recursive https://github.com/PontusO/esp-at
$ cd esp-at
$ ./build.py install
$ ./build.py build
$ cd ..

Get esptool and copy the firmware and attempt to flash:
$ cp esp-at/build/factory/filename.bin esptool/
$ cd esptool
$ ./esptool.py -p /dev/ttyACM0 -b 2000000 --before=default_reset --after=hard_reset write_flash --flash_mode dio --flash_freq 40m --flash_size 4MB 0 ../filename.bin
```

## OpenTelemetry proto preparation with Nanopb

This is how to translate a proto file into .c/.h files using Nanopb:
```
./generator/nanopb_generator.py -L quote file.proto
```

This is assuming all encoding is handled in the code. 

## Building

Providing the above is understood and executable by the user, the code can be built by:
- Cloning this repo
- Get the latest [OpenTelemetry Proto](https://github.com/open-telemetry/opentelemetry-proto) files
- Compile the supporting protobuf headers using Nanopb and OpenTelemetry proto files
  - Not supplied in repo as they should be built from latest / for your compiler
- Clone WiFiEspAt and modify accordingly:
  - [Enable v2 for SSL](https://github.com/JAndrassy/WiFiEspAT#getting-started)
  - (Optional) Patch it for fast-scanning [WiFiEspAT.patch](/WiFiEspAT.patch)

From there, the codebase can be compiled in a favorite Arduino compatible IDE or at the command-line.

## Verification

There are VERBOSE and DEBUG options that can be switched on:
- VERBOSE will write human-readable messages
- DEBUG will noisily print characters to represent data flow inside
- MEMORY will print memory usage at certain points

Otherwise an RGB LED works as follows (change the GPIO accordingly):
- Wireless:
  - White: Initialisation / WiFi Provision (hit "bootsel")
  - Cyan: WiFi waiting
  - Blue: Setup complete, WiFi connected
- Transmission:
  - Green: Message sent, good response
  - Yellow: Message sent, bad response
  - Magenta: Message sent, no response
  - Red: Nothing sent, connection failure 

## Further references
- [The C Programming Language](https://en.wikipedia.org/wiki/The_C_Programming_Language)
  - Linked lists and memory basics
- [NMEA-0183 Serial Communications](https://en.wikipedia.org/wiki/NMEA_0183)
  - Common GPS communication format
- [Adafruit GPS library](https://github.com/adafruit/Adafruit_GPS)
  - Not used in the project, but a good reference
- [Espressif WiFi AT Commands](https://docs.espressif.com/projects/esp-at/en/latest/esp32/AT_Command_Set/Wi-Fi_AT_Commands.html)
  - For testing wireless communications by hand
- [Worldsemi WS2812B RGB LED datasheet](https://cdn-shop.adafruit.com/datasheets/WS2812B.pdf)
- IMU Datasheets:
  - [TDK InvenSense MPU9250](https://invensense.tdk.com/wp-content/uploads/2015/02/PS-MPU-9250A-01-v1.1.pdf)
  - [Memsic MC3419](https://www.memsic.com/Public/Uploads/uploadfile/files/20220119/MC3419-PDatasheet%28APS-048-0073v1.3%29.pdf)
