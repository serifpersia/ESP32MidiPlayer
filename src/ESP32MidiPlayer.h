#ifndef ESP32MidiPlayer_H
#define ESP32MidiPlayer_H

#include <Arduino.h>
#include <FS.h>
#include <vector>
#include <cstdarg> // For va_list

// --- Log Level Definition ---
enum class MidiLogLevel {
    NONE = -1,  // Disable all logging
    ERROR = 0, // Critical errors that prevent operation
    WARN = 1,  // Warnings about potential issues or unexpected data
    INFO = 2,  // General informational messages (file loaded, playback start/stop)
    DEBUG = 3, // Detailed step-by-step debugging information
    VERBOSE = 4// Even more detailed info (e.g., raw byte reads - not used much here yet)
};

// --- Callback Function Pointer Types ---
// LogCallback now accepts the level
typedef void (*LogCallback)(MidiLogLevel level, const char* message);
typedef void (*NoteOnCallback)(uint8_t channel, uint8_t note, uint8_t velocity);
typedef void (*NoteOffCallback)(uint8_t channel, uint8_t note, uint8_t velocity);
typedef void (*ControlChangeCallback)(uint8_t channel, uint8_t controller, uint8_t value);
typedef void (*ProgramChangeCallback)(uint8_t channel, uint8_t program);
typedef void (*PitchBendCallback)(uint8_t channel, int16_t value); // +/- 8192
typedef void (*TempoChangeCallback)(uint32_t microsecondsPerQuarterNote);
typedef void (*TimeSignatureCallback)(uint8_t numerator, uint8_t denominator_pow2, uint8_t clocksPerMetronome, uint8_t thirtySecondNotesPerQuarter); // Keep denominator_pow2 for raw MIDI value if preferred
typedef void (*EndOfTrackCallback)(uint8_t trackIndex); // Called when a track finishes
typedef void (*PlaybackCompleteCallback)(); // Called when all tracks finish


// --- Playback State Enum ---
enum class PlaybackState {
    STOPPED,
    PLAYING,
    PAUSED
};

// --- Track Info Structure ---
struct TrackInfo {
    uint32_t startOffset = 0;
    uint32_t currentOffset = 0;
    uint64_t nextEventTick = 0; // Use 64-bit for potentially very long files/high tick counts
    uint8_t lastStatusByte = 0;
    bool endOfTrackReached = false;
};

class ESP32MidiPlayer {
public:
    // Constructor - Takes the filesystem to use (e.g., LittleFS)
    ESP32MidiPlayer(FS& filesystem);
    ~ESP32MidiPlayer();

    // --- Configuration ---
    void setLogCallback(LogCallback callback);
    void setLogLevel(MidiLogLevel level);      // Method to set log level
    void setNoteOnCallback(NoteOnCallback callback);
    void setNoteOffCallback(NoteOffCallback callback);
    void setControlChangeCallback(ControlChangeCallback callback);
    void setProgramChangeCallback(ProgramChangeCallback callback);
    void setPitchBendCallback(::PitchBendCallback callback);
    void setTempoChangeCallback(TempoChangeCallback callback);
    void setTimeSignatureCallback(TimeSignatureCallback callback);
    void setEndOfTrackCallback(EndOfTrackCallback callback);
    void setPlaybackCompleteCallback(PlaybackCompleteCallback callback);

    // --- File Handling & Playback Control ---
    bool load(const char* filename); // Load MIDI file header and prepare tracks
    void play();                     // Start playback from the beginning or resume if paused
    void pause();                    // Pause playback
    void resume();                   // Resume playback (alias for play() when paused)
    void stop();                     // Stop playback and reset

    // --- Main Loop Update ---
    // This MUST be called frequently in the main loop()
    void tick();

    // --- Status Queries ---
    PlaybackState getState() const;
    bool isPlaying() const;
    bool isPaused() const;
    uint32_t getCurrentTick() const; // Get the current playback position in MIDI ticks
    uint32_t getTempo() const; // Get current tempo in Microseconds Per Quarter Note

private:
    // --- Private Helper Methods ---
    void _resetPlaybackState();
    bool _parseFileHeader();
    bool _prepareTracks();
    uint32_t _readVariableLengthQuantity(uint32_t& offset); // Reads from currentOffset of a track
    uint32_t _readBytes(uint32_t offset, uint8_t* buffer, uint32_t length); // Reads from file
    uint32_t _peekBytes(uint32_t offset, uint8_t* buffer, uint32_t length); // Peeks from file
    uint8_t _readUint8(uint32_t& offset);
    uint16_t _readUint16BE(uint32_t& offset); // Read Big Endian Short
    uint32_t _readUint32BE(uint32_t& offset); // Read Big Endian Long
    void _processNextEvent();
    int _findTrackWithNextEvent() const; // Returns index of track with earliest nextEventTick, or -1
    // Updated signature:
    void _handleMidiEvent(uint8_t trackIndex, uint8_t statusByte, uint32_t& trackOffset, bool runningStatusUsed, uint8_t data1_val);
    void _handleMetaEvent(uint8_t trackIndex, uint32_t& trackOffset);
    void _handleSysexEvent(uint8_t trackIndex, uint8_t type, uint32_t& trackOffset);
    void _advanceTickTime();
    // Updated signature:
    void _log(MidiLogLevel level, const char* format, ...); // Internal logging helper

    // --- Member Variables ---
    FS& _fs;                     // Filesystem reference
    File _midiFile;              // Current MIDI file handle
    String _filename = "";       // Current filename
    PlaybackState _state = PlaybackState::STOPPED;
    MidiLogLevel _currentLogLevel = MidiLogLevel::INFO; // Default log level

    // MIDI Header Info
    uint16_t _format = 0;
    uint16_t _trackCount = 0;
    uint16_t _division = 96;    // Ticks Per Quarter Note (TPQN)

    // Playback Timing
    uint32_t _microsecondsPerQuarterNote = 500000; // Default: 120 BPM
    uint64_t _currentTick = 0;
    uint64_t _playbackStartMicros = 0;
    uint64_t _lastEventMicros = 0;
    uint64_t _pauseStartMicros = 0; // To calculate paused duration

    // Track Data
    std::vector<TrackInfo> _tracks;
    uint8_t _finishedTracks = 0; // Count of tracks that reached EOT

    // Callbacks
    LogCallback _logCallback = nullptr;
    NoteOnCallback _noteOnCallback = nullptr;
    NoteOffCallback _noteOffCallback = nullptr;
    ControlChangeCallback _controlChangeCallback = nullptr;
    ProgramChangeCallback _programChangeCallback = nullptr;
	::PitchBendCallback _pitchBendCallback = nullptr;
    TempoChangeCallback _tempoChangeCallback = nullptr;
    TimeSignatureCallback _timeSignatureCallback = nullptr;
    EndOfTrackCallback _endOfTrackCallback = nullptr;
    PlaybackCompleteCallback _playbackCompleteCallback = nullptr;

    // Internal buffer (optional)
    // uint8_t _readBuffer[64];
    // uint32_t _bufferOffset = 0;
    // uint32_t _bufferSize = 0;
    // uint32_t _bufferFilePos = 0;
};

#endif // ESP32_MIDI_PLAYER_H