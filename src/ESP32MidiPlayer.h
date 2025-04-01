#ifndef ESP32MidiPlayer_h
#define ESP32MidiPlayer_h

#include <Arduino.h>
#include "FS.h"
#include "LittleFS.h"
#include <vector>
#include <limits>
#include <queue>
#include <utility>

// Buffer size for reading track data
#define MIDI_PLAYER_TRACK_BUFFER_SIZE 256

// Define the possible states of the MIDI player
enum class MidiPlayerState {
    STOPPED,
    PLAYING,
    PAUSED,
    FINISHED,
    ERROR
};

// Logging Levels
typedef enum {
    MIDI_LOG_NONE = 0,
    MIDI_LOG_FATAL = 1,
    MIDI_LOG_ERROR = 2,
    MIDI_LOG_WARN = 3,
    MIDI_LOG_INFO = 4,
    MIDI_LOG_DEBUG = 5,
    MIDI_LOG_VERBOSE = 6
} MidiLogLevel;

// Logging callback function pointer type
typedef void (*MidiLogCallback)(MidiLogLevel level, const char* format, ...);

class ESP32MidiPlayer {
public:
    ESP32MidiPlayer();
    ~ESP32MidiPlayer();
    bool begin();
    bool loadFile(const char* filename);
    void play();
    void stop();
    void pause();
    void resume();

    MidiPlayerState getState() const;

    // MIDI event queries
    bool isNoteOn(byte& channel, byte& note, byte& velocity);
    bool isNoteOff(byte& channel, byte& note);
    bool isControlChange(byte& channel, byte& controller, byte& value);
    bool isProgramChange(byte& channel, byte& program);
    bool isPitchBend(byte& channel, int& bendValue);

    void update();

    // Logging setup
    void setLogger(MidiLogLevel level, MidiLogCallback callback);

private:
    // Internal structure to hold MIDI event data temporarily during processing
    struct MidiEventData {
        byte status;
        byte data1;
        byte data2;
    };

    // Internal structure to manage the state of each track during streaming
    struct TrackState {
        size_t trackChunkStart = 0;
        size_t trackChunkEnd = 0;
        size_t currentOffset = 0;
        unsigned long nextEventTick = 0;
        byte runningStatus = 0;
        bool finished = false;

        byte buffer[MIDI_PLAYER_TRACK_BUFFER_SIZE];
        size_t bufferFill = 0;
        size_t bufferPos = 0;
    };

    // Type alias for event queue entries: {tick, track_index}
    using EventQueueEntry = std::pair<unsigned long, size_t>;

    // Playback state and control
    MidiPlayerState _currentState = MidiPlayerState::STOPPED;
    File _midiFile;
    const char* _currentFilename = nullptr;

    // Timing variables
    unsigned long _ticksPerQuarterNote = 96;
    float _microsecondsPerQuarterNote = 500000.0f; // Default 120 BPM
    float _currentBPM = 120.0f;
    unsigned long _songStartMillis = 0;
    unsigned long _pauseStartMillis = 0;
    unsigned long _currentTick = 0;
    unsigned long _lastTickMillis = 0; // Real time timestamp of _currentTick

    // Track state storage
    std::vector<TrackState> _trackStates;
    size_t _activeTracks = 0;

    // Priority queue for efficient event scheduling
    // Stores {nextEventTick, trackIndex}, ordered by tick (min-heap)
    std::priority_queue<EventQueueEntry, std::vector<EventQueueEntry>, std::greater<EventQueueEntry>> _eventQueue;

    // Current event state
    byte _currentEventType = 0;
    byte _currentChannel = 0;
    byte _currentData1 = 0;
    byte _currentData2 = 0;
    bool _eventProcessed = false;

    // Logging members
    MidiLogCallback _logCallback = nullptr;
    MidiLogLevel _logLevel = MIDI_LOG_NONE;

    // --- Internal methods ---
    void resetPlayback();
    void closeFile();
    // *** ADDED const qualifier here ***
    void _log(MidiLogLevel level, const char* format, ...) const;

    // File/Buffer reading helpers
    bool readTrackByte(TrackState& track, byte& outByte);
    bool refillTrackBuffer(TrackState& track);

    // Parsing helpers
    unsigned long readVariableLengthQuantity(TrackState& track);
    bool parseEvent(TrackState& track, MidiEventData& eventData);
    bool initializeTrackStates();

    // Helper to calculate ms per tick
    float calculateMsPerTick() const;
};

#endif