# ESP32MidiPlayer Arduino Library

[![Arduino Library](https://img.shields.io/badge/Arduino-Library-blue.svg)]()
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Version](https://img.shields.io/badge/Version-1.0.0-green.svg)](https://github.com/serifpersia/ESP32MidiPlayer)

A MIDI Player library for ESP32 microcontrollers that streams and parses MIDI files from LittleFS. This library provides real-time MIDI event processing with playback control and logging features.

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
- **LittleFS**: Required for library compile and file system operations inside your arduino project .ino file. ESP32 Arduino Core must be installed.

## Usage
Open example project and send playback controls via serial monitor to test midi playback.

- ESP32PartitionTool is recommended to upload test midi file located in data directory inside the example proejct. 


## License

This project is licensed under the [MIT License](LICENSE).
