# ESP32 Scream Audio Device

A wireless audio streaming solution for ESP32 microcontrollers that works with the Scream virtual audio driver protocol.

## Description

This project turns an ESP32 or ESP32-S3 microcontroller into a versatile audio streaming device. It can function as both:

- A **receiver** that plays audio streamed from Windows PCs running the Scream virtual audio driver
- A **sender** that captures audio from USB devices and transmits it to other Scream receivers

It uses WiFi to transmit high-quality, low-latency audio throughout your network, effectively creating wireless audio links between devices.

## Capabilities

### Receiver Mode
- Receives audio wirelessly from any Windows PC running the Scream audio driver
- Outputs to USB DAC (ESP32-S3) or SPDIF (ESP32/ESP32-S3)
- Supports 16-bit, 48kHz audio
- Dynamic buffer management for balancing latency and stability
- Volume control through web interface

### Sender Mode (ESP32-S3 only)
- Acts as a USB sound card and captures audio from hosts (computers, game consoles, phones)
- Transmits audio wirelessly to any Scream-compatible receiver
- Configurable destination IP and port
- Volume and mute controls via web interface

### System Features
- User-friendly web configuration interface
- Smart WiFi management with roaming support
- Power-saving sleep modes when inactive
- Automatic wake-on-audio functionality

### ScreamRouter Compatibility
This device is fully compatible with [ScreamRouter](https://github.com/netham45/screamrouter), an advanced audio routing and management system that supports both Scream and RTP audio sources. ScreamRouter enables:

- Complex whole-house audio setups
- Audio routing between multiple sources and receivers
- Group control of multiple audio endpoints
- Equalization and audio processing
- Time-shifting and audio buffering
- Web-based MP3 streaming for browser listening
- Home Assistant integration

## Installation

### Hardware Requirements

#### For Receiver Mode:
- ESP32-S3 with USB DAC *or* ESP32/ESP32-S3 with SPDIF DAC
- 5V power supply
- WiFi network

#### For Sender Mode:
- ESP32-S3 development board
- USB audio input device
- WiFi network

### Firmware Installation

**Option 1: Pre-built Firmware**
1. Download the firmware that matches your hardware:
   - `firmware-esp32s3-usb.bin` (ESP32-S3 with USB audio)
   - `firmware-esp32s3-spdif.bin` (ESP32-S3 with SPDIF)
   - `firmware-esp32-spdif.bin` (ESP32 with SPDIF)

2. Flash using esptool:
   ```
   esptool.py -p (PORT) write_flash 0x0 firmware-xxx.bin
   ```

**Option 2: Build from Source**
1. Install ESP-IDF (v5.4+)
2. Clone this repository
3. Copy the template sdkconfig to \sdkconfig:
   ```
   # For ESP32-S3 USB
   cp sdkconfig.esp32s3_usb sdkconfig
   
   # For ESP32-S3 SPDIF
   cp sdkconfig.esp32s3_spdif sdkconfig

   # For ESP32 SPDIF
   cp sdkconfig.esp32_spdif sdkconfig
   ```
4. Build, flash, and monitor:
   ```
   idf.py build
   idf.py -p (PORT) flash
   idf.py -p (PORT) monitor
   ```

## First-Time Setup

1. **Power on the device**
   - Connect power to your ESP32/ESP32-S3
   - For ESP32-S3 receiver, connect a USB DAC
   - For ESP32 receiver, connect a SPDIF DAC to the output pin
   - For ESP32-S3 sender, connect to a USB audio capable host

2. **Connect to initial WiFi access point**
   - The device creates a WiFi network named "ESP32-Scream"
   - Connect to this network with your phone/computer
   - A captive portal should open (or navigate to http://192.168.4.1/)

3. **Configure your WiFi**
   - Select your home network
   - Enter the password
   - Device will connect and restart

4. **Direct Connection Mode (No WiFi Network)**
   - If you want to use the receiver without connecting it to a WiFi network:
     - The receiver will remain in AP mode with IP address 192.168.4.1
     - Connect your sender device to the "ESP32-Scream" AP network
     - Configure the sender to use destination IP 192.168.4.1 and port 4010
     - Audio streaming will work directly between the devices

5. **Access the web interface**
   - Find the device's IP on your network
   - Or reconnect to "ESP32-Scream" 
   - Open the IP address in a browser
   - Configure options based on your use case

## Factory Reset
Hold GPIO pin 0 during the first 3 seconds of boot to reset all settings.
