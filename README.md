# ESP32MidiPlayer Library

Arduino MIDI player library for ESP32 microcontrollers that streams and parses MIDI files from LittleFS. This library provides real-time MIDI event processing with playback control and logging features.

## Features
- Streams MIDI files from LittleFS
- Supports playback states: STOPPED, PLAYING, PAUSED, FINISHED, ERROR.
- Handles MIDI events: Note On/Off, Control Change, Program Change, Pitch Bend.
- Configurable logging with customizable levels (NONE, FATAL, ERROR, WARN, INFO, DEBUG, VERBOSE).

## Installation
1. **Manual Installation**:
   - Download latest release zip
   - In the Arduino IDE, go to `Sketch > Include Library > Add .ZIP Library` and select the downloaded file.
2. **Arduino Library Manager** (once published):
   - Search for "ESP32MidiPlayer" and install it.

## Dependencies
- **LittleFS**: Required for file system operations. Install via Arduino Library Manager.

## Usage
Open example project and send playback controls via serial monitor to test midi playback.

- ESP32PartitionTool is recommended to upload test midi file located in data directory inside the example proejct. 