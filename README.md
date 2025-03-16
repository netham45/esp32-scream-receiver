# ESP32 Scream Receiver

A WiFi-based audio streaming receiver for the Scream protocol, implemented on ESP32/ESP32-S3 microcontrollers.

## Table of Contents
- [Introduction](#introduction)
- [Hardware Requirements](#hardware-requirements)
- [Build and Flash Instructions](#build-and-flash-instructions)
- [Architecture Overview](#architecture-overview)
- [Core Components - Audio Subsystem](#core-components---audio-subsystem)
- [Core Components - Network Stack](#core-components---network-stack)
- [Core Components - WiFi Management](#core-components---wifi-management)
- [Core Components - Power Management](#core-components---power-management)
- [Program Flow](#program-flow)
- [Configuration Options](#configuration-options)
- [Technical Details](#technical-details)
- [Troubleshooting and FAQs](#troubleshooting-and-faqs)

## Introduction

The ESP32 Scream Receiver is an implementation of a network audio receiver for the [Scream](https://github.com/duncanthrax/scream) virtual audio driver. It allows you to wirelessly stream audio from a Windows PC running the Scream audio driver to an ESP32 or ESP32-S3 connected to a USB DAC or SPDIF device.

### What is Scream?

Scream is an open-source virtual audio driver for Windows that captures system audio and sends it over the network. It acts as a virtual sound card that transmits audio over UDP or TCP to receivers on the local network. This project implements a receiver compatible with the Scream protocol on the ESP32 platform.

### Key Features

- **Wireless Audio Streaming**: Receive audio from any Windows PC running Scream over WiFi
- **Hardware Compatibility**:
  - ESP32-S3: Support for USB Audio Class 1.0 devices (DACs)
  - ESP32: Support for SPDIF digital audio output
- **Advanced WiFi Management**:
  - Automatic connection to strongest known network
  - WiFi roaming with 802.11k/v/r support
  - Captive portal for configuration
- **Power Efficiency**:
  - Light sleep mode during audio silence
  - Deep sleep mode when no DAC is connected
  - Automatic wake on network activity
- **Web Configuration Interface**: Easy setup via browser
- **Dynamic Buffer Management**: Balance between latency and playback stability

## Hardware Requirements

### Compatible Hardware

#### ESP32-S3 (For USB Audio Output)
- ESP32-S3 development board (ESP32-S3-DevKitC recommended)
- USB OTG support for connecting to USB DACs
- Minimum 4MB flash and 2MB PSRAM recommended

#### ESP32 (For SPDIF Output)
- Standard ESP32 development board
- GPIO pins available for SPDIF digital output
- Minimum 4MB flash recommended

### Audio Output Devices

#### For ESP32-S3
- USB Audio Class 1.0 compatible DAC
- Tested with various USB DACs supporting 16-bit, 48kHz audio
- Note: USB Audio Class 2.0 devices are not supported

#### For ESP32
- SPDIF receiver/DAC
- Connections to appropriate GPIO pins (defined in configuration)

### Power Supply Considerations

- 5V power supply with at least 500mA capacity
- Clean power source recommended for audio applications
- USB power from computer or quality phone charger is typically sufficient
- Optional battery operation possible with appropriate voltage regulation

### Physical Connections

#### ESP32-S3 with USB DAC
- Connect USB DAC to the USB OTG port on the ESP32-S3
- Power the ESP32-S3 via its USB port or external power pins

#### ESP32 with SPDIF
- Connect SPDIF output pin to the SPDIF input of your DAC
- Follow proper digital audio interfacing practices (impedance matching, short connections)
- Provide appropriate power to both ESP32 and SPDIF receiver

## Build and Flash Instructions

### Development Environment Setup

1. Install ESP-IDF (v4.5 or newer recommended):
   - Follow the [official installation guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/index.html)
   - Ensure you have the correct target set:
     - For ESP32: `idf.py set-target esp32`
     - For ESP32-S3: `idf.py set-target esp32s3`

2. Required dependencies:
   - Python 3.6 or newer
   - Git
   - Make, CMake, Ninja

### Obtaining the Source Code

```bash
git clone https://github.com/netham45/esp32-scream-receiver.git
cd esp32-scream-receiver
```

### Configuration

Configure the project for your specific needs:

```bash
idf.py menuconfig
```

Key configuration areas:
- Component config → USB Host HAC Driver (for ESP32-S3)
- Component config → ESP WIFI Configuration
- Component config → Power Management

### Building the Project

#### For ESP32-S3 (USB Audio)

```bash
# Make sure USB audio is enabled
# Edit main/config.h and ensure IS_USB is defined and IS_SPDIF is commented out
idf.py build
```

#### For ESP32 (SPDIF)

```bash
# Make sure SPDIF is enabled
# Edit main/config.h and ensure IS_SPDIF is defined and IS_USB is commented out
idf.py build
```

### Flashing Instructions

Connect your ESP32/ESP32-S3 to your computer via USB, then:

```bash
idf.py -p PORT flash
```

Replace `PORT` with your device's serial port (e.g., COM3 on Windows, /dev/ttyUSB0 on Linux).

For monitoring the device logs:

```bash
idf.py -p PORT monitor
```

### First Boot and Initial Setup

1. On first boot, the device will enter AP mode with SSID "ESP32-Scream"
2. Connect to this WiFi network from your computer or phone
3. A captive portal should automatically open (or navigate to 192.168.4.1)
4. Select your home WiFi network and enter credentials
5. The device will connect to your network and be ready to receive audio

## Architecture Overview

The ESP32 Scream Receiver is built as a multi-component system with real-time audio processing requirements. The architecture is designed to efficiently receive network audio data and output it to audio devices with minimal latency while managing power efficiently.

### System Architecture Diagram

```
┌─────────────────┐          ┌─────────────────┐          ┌─────────────────┐
│  WiFi Manager   │◄────────►│  Network Stack  │────────► │  Buffer Manager │
└─────────────────┘          └─────────────────┘          └─────────────────┘
        ▲                                                         │
        │                                                         ▼
┌─────────────────┐                                      ┌─────────────────┐
│   Web Server    │                                      │Audio Processing │
└─────────────────┘                                      └─────────────────┘
                                                                 │
┌─────────────────┐                                              ▼
│Power Management │◄────────────────────────────────────►┌─────────────────┐
└─────────────────┘                                      │ USB/SPDIF Driver│
                                                         └─────────────────┘
```

### Main Subsystems

1. **WiFi Manager**: Handles network connectivity, scanning, connection management, and roaming
2. **Network Stack**: Receives Scream audio packets via UDP/TCP and processes headers
3. **Buffer Manager**: Handles audio data buffering to prevent underruns/overruns
4. **Audio Processing**: Processes audio data and prepares it for output
5. **USB/SPDIF Driver**: Outputs audio data to the connected audio device
6. **Power Management**: Controls system power states based on audio activity
7. **Web Server**: Provides configuration interface via browser

### Threading Model

The system uses FreeRTOS tasks distributed across both cores of the ESP32:

- **Core 0**: Primarily handles network communication, WiFi management, and system tasks
  - USB/UAC host task
  - WiFi event handling
  - Network monitoring task (during sleep)
  
- **Core 1**: Primarily handles real-time audio processing
  - Network receive task (UDP/TCP)
  - Audio output tasks
  - Buffer management

Tasks use various priorities to ensure audio processing is prioritized appropriately.

### Memory Architecture

- Network receive buffers for UDP/TCP data
- Audio ring buffer with configurable size (default 16 chunks)
- Dynamic buffer sizing based on network conditions
- Direct memory transfers where possible to minimize copying

## Core Components - Audio Subsystem

### Audio Pipeline Overview

The audio subsystem handles the path from network packets to audio output, managing buffering, timing, and device interfacing.

```
Network Packets → Header Parsing → Audio Buffer → [ESP32-S3] → USB Audio Class Driver → USB DAC
                                              → [ESP32]   → SPDIF Output → SPDIF Receiver
```

### USB Audio Implementation (ESP32-S3)

The ESP32-S3 version uses the ESP-IDF USB Host stack with the UAC (USB Audio Class) driver:

- **Device Detection**: Automatically detects and enumerates USB Audio Class 1.0 devices
- **Audio Configuration**: Configures the DAC for 16-bit, 48kHz stereo PCM audio
- **Stream Management**: Handles USB isochronous transfers for continuous audio
- **Volume Control**: Provides volume adjustment through the USB interface
- **Error Recovery**: Handles USB disconnection events and reconnection

Key components:
- `uac_host_device_callback`: Manages device events (connect, disconnect)
- `start_playback`: Initializes audio streaming to the DAC
- `stop_playback`: Safely stops audio streaming

### SPDIF Implementation (ESP32)

The ESP32 version generates SPDIF output directly through GPIO pins:

- **Protocol Handling**: Implements SPDIF frame structure and timing
- **Hardware Timing**: Uses hardware timers for precise bit timing
- **Buffer Management**: Ensures continuous data flow to prevent glitches

### Audio Quality Considerations

- **Buffering Strategy**: Dynamic buffer sizing balances latency vs. stability
  - Initial buffer size: 4 chunks (configurable)
  - Maximum buffer size: 16 chunks (configurable)
  - Buffer growth on underrun: configurable
  
- **Silence Detection**:
  - Monitors audio amplitude to detect silence periods
  - Threshold: 10 (out of 32767) for amplitude
  - Duration: 30000ms of silence triggers sleep mode
  
- **Clock Synchronization**:
  - Output sample rate locked to 48kHz
  - No sample rate conversion implemented
  - May lead to eventual buffer underrun/overrun with imperfect clocks

## Core Components - Network Stack

### Network Protocol Implementation

The receiver implements the Scream network protocol:

- **Packet Format**:
  - 5-byte header + PCM audio data
  - PCM format: 16-bit, 48kHz stereo (default)
  
- **Transport Protocols**:
  - UDP (default): Lower latency, connectionless
  - TCP: Optional, provides reliable delivery but higher latency
  - Auto-switching: Can detect and switch to TCP when available

### Socket Management

- **UDP Socket**: Bound to port 4010 (configurable in config.h)
- **TCP Socket**: Connection established to detected Scream sender
- **QoS Settings**: IP_TOS priority set to 6 (network control)
- **TCP_NODELAY**: Enabled on TCP sockets to reduce latency

### Network Data Flow

1. Socket receives data into temporary buffer
2. Header is parsed to validate packet
3. Audio data is extracted and sent directly to the audio subsystem
4. Packets are tracked for activity monitoring during sleep modes

### Performance Optimizations

- **Direct Writing**: Audio data written directly to output device when possible
- **Header Skipping**: 5-byte header efficiently skipped without separate parsing step
- **Minimal Copying**: Data layout designed to minimize memory copies
- **Task Prioritization**: Network tasks prioritized appropriately for real-time audio

### Failure Recovery

- **UDP Mode**: Automatically maintains listening socket, recovers instantly
- **TCP Mode**: Detects disconnection, reverts to UDP listening mode
- **Reconnection**: Handles recovery from network changes or sender restarts

## Core Components - WiFi Management

### WiFi Manager Architecture

The WiFi manager handles all aspects of network connectivity with a state-machine design:

```
                  ┌─────────────────┐
                  │Not Initialized  │
                  └────────┬────────┘
                           │
                           ▼
┌─────────────┐     ┌─────────────────┐     ┌─────────────────┐
│   AP Mode   │◄────│    Connecting   │────►│    Connected    │
└──────┬──────┘     └─────────────────┘     └─────────────────┘
       │                      ▲                      │
       │                      │                      │
       └──────────────────────┴──────────────────────┘
```

- **State Machine States**:
  - `WIFI_MANAGER_STATE_NOT_INITIALIZED`: Initial state
  - `WIFI_MANAGER_STATE_CONNECTING`: Attempting to connect to a network
  - `WIFI_MANAGER_STATE_CONNECTED`: Successfully connected
  - `WIFI_MANAGER_STATE_CONNECTION_FAILED`: Connection attempt failed
  - `WIFI_MANAGER_STATE_AP_MODE`: Operating as an access point for configuration

### Connection Strategies

- **Stored Credentials**: Uses previously saved WiFi credentials from NVS
- **Strongest Network**: Can scan and connect to the strongest known network
- **AP Fallback**: Creates "ESP32-Scream" access point when no credentials available
- **Reset Option**: GPIO 0 or 1 held during boot clears saved WiFi credentials

### WiFi Roaming Capabilities

The system implements advanced WiFi roaming features for better coverage:

- **802.11k Support**: Neighbor report requests to identify nearby APs
- **802.11v Support**: BSS transition management for smooth handovers
- **RSSI Monitoring**: Triggers roaming when signal strength drops below -58dBm
- **Seamless Transition**: Maintains audio streaming during roaming events

### Configuration Storage

- **NVS Storage**: WiFi credentials securely stored in non-volatile storage
- **Multiple Networks**: Can store and select from multiple known networks
- **Persistent Configuration**: Settings retained across power cycles

### Web Configuration Interface

- **Captive Portal**: Automatically redirects to configuration page
- **Network Scanning**: Shows available networks with signal strength
- **Password Management**: Securely configures WiFi credentials
- **Simple UI**: Mobile-friendly interface for easy setup

## Core Components - Power Management

### Power States Overview

The system implements multiple power states to maximize efficiency:

1. **Normal Operation**: Full functionality, all subsystems active
   - CPU at normal frequency
   - WiFi in standard mode
   - USB/SPDIF active
   - All tasks running

2. **Light Sleep Mode** (Silence Sleep): Activated during audio silence
   - CPU in light sleep between operations
   - WiFi in power save mode
   - USB device detached but system ready
   - Network monitoring active

3. **Deep Sleep Mode**: Activated when no DAC is connected
   - Most systems powered down
   - Wake timer active
   - Periodic wake to check for DAC connection

### Power Saving Techniques

- **CPU Frequency Scaling**: Reduced to 80MHz from 240MHz
- **WiFi Power Save**: `WIFI_PS_MIN_MODEM` or `WIFI_PS_MAX_MODEM` depending on state
- **Task Suspension**: Non-critical tasks suspended during sleep modes
- **Peripheral Power Management**: Unused peripherals disabled

### Silence Detection

The system monitors audio data to detect silence periods:

- **Amplitude Threshold**: Value below 10 (out of 32767) considered silence
- **Duration Threshold**: 30 seconds (configurable) of continuous silence
- **False Positive Prevention**: Short silence periods ignored

### Network Activity Monitoring

During light sleep mode, network packets are monitored:

- **Packet Counting**: Tracks incoming Scream packets
- **Activity Threshold**: 2 packets (configurable) triggers wake
- **Inactivity Timeout**: 5 seconds without packets maintains sleep

### Wake Conditions

- **DAC Connection**: Wakes from deep sleep when USB DAC is connected
- **Network Activity**: Wakes from light sleep when audio streaming resumes
- **Timer Wake**: Periodic wake from deep sleep to check for DAC
- **Manual Wake**: GPIO button press during boot for configuration reset

## Program Flow

### Initialization Sequence

The system follows a specific boot sequence:

1. **Hardware Initialization**:
   - NVS (Non-Volatile Storage) initialization
   - Check for wake reason (power-on vs sleep wake)
   - GPIO configuration for reset detection

2. **Optional WiFi Reset**:
   - 3-second window to press GPIO 0 or 1
   - Clears stored WiFi credentials if pressed

3. **USB/SPDIF Initialization**:
   - USB host stack initialization (ESP32-S3)
   - Device enumeration and detection
   - DAC presence detection

4. **Power Management Setup**:
   - Configure CPU frequency scaling
   - Set up sleep modes

5. **WiFi Initialization**:
   - Initialize WiFi manager
   - Register event handlers
   - Connect to strongest network or start AP mode

6. **Audio Subsystem Initialization**:
   - Buffer setup
   - Audio output configuration
   - Stream parameter configuration

7. **Network Stack Initialization**:
   - Create UDP listener socket
   - Configure socket parameters
   - Start network tasks

### Audio Data Path

The audio data flows through the system in the following sequence:

1. **Network Reception**:
   - UDP/TCP socket receives Scream packet
   - Header validation (5 bytes)

2. **Buffer Management**:
   - Audio data extracted from packet
   - Direct writing to audio output or buffering based on configuration

3. **Audio Processing**:
   - PCM data sent to USB/SPDIF driver
   - Volume adjustment if configured

4. **Output Generation**:
   - USB: Data sent via isochronous transfers
   - SPDIF: Data formatted and output via GPIO

### Connection Management Flow

1. **Initial Connection**:
   - WiFi manager attempts connection with stored credentials
   - Falls back to AP mode if connection fails
   - Web server provides configuration interface

2. **Network Monitoring**:
   - RSSI levels continuously monitored
   - Roaming triggers when signal drops below threshold
   - Neighbor reports requested to identify better APs

3. **Roaming Execution**:
   - Transition management to new AP
   - Network stack handles temporary disconnection
   - Audio buffers prevent interruption during transition

### Power State Transitions

1. **Normal to Light Sleep** (Silence Detection):
   - Audio amplitude below threshold for 30 seconds
   - Stop audio processing and detach USB device
   - Configure WiFi for maximum power saving
   - Start network monitoring task
   - Enter light sleep between operations

2. **Light Sleep to Normal** (Activity Detection):
   - Network packets detected above threshold
   - Exit light sleep mode
   - Reconfigure WiFi for normal operation
   - Reconnect USB device and resume audio

3. **Normal to Deep Sleep** (No DAC):
   - DAC disconnection detected
   - Stop WiFi and network stack
   - Configure wake timer
   - Enter deep sleep mode

4. **Deep Sleep to Normal** (DAC Detection):
   - Timer wake or external wake
   - Check for DAC presence
   - If DAC detected, initialize full system
   - If no DAC, return to deep sleep

## Configuration Options

### Runtime Configuration

The system can be configured at runtime through the web interface:

- **WiFi Settings**:
  - Network selection
  - Password configuration
  - Signal strength display

### Build-time Configuration

Several parameters can be modified in `config.h`:

```c
// TCP port for Scream server data
#define PORT 4010

// Buffer size configuration
#define INITIAL_BUFFER_SIZE 4
#define BUFFER_GROW_STEP_SIZE 0
#define MAX_BUFFER_SIZE 16
#define MAX_GROW_SIZE 4

// Audio configuration
#define SAMPLE_RATE 48000
#define BIT_DEPTH 16
#define VOLUME 1.0f

// Sleep configuration
#define DAC_CHECK_SLEEP_TIME_MS 2000
#define SILENCE_THRESHOLD_MS 30000
#define NETWORK_CHECK_INTERVAL_MS 1000
#define ACTIVITY_THRESHOLD_PACKETS 2
#define SILENCE_AMPLITUDE_THRESHOLD 10
#define NETWORK_INACTIVITY_TIMEOUT_MS 5000

// Output mode selection
//#define IS_SPDIF
#define IS_USB
```

### Audio Settings

- **Sample Rate**: Fixed at 48kHz (device limitation)
- **Bit Depth**: 16-bit PCM (configurable in Scream Windows driver)
- **Channels**: Stereo (2 channels)
- **Volume**: Adjustable from 0.0 to 1.0

### Network Parameters

- **Port**: Default 4010, configurable
- **Protocol**: Auto-selecting between UDP and TCP
- **QoS**: Network control priority (6)
- **Buffer Configuration**: Tunable for latency vs. stability tradeoff

### Power Management Options

- **Silence Threshold**: Duration of silence before sleep mode
- **Activity Detection**: Number of packets to trigger wake
- **Sleep Behavior**: Timer duration and wake conditions
- **CPU Frequency**: Power vs. performance balance

## Technical Details

### Scream Protocol Implementation

The system implements the Scream network protocol:

- **Packet Format**:
  - 5-byte header: Contains information about the audio stream
  - PCM Audio Data: Raw 16-bit samples, interleaved stereo
  
- **Sample Format**:
  - 16-bit signed PCM
  - Little-endian byte order
  - 48kHz fixed sample rate
  - Stereo channel configuration

- **Transport**:
  - UDP multicast or unicast (primary)
  - TCP fallback for reliability
  - Automatic protocol detection and switching

### Resource Usage

The actual resource usage will vary depending on specific hardware configuration and network conditions.

### Critical Paths and Timing

- **Audio Output**: Isochronous USB transfers or precisely timed SPDIF output
- **Network Reception**: High priority task for minimal packet processing delay
- **Buffer Management**: Critical for preventing audio underruns

### ESP32 vs ESP32-S3 Differences

- **USB Support**: ESP32-S3 has native USB OTG support required for USB audio
- **Memory Architecture**: ESP32-S3 has more integrated RAM and optional PSRAM
- **Output Method**: ESP32 uses SPDIF output via GPIO, ESP32-S3 uses USB
- **Build Configuration**: Different `sdkconfig` defaults for each target

### Limitations and Known Issues

- **Sample Rate**: Fixed at 48kHz, no sample rate conversion
- **Bit Depth**: 16-bit only, no support for 24/32-bit audio
- **USB Compatibility**: Only works with USB Audio Class 1.0 devices, USB Audio Class 2.0 is not supported
- **WiFi Range**: Roaming helps, but audio may stutter during AP transitions
- **Power Management**: Deep sleep mode requires manual wake to reconfigure if WiFi changes

## Troubleshooting and FAQs

### Common Issues

#### No Audio Output

- **Verify DAC Connection**: Check that the USB DAC is properly connected and powered
- **Check WiFi Connection**: Ensure the ESP32 is connected to the same network as the Scream sender
- **Verify Scream Configuration**: Ensure Scream is configured to send to the correct network interface and port

#### Audio Stuttering or Dropouts

- **WiFi Signal Strength**: Move closer to your access point or add additional APs for roaming
- **Network Congestion**: Ensure your network has sufficient bandwidth
- **Buffer Settings**: Increase buffer size in `config.h` at the cost of higher latency

#### Cannot Connect to WiFi

- **Reset WiFi Configuration**: Hold GPIO 0 or 1 during boot for 3 seconds
- **Check AP Mode**: Connect to "ESP32-Scream" WiFi network and configure through web interface
- **Verify Credentials**: Ensure your WiFi password is correct

### Diagnostic Procedures

- **Serial Monitoring**: Use `idf.py monitor` to view debug output
- **LED Indicators**: Different blink patterns indicate various states
- **Web Interface**: Access device status through the web configuration page

### FAQ

**Q: Which USB DACs are compatible?**  
A: Most USB Audio Class 1.0 devices are compatible. USB Audio Class 2.0 devices are not supported.

**Q: Can I change the audio quality settings?**  
A: The audio format is fixed at 48kHz, 16-bit stereo due to device limitations.

**Q: How do I improve battery life?**  
A: Adjust silence detection thresholds in `config.h` for quicker entry into sleep modes.

**Q: Can I use this with multiple computers?**  
A: Yes, the receiver will play audio from any computer running Scream on the same network.

**Q: Why does the device go to sleep when there's no audio?**  
A: This is the power-saving feature. It will automatically wake when audio streaming resumes.
