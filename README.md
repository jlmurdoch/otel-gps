# OpenTelemetry-compatible GPS Device

## Introduction

This is an attempt at an OpenTelemetry-compatible GPS device that collects positional / kinetic metrics and sends them to an OpenTelemetry Protocol endpoint, with the potential to add more metrics (altitude, acceleration, etc).

From a developer-perspective, this has built from a hobby-perspective to learn more about OpenTelemetry and it's inner-workings in their spare time and is not representative of production implementation, official guidance, etc.

In basic technical terms, the device:
 - On core 1:
   - Reads data from a GPS unit / other devices
   - Stores the data in core 1 memory
   - Loads the data onto a RP2040 FIFO
   - Clears the data from memory when data is popped off the FIFO
 - On core 0:
   - Sets up serial console, wireless
   - Pops the data from the RP2040 FIFO
   - Stores the data in core 0 memory
   - Writes metadata to an OpenTelemetry metrics protobuf
   - Appends the collected data into an OpenTelemetry metrics protobuf
   - Attempts to connect to an OpenTelemetry HTTPS endpoint
   - Sends the protobuf using OTLP (Protobuf)
   - Clears all data from memory when sent

## Release Information
- 2023-07-08 - First Release
  - Just GPS positional data

## Hardware 
The following hardware is utilised:
 - [iLabs Challenger RP2040 WiFi/BLE with 16-bit Accelerometer](https://ilabs.se/product/challenger-rp2040-wifi-ble-mkii-with-chip-antenna-and-16bit-accelerometer/)
   - Using upgraded ESP32-C3 firmware (v2+) for SSL support
 - [Adafruit Ultimate GPS Featherwing](https://learn.adafruit.com/adafruit-ultimate-gps-featherwing/overview)

## Software

During execution, the following software libraries are used:
 - [arduino-pico](https://arduino-pico.readthedocs.io/en/latest/index.html)
   - Used for Pico SDK, RP2040 support, dual-cores, etc
 - [Nanopb](https://jpa.kapsi.fi/nanopb/)
   - Applied to metrics, common and resources OpenTelemetry proto
   - Gives the ability to make small, compact, dynamic payloads
 - [WiFiEspAT](https://github.com/JAndrassy/WiFiEspAT)
   - For ESP32/ESP8266 wireless
   - AT firmware version 2.4.0+ support (needed for SSL)
   - Modified by me to fast-scan when joining

## Challenger RP2040 ESP-C3 firmware updates

This is done to get native SSL support with a Challenger RP2040 Wifi (so SSL doesn't need to be implemented within the code).

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
  - Enable v2 (for SSL) [https://github.com/JAndrassy/WiFiEspAT#getting-started]
  - (Optional) Patch it for fast-scanning [WiFiEspAT.patch](/WiFiEspAT.patch)

From there, the codebase can be compiled in a favorite Arduino compatible IDE or at the command-line.

## Verification

There are VERBOSE and DEBUG options that can be switched on:
- VERBOSE will write human-readable messages
- DEBUG will noisily print characters to represent data flow inside

Otherwise the LEDs work as follows:
- Red GPS "Lock" LED
  - 1 second on/off blinking - attempting to attain lock
  - 1 second on, with longer off periods - lock obtained
- Green onboard LED
  - On at start until networking is operational
  - Blinks once to indicate a successful transfer
  - Blinks twice to indicate a failed transfer

## Further references
- [The C Programming Language](https://en.wikipedia.org/wiki/The_C_Programming_Language)
  - Linked lists and memory basics
- [NMEA-0183 Serial Communications](https://en.wikipedia.org/wiki/NMEA_0183)
  - For how some GPS units may communicate
- [Adafruit GPS library](https://github.com/adafruit/Adafruit_GPS)
  - Not used, but a good reference
- [Espressif WiFi AT Commands](https://docs.espressif.com/projects/esp-at/en/latest/esp32/AT_Command_Set/Wi-Fi_AT_Commands.html)
  - For testing wireless communications by hand
