// Basic MIDI playback example. 
// To upload midi to ESP32's storage
// you can use my ESP32PartitionTool to upload LittleFS binary (chose LittleFS as SPIFFS type before upload)
// ESP32PartitionTool > https://github.com/serifpersia/esp32partitiontool

// Serial commands: play, pause, resume, stop


#include <LittleFS.h>
#include "ESP32MidiPlayer.h"

ESP32MidiPlayer midiPlayer(LittleFS); // Use LittleFS

const char* MIDI_FILE = "/test.mid";


void handleLog(MidiLogLevel level, const char* message) {
  const char* levelStr = "";
  switch (level) {
    case MidiLogLevel::ERROR:
      levelStr = "[ERR] "; // Errors that might halt playback or indicate corruption
      break;
    case MidiLogLevel::WARN:
      levelStr = "[WRN] "; // Warnings about unexpected data or potential issues
      break;
    case MidiLogLevel::INFO:
      levelStr = "[INF] "; // General information (playback start/stop, file loaded)
      break;
    case MidiLogLevel::DEBUG:
      levelStr = "[DBG] "; // Detailed debugging steps (event parsing, byte reads)
      break;
    case MidiLogLevel::VERBOSE:
      levelStr = "[VER] "; // Extremely detailed info (often too noisy)
      break;
    // case MidiLogLevel::NONE: // No need to handle NONE, the library checks this
    default:
      levelStr = "[???] "; // Unknown level? Should not happen.
      break;
  }
  // Print the prefix and the message, followed by a newline
  Serial.printf("%s%s\n", levelStr, message);
}

void handleNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
Serial.printf("[EVT] Note On:  Ch=%u Note=%u Vel=%u (Tick: %lu)\n",
channel + 1, note, velocity, midiPlayer.getCurrentTick()); // Display channel 1-16
}

void handleNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) {
Serial.printf("[EVT] Note Off: Ch=%u Note=%u Vel=%u (Tick: %lu)\n",
channel + 1, note, velocity, midiPlayer.getCurrentTick()); // Display channel 1-16
}

void handleControlChange(uint8_t channel, uint8_t controller, uint8_t value) {
Serial.printf("[EVT] Ctrl Chg: Ch=%u CC=%u Val=%u (Tick: %lu)\n",
channel + 1, controller, value, midiPlayer.getCurrentTick()); // Display channel 1-16
}

void handleProgramChange(uint8_t channel, uint8_t program) {
Serial.printf("[EVT] Prog Chg: Ch=%u Prog=%u (Tick: %lu)\n",
channel + 1, program, midiPlayer.getCurrentTick()); // Display channel 1-16
}

void handlePitchBend(uint8_t channel, int16_t value) {
Serial.printf("[EVT] Pitch Bnd: Ch=%u Val=%d (Tick: %lu)\n",
channel + 1, value, midiPlayer.getCurrentTick()); // Display channel 1-16
}

void handleTempoChange(uint32_t microsecondsPerQuarterNote) {
float bpm = 60000000.0f / microsecondsPerQuarterNote;
Serial.printf("[EVT] Tempo Chg: %lu us/qn (%.2f BPM) (Tick: %lu)\n",
microsecondsPerQuarterNote, bpm, midiPlayer.getCurrentTick());
}

void handleTimeSignature(uint8_t num, uint8_t den_pow2, uint8_t clocks, uint8_t b) {
Serial.printf("[EVT] Time Sig: %u/%u Clocks=%u 32nds/QN=%u (Tick: %lu)\n",
num, (1 << den_pow2), clocks, b, midiPlayer.getCurrentTick());
}

void handleEndOfTrack(uint8_t trackIndex) {
Serial.printf("[INF] EndOfTrack reached for track %u (Tick: %lu)\n",
trackIndex, midiPlayer.getCurrentTick());
}

void handlePlaybackComplete() {
Serial.println("\n[INF] === Playback Finished ===\n");
}

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);
  delay(1000);

  Serial.println("Initializing MIDI Player...");


  // Enable logging callback (set to info by default)
  //midiPlayer.setLogCallback(handleLog);
  //midiPlayer.setLogLevel(MidiLogLevel::INFO);

  midiPlayer.setNoteOnCallback(handleNoteOn);
  midiPlayer.setNoteOffCallback(handleNoteOff);
  midiPlayer.setControlChangeCallback(handleControlChange);
  midiPlayer.setProgramChangeCallback(handleProgramChange);
  midiPlayer.setPitchBendCallback(handlePitchBend);
  midiPlayer.setTempoChangeCallback(handleTempoChange);
  midiPlayer.setTimeSignatureCallback(handleTimeSignature);
  midiPlayer.setEndOfTrackCallback(handleEndOfTrack);
  midiPlayer.setPlaybackCompleteCallback(handlePlaybackComplete);

  if (!LittleFS.exists(MIDI_FILE)) {
    Serial.printf("MIDI file %s not found in LittleFS!\n", MIDI_FILE);
    while (true) delay(1000);
  }

  if (!midiPlayer.load(MIDI_FILE)) {
    Serial.printf("Failed to load MIDI file %s\n", MIDI_FILE);
    while (true) delay(1000);
  }

  Serial.println("MIDI Player ready. Commands: play, pause, resume, stop");
}

void handleSerialCommands() {
  if (Serial.available() > 0) {
    String command = Serial.readStringUntil('\n');
    command.trim();
    
    PlaybackState state = midiPlayer.getState();
    
    if (command.equalsIgnoreCase("play")) {
      if (state != PlaybackState::PLAYING) {
        midiPlayer.play();
        Serial.println("Playback started");
      } else {
        Serial.println("Already playing");
      }
    }
    else if (command.equalsIgnoreCase("pause")) {
      if (state == PlaybackState::PLAYING) {
        midiPlayer.pause();
        Serial.println("Playback paused");
      } else {
        Serial.println("Not playing - cannot pause");
      }
    }
    else if (command.equalsIgnoreCase("stop")) {
      if (state == PlaybackState::PLAYING || state == PlaybackState::PAUSED) {
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
  midiPlayer.tick(); // Process MIDI events
  handleSerialCommands(); // Handle serial input
}