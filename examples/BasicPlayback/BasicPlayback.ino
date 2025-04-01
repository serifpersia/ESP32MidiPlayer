// Basic MIDI playback example. 
// To upload midi to ESP32's storage
// you can use my ESP32PartitionTool to upload LittleFS binary (chose LittleFS as SPIFFS type before upload)
// ESP32PartitionTool > https://github.com/serifpersia/esp32partitiontool

// Serial commands: play, pause, resume, stop


#include <LittleFS.h>
#include "ESP32MidiPlayer.h"

ESP32MidiPlayer midiPlayer;
const char* MIDI_FILE = "/test.mid";

void midiLoggerCallback(MidiLogLevel level, const char* format, ...) {
  const char* levelStr = "";
  switch (level) {
    case MIDI_LOG_NONE:    levelStr = "[NONE] "; break;
    case MIDI_LOG_FATAL:   levelStr = "[FATAL] "; break;
    case MIDI_LOG_ERROR:   levelStr = "[ERROR] "; break;
    case MIDI_LOG_WARN:    levelStr = "[WARN]  "; break;
    case MIDI_LOG_INFO:    levelStr = "[INFO]  "; break;
    case MIDI_LOG_DEBUG:   levelStr = "[DEBUG] "; break;
    case MIDI_LOG_VERBOSE: levelStr = "[VERB]  "; break;
    default: break;
  }

  char messageBuffer[256];
  va_list args;
  va_start(args, format);
  vsnprintf(messageBuffer, sizeof(messageBuffer), format, args);
  va_end(args);

  Serial.print(levelStr);
  Serial.println(messageBuffer);
}

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);
  delay(1000);

  Serial.println("Initializing MIDI Player...");
  
  if (!midiPlayer.begin()) {
    Serial.println("Failed to initialize MidiPlayer! Check LittleFS mount.");
    while (true) delay(1000); // Halt execution
  }

  // Enable logging callback (set to info by default)
  //midiPlayer.setLogger(MIDI_LOG_INFO, midiLoggerCallback);

  if (!LittleFS.exists(MIDI_FILE)) {
    Serial.printf("MIDI file %s not found in LittleFS!\n", MIDI_FILE);
    while (true) delay(1000);
  }

  if (!midiPlayer.loadFile(MIDI_FILE)) {
    Serial.printf("Failed to load MIDI file %s\n", MIDI_FILE);
    while (true) delay(1000);
  }

  Serial.println("MIDI Player ready. Commands: play, pause, resume, stop");
}

void handleSerialCommands() {
  if (Serial.available() > 0) {
    String command = Serial.readStringUntil('\n');
    command.trim();
    
    MidiPlayerState state = midiPlayer.getState();
    
    if (command.equalsIgnoreCase("play")) {
      if (state != MidiPlayerState::PLAYING) {
        midiPlayer.play();
        Serial.println("Playback started");
      } else {
        Serial.println("Already playing");
      }
    }
    else if (command.equalsIgnoreCase("pause")) {
      if (state == MidiPlayerState::PLAYING) {
        midiPlayer.pause();
        Serial.println("Playback paused");
      } else {
        Serial.println("Not playing - cannot pause");
      }
    }
    else if (command.equalsIgnoreCase("resume")) {
      if (state == MidiPlayerState::PAUSED) {
        midiPlayer.resume();
        Serial.println("Playback resumed");
      } else {
        Serial.println("Not paused - cannot resume");
      }
    }
    else if (command.equalsIgnoreCase("stop")) {
      if (state == MidiPlayerState::PLAYING || state == MidiPlayerState::PAUSED) {
        midiPlayer.stop();
        Serial.println("Playback stopped");
      } else {
        Serial.println("Already stopped");
      }
    }
    else {
      Serial.println("Unknown command. Use: play, pause, resume, stop");
    }
  }
}

void loop() {
  midiPlayer.update(); // Process MIDI events
  handleSerialCommands(); // Handle serial input

  // MIDI event handling
  uint8_t channel, note, velocity, controller, value, program;
  int bendValue;

  if (midiPlayer.isNoteOn(channel, note, velocity)) {
    Serial.printf("[MIDI] Note On - Channel: %d, Note: %d, Velocity: %d\n", 
                  channel, note, velocity);
  }
  if (midiPlayer.isNoteOff(channel, note)) {
    Serial.printf("[MIDI] Note Off - Channel: %d, Note: %d\n", 
                  channel, note);
  }
  if (midiPlayer.isControlChange(channel, controller, value)) {
    Serial.printf("[MIDI] Control Change - Channel: %d, Controller: %d, Value: %d\n", 
                  channel, controller, value);
  }
  if (midiPlayer.isProgramChange(channel, program)) {
    Serial.printf("[MIDI] Program Change - Channel: %d, Program: %d\n", 
                  channel, program);
  }
  if (midiPlayer.isPitchBend(channel, bendValue)) {
    Serial.printf("[MIDI] Pitch Bend - Channel: %d, Value: %d\n", 
                  channel, bendValue);
  }
}