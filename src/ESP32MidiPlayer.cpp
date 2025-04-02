#include "ESP32MidiPlayer.h" // Include the header first
#include <stdio.h>              // For snprintf

// --- Constants ---
const uint32_t MTHD_CHUNK_TYPE = 0x4D546864; // "MThd"
const uint32_t MTRK_CHUNK_TYPE = 0x4D54726B; // "MTrk"
const uint8_t META_EVENT = 0xFF;
const uint8_t META_END_OF_TRACK = 0x2F;
const uint8_t META_TEMPO = 0x51;
const uint8_t META_TIME_SIGNATURE = 0x58;
// Add other meta types if needed
const uint8_t META_TRACK_NAME = 0x03;
const uint8_t SYSEX_START = 0xF0;
const uint8_t SYSEX_END = 0xF7;


// --- Static Variables for One-Time Warnings ---
// These are declared at file scope (outside the class)
static bool _divisionWarningLogged = false;
static bool _tempoWarningLogged = false;

// --- Helper Function to Estimate VLQ byte length ---
// (Not part of the class, just a utility for this file)
static uint8_t _getVlqLength(uint32_t value) {
    if (value < 0x80) return 1;      // Max 0x7F
    if (value < 0x4000) return 2;    // Max 0x3FFF
    if (value < 0x200000) return 3;   // Max 0x1FFFFF
    if (value < 0x10000000) return 4; // Max 0x0FFFFFFF (Standard MIDI Limit)
    // Technically possible to encode larger, but highly unlikely/non-standard
    return 5; // Or handle as error? For logging offset, 4 or 5 is usually fine.
}

ESP32MidiPlayer::ESP32MidiPlayer(FS& filesystem) : _fs(filesystem) {
    // Initialize default state
    _resetPlaybackState();
}

ESP32MidiPlayer::~ESP32MidiPlayer() {
    stop(); // Ensure file is closed if open
}

// --- Configuration ---
void ESP32MidiPlayer::setLogCallback(LogCallback callback) { _logCallback = callback; }
void ESP32MidiPlayer::setLogLevel(MidiLogLevel level) { _currentLogLevel = level; } // Added
void ESP32MidiPlayer::setNoteOnCallback(NoteOnCallback callback) { _noteOnCallback = callback; }
void ESP32MidiPlayer::setNoteOffCallback(NoteOffCallback callback) { _noteOffCallback = callback; }
void ESP32MidiPlayer::setControlChangeCallback(ControlChangeCallback callback) { _controlChangeCallback = callback; }
void ESP32MidiPlayer::setProgramChangeCallback(ProgramChangeCallback callback) { _programChangeCallback = callback; }
void ESP32MidiPlayer::setPitchBendCallback(PitchBendCallback callback) { _pitchBendCallback = callback; }
void ESP32MidiPlayer::setTempoChangeCallback(TempoChangeCallback callback) { _tempoChangeCallback = callback; }
void ESP32MidiPlayer::setTimeSignatureCallback(TimeSignatureCallback callback) { _timeSignatureCallback = callback; }
void ESP32MidiPlayer::setEndOfTrackCallback(EndOfTrackCallback callback) { _endOfTrackCallback = callback; }
void ESP32MidiPlayer::setPlaybackCompleteCallback(PlaybackCompleteCallback callback) { _playbackCompleteCallback = callback; }


// --- File Handling & Playback Control ---

void ESP32MidiPlayer::_resetPlaybackState() {
    _state = PlaybackState::STOPPED;
    _currentTick = 0;
    _microsecondsPerQuarterNote = 500000; // Reset tempo to 120 BPM
    _playbackStartMicros = 0;
    _lastEventMicros = 0;
    _pauseStartMicros = 0;
    _tracks.clear();
    _finishedTracks = 0;
    _format = 0;
    _trackCount = 0;
    _division = 96; // Default TPQN

    // Reset track-specific info
    for (auto& track : _tracks) {
        track.currentOffset = track.startOffset;
        track.nextEventTick = 0;
        track.lastStatusByte = 0;
        track.endOfTrackReached = false;
    }
}

bool ESP32MidiPlayer::load(const char* filename) {
    stop(); // Stop any current playback and close the file

    _filename = filename;
    _midiFile = _fs.open(filename, FILE_READ);
    if (!_midiFile) {
        _log(MidiLogLevel::ERROR, "Failed to open MIDI file '%s'", filename);
        _filename = "";
        return false;
    }
    _log(MidiLogLevel::INFO, "Opened MIDI file: %s (Size: %u)", filename, _midiFile.size());

    if (!_parseFileHeader()) {
        _log(MidiLogLevel::ERROR, "Invalid MIDI file header.");
        stop(); // Close file
        return false;
    }

    if (!_prepareTracks()) {
        _log(MidiLogLevel::ERROR, "Failed to find or parse track chunks.");
        stop(); // Close file
        return false;
    }

    _log(MidiLogLevel::INFO, "MIDI File Loaded: Format %u, Tracks %u, TPQN %u", _format, _trackCount, _division);
    _state = PlaybackState::STOPPED; // Ready to play
    return true;
}

void ESP32MidiPlayer::play() {
    if (!_midiFile) {
        _log(MidiLogLevel::ERROR, "No MIDI file loaded, cannot play.");
        return;
    }

    uint64_t now = micros();

    if (_state == PlaybackState::STOPPED) {
        _log(MidiLogLevel::DEBUG, "Starting playback from beginning.");
        _currentTick = 0;
        _finishedTracks = 0;
        // Reset track positions and next event times
        for (size_t i = 0; i < _tracks.size(); ++i) {
             auto& track = _tracks[i];
             track.currentOffset = track.startOffset;
             track.lastStatusByte = 0;
             track.endOfTrackReached = false;
             // Read the first delta time for this track
             uint32_t initialDeltaOffset = track.currentOffset;
             track.nextEventTick = _readVariableLengthQuantity(track.currentOffset);
             _log(MidiLogLevel::DEBUG, "T%d Initial delta %llu (read at offset %u, next offset %u)", i, track.nextEventTick, initialDeltaOffset, track.currentOffset);
        }
        _playbackStartMicros = now;
        _lastEventMicros = now;
        _state = PlaybackState::PLAYING;
        _log(MidiLogLevel::INFO, "Playback started.");
    } else if (_state == PlaybackState::PAUSED) {
        // Adjust timing based on pause duration
        uint64_t pausedDuration = now - _pauseStartMicros;
        _playbackStartMicros += pausedDuration; // Effectively shift start time forward
        _lastEventMicros += pausedDuration;     // Shift last event time forward too
        _state = PlaybackState::PLAYING;
        _log(MidiLogLevel::INFO, "Playback resumed after %llu us pause.", pausedDuration);
    } else if (_state == PlaybackState::PLAYING) {
        _log(MidiLogLevel::WARN, "Play command received while already playing.");
    }
}

void ESP32MidiPlayer::pause() {
    if (_state == PlaybackState::PLAYING) {
        _state = PlaybackState::PAUSED;
        _pauseStartMicros = micros();
        _log(MidiLogLevel::INFO, "Playback paused at tick %lu.", (uint32_t)_currentTick);
        // Optional: Send All Notes Off / All Sound Off CC messages if desired
        // for (uint8_t ch = 0; ch < 16; ++ch) {
        //     if (_controlChangeCallback) _controlChangeCallback(ch, 123, 0); // All notes off
        //     if (_controlChangeCallback) _controlChangeCallback(ch, 120, 0); // All sound off
        // }
    } else {
         _log(MidiLogLevel::WARN, "Pause command received but not playing.");
    }
}

void ESP32MidiPlayer::resume() {
    if (_state == PlaybackState::PAUSED) {
        play(); // Same logic for resuming as starting from paused state
    } else {
         _log(MidiLogLevel::WARN, "Resume command received but not paused.");
    }
}

void ESP32MidiPlayer::stop() {
    bool wasPlaying = (_state != PlaybackState::STOPPED);
    if (_midiFile) {
        _midiFile.close();
        if (wasPlaying) _log(MidiLogLevel::INFO, "MIDI file closed due to stop.");
        else _log(MidiLogLevel::DEBUG, "MIDI file closed."); // Might be closed during load error etc.
    }
    _filename = "";
    _resetPlaybackState(); // Resets state to STOPPED among other things

     if (wasPlaying) {
        _log(MidiLogLevel::INFO, "Playback stopped.");
        // Optional: Send All Notes Off / All Sound Off CC messages
        // for (uint8_t ch = 0; ch < 16; ++ch) {
        //     if (_controlChangeCallback) _controlChangeCallback(ch, 123, 0); // All notes off
        //     if (_controlChangeCallback) _controlChangeCallback(ch, 120, 0); // All sound off
        // }
     }
}

// --- Main Loop Update ---

void ESP32MidiPlayer::tick() {
    if (_state != PlaybackState::PLAYING) {
        return; // Only process if playing
    }

    // 1. Advance Tick Time based on micros()
    _advanceTickTime();

    // 2. Process all events scheduled up to the current tick
    while (true) {
        int nextTrackIdx = _findTrackWithNextEvent();

        // If no track has an event ready (or all tracks finished)
        if (nextTrackIdx < 0) {
            _log(MidiLogLevel::VERBOSE, "Tick %llu: No tracks ready.", _currentTick); // Usually too noisy
            break; // No tracks have events scheduled
        }

        // Check if the *earliest* event is actually due yet
        if (_tracks[nextTrackIdx].nextEventTick > _currentTick) {
            // _log(MidiLogLevel::VERBOSE, "Tick %llu: Earliest event on T%d is at tick %llu, not due yet.", _currentTick, nextTrackIdx, _tracks[nextTrackIdx].nextEventTick);
            break; // No more events ready at this exact moment
        }

        // Process the event from the chosen track
        // _log(MidiLogLevel::DEBUG, "Tick %llu: Processing event for T%d scheduled at tick %llu", _currentTick, nextTrackIdx, _tracks[nextTrackIdx].nextEventTick);
        _processNextEvent(); // This function finds the track internally again

         // Check if all tracks are finished AFTER processing an event
        if (_finishedTracks >= _trackCount) {
             _log(MidiLogLevel::INFO, "All tracks finished.");
             if (_playbackCompleteCallback) {
                 _playbackCompleteCallback();
             }
             stop(); // Stop playback automatically
             break; // Exit the while loop
        }
    }
}

// --- Status Queries ---
PlaybackState ESP32MidiPlayer::getState() const { return _state; }
bool ESP32MidiPlayer::isPlaying() const { return _state == PlaybackState::PLAYING; }
bool ESP32MidiPlayer::isPaused() const { return _state == PlaybackState::PAUSED; }
uint32_t ESP32MidiPlayer::getCurrentTick() const { return (uint32_t)_currentTick; } // Cast for typical usage
uint32_t ESP32MidiPlayer::getTempo() const { return _microsecondsPerQuarterNote; }

// --- Private Helper Methods Implementation ---

// Reads Big-Endian uint16 from file at current position + offset, advances position
uint16_t ESP32MidiPlayer::_readUint16BE(uint32_t& offset) {
    uint8_t buffer[2];
    if (_readBytes(offset, buffer, 2) == 2) {
        offset += 2;
        return ((uint16_t)buffer[0] << 8) | buffer[1];
    }
    _log(MidiLogLevel::ERROR, "Read error: Uint16BE at offset %u", offset);
    stop(); // Critical error, stop playback
    return 0;
}

// Reads Big-Endian uint32 from file at current position + offset, advances position
uint32_t ESP32MidiPlayer::_readUint32BE(uint32_t& offset) {
    uint8_t buffer[4];
    if (_readBytes(offset, buffer, 4) == 4) {
        offset += 4;
        return ((uint32_t)buffer[0] << 24) | ((uint32_t)buffer[1] << 16) | ((uint32_t)buffer[2] << 8) | buffer[3];
    }
     _log(MidiLogLevel::ERROR, "Read error: Uint32BE at offset %u", offset);
     stop(); // Critical error
    return 0;
}

// Reads a single byte
uint8_t ESP32MidiPlayer::_readUint8(uint32_t& offset) {
    uint8_t buffer[1];
    if (_readBytes(offset, buffer, 1) == 1) {
        offset += 1;
        return buffer[0];
    }
    _log(MidiLogLevel::ERROR, "Read error: Uint8 at offset %u", offset);
    stop(); // Critical error
    return 0;
}


// Reads bytes directly from file - basic implementation
uint32_t ESP32MidiPlayer::_readBytes(uint32_t offset, uint8_t* buffer, uint32_t length) {
    if (!_midiFile) {
        _log(MidiLogLevel::ERROR, "Read attempt failed: File not open (offset %u)", offset);
        return 0;
    }
    if (!_midiFile.seek(offset)) {
        _log(MidiLogLevel::ERROR, "Seek failed to offset %u", offset);
        // Consider stopping playback on seek fail? Maybe file closed unexpectedly.
        stop();
        return 0;
    }
    size_t bytesRead = _midiFile.read(buffer, length);
    if (bytesRead != length) {
        // This might happen legitimately if near EOF for some reads (like VLQ),
        // but can be an error for others (like fixed-size reads). The calling function should check.
         _log(MidiLogLevel::WARN, "Read incomplete at offset %u. Requested %u, got %u. EOF?", offset, length, bytesRead);
         // Should we stop? Depends if the caller expected exactly 'length' bytes.
         // For now, just return what was read. The caller must handle potential errors.
    }
    return bytesRead;
}
// Peeks bytes directly from file (reads without changing file position)
uint32_t ESP32MidiPlayer::_peekBytes(uint32_t offset, uint8_t* buffer, uint32_t length) {
     if (!_midiFile) {
        _log(MidiLogLevel::ERROR, "Peek attempt failed: File not open (offset %u)", offset);
        return 0;
    }
    size_t currentPos = _midiFile.position(); // Get current position
    if (!_midiFile.seek(offset)) {
         _log(MidiLogLevel::ERROR, "Peek seek failed to offset %u", offset);
        // Don't try to restore position if seek failed
        return 0;
    }
    size_t bytesRead = _midiFile.read(buffer, length);
    if (!_midiFile.seek(currentPos)) { // Attempt to restore position
         _log(MidiLogLevel::ERROR, "Peek failed to restore file position to %u after reading at %u", currentPos, offset);
         stop(); // If we can't restore position, state is likely corrupted
    }
     if (bytesRead != length) {
        _log(MidiLogLevel::WARN, "Peek incomplete at offset %u. Requested %u, got %u. EOF?", offset, length, bytesRead);
        // Return actual bytes read
    }
    return bytesRead;
}


// Reads a MIDI variable-length quantity from the file
uint32_t ESP32MidiPlayer::_readVariableLengthQuantity(uint32_t& offset) {
    uint32_t value = 0;
    uint8_t byte_in;
    uint32_t bytesReadCount = 0;
    uint32_t startOffset = offset; // For error logging

    do {
        if (_readBytes(offset, &byte_in, 1) != 1) {
             _log(MidiLogLevel::ERROR, "VLQ read error at file offset %u (started at %u)", offset, startOffset);
             stop(); // Can't recover if VLQ is cut short
             return 0; // Error reading byte
        }
        offset++;
        bytesReadCount++;
        value = (value << 7) | (byte_in & 0x7F);
        if (bytesReadCount > 4) {
            // Standard MIDI VLQs shouldn't exceed 4 bytes (representing up to 0x0FFFFFFF)
            _log(MidiLogLevel::ERROR, "VLQ too long (>4 bytes) at file offset %u (started at %u)", offset, startOffset);
            stop(); // Corrupt data
            return value; // Return potentially garbage value, but playback stopped
        }
    } while (byte_in & 0x80); // Continue if MSB is set

    return value;
}


bool ESP32MidiPlayer::_parseFileHeader() {
    uint32_t currentOffset = 0;
    // Read MThd chunk header
    uint32_t chunkType = _readUint32BE(currentOffset); // Offset advanced inside
    if (!_midiFile) return false; // Check if readUint32BE stopped playback
    uint32_t headerLength = _readUint32BE(currentOffset); // Offset advanced inside
    if (!_midiFile) return false;

    if (chunkType != MTHD_CHUNK_TYPE) {
        _log(MidiLogLevel::ERROR, "Invalid MThd chunk type (Expected 0x%08X, Got 0x%08X)", MTHD_CHUNK_TYPE, chunkType);
        return false;
    }
     if (headerLength < 6) {
        _log(MidiLogLevel::ERROR, "Invalid MThd header length (%u, expected >= 6)", headerLength);
        return false;
    }

    _format = _readUint16BE(currentOffset); // Offset advanced inside
     if (!_midiFile) return false;
    _trackCount = _readUint16BE(currentOffset); // Offset advanced inside
     if (!_midiFile) return false;
    _division = _readUint16BE(currentOffset); // Offset advanced inside
     if (!_midiFile) return false;


    // We only reliably support TPQN timing for now
    if (_division & 0x8000) {
        // SMPTE timing format (frames per second)
        int8_t framesPerSecond = -((int8_t)(_division >> 8)); // Negative frames per second code
        uint8_t ticksPerFrame = _division & 0xFF;
        _log(MidiLogLevel::WARN, "SMPTE timing format (FPS: %d, Ticks/Frame: %u) detected. Playback timing may be incorrect!", framesPerSecond, ticksPerFrame);
        // For simplicity, we'll *try* to continue using a default TPQN, but timing will be wrong.
        // A more robust library would handle SMPTE timing calculations.
        _division = 96; // Fallback TPQN
         _log(MidiLogLevel::WARN, "Falling back to TPQN = %u for timing calculations.", _division);
    } else {
         _log(MidiLogLevel::DEBUG, "TPQN (Ticks Per Quarter Note) division found: %u", _division);
    }

    // Skip any extra header data beyond the standard 6 bytes
    uint32_t extraData = headerLength - 6;
    if (extraData > 0) {
         _log(MidiLogLevel::DEBUG, "Skipping %u extra bytes in MThd header.", extraData);
         currentOffset += extraData;
         if (!_midiFile.seek(currentOffset)) {
             _log(MidiLogLevel::ERROR,"Failed to seek past extra MThd header data.");
             return false;
         }
    }

    return true;
}

bool ESP32MidiPlayer::_prepareTracks() {
    if (_trackCount == 0) {
        _log(MidiLogLevel::ERROR, "MIDI file header indicates 0 tracks.");
        return false;
    }
    _tracks.resize(_trackCount); // Allocate space for track info
    uint32_t currentOffset = 6 + 8; // Start searching after MThd ID(4)+Length(4)+Data(6)

    for (uint16_t i = 0; i < _trackCount; ++i) {
        bool trackFound = false;
        // Protect against infinite loop if file is corrupt
        uint32_t searchStartOffset = currentOffset;
        uint32_t fileSize = _midiFile.size();

        while (currentOffset < fileSize) {
             // Need at least 8 bytes for ID + Length
            if (currentOffset + 8 > fileSize) {
                 _log(MidiLogLevel::ERROR, "Reached EOF while searching for MTrk header for track %u (offset %u)", i, currentOffset);
                 return false;
            }

            uint32_t chunkType = _readUint32BE(currentOffset);
            if (!_midiFile) return false; // Read error check
            uint32_t chunkLength = _readUint32BE(currentOffset);
             if (!_midiFile) return false; // Read error check


            if (chunkType == MTRK_CHUNK_TYPE) {
                 _log(MidiLogLevel::INFO, "Found Track %u header at offset %u, data length %u", i, currentOffset - 8, chunkLength);
                _tracks[i].startOffset = currentOffset; // Start of track *data*
                _tracks[i].currentOffset = currentOffset; // Initially same
                _tracks[i].nextEventTick = 0; // Will be read on play()
                _tracks[i].endOfTrackReached = false;
                _tracks[i].lastStatusByte = 0;

                // Skip over this track's data to find the next one
                currentOffset += chunkLength;
                 // Basic sanity check for chunk length
                 if (currentOffset > fileSize) {
                      _log(MidiLogLevel::ERROR, "Track %u chunk length (%u) exceeds file size (%u) from offset %u", i, chunkLength, fileSize, _tracks[i].startOffset);
                      return false;
                 }
                trackFound = true;
                break; // Found track i, move to next outer loop iteration
            } else {
                // Handle potential non-MTrk chunks between MThd and MTrk, or between MTrk chunks
                char chunkTypeStr[5];
                chunkTypeStr[0] = (chunkType >> 24) & 0xFF;
                chunkTypeStr[1] = (chunkType >> 16) & 0xFF;
                chunkTypeStr[2] = (chunkType >> 8) & 0xFF;
                chunkTypeStr[3] = chunkType & 0xFF;
                chunkTypeStr[4] = '\0';
                 _log(MidiLogLevel::WARN, "Skipping unexpected chunk type '%s' (0x%08X) at offset %u, length %u", chunkTypeStr, chunkType, currentOffset - 8, chunkLength);
                 currentOffset += chunkLength; // Skip over this unknown chunk
                 // Sanity check
                  if (currentOffset > fileSize) {
                      _log(MidiLogLevel::ERROR, "Unexpected chunk '%s' length (%u) exceeds file size (%u) from offset %u", chunkTypeStr, chunkLength, fileSize, currentOffset - chunkLength - 8);
                      return false;
                 }
            }

            // Prevent infinite loop on corrupt files where offset doesn't advance
            if(currentOffset <= searchStartOffset && chunkLength == 0 && chunkType != MTRK_CHUNK_TYPE){
                _log(MidiLogLevel::ERROR, "Detected potential infinite loop parsing chunks near offset %u. Aborting.", currentOffset);
                return false;
            }
            searchStartOffset = currentOffset; // Update for next iteration check
        }
        if (!trackFound) {
             _log(MidiLogLevel::ERROR, "Could not find MTrk chunk for track %u after offset %u", i, searchStartOffset);
             return false;
        }
    }
    return true;
}

// Calculate elapsed ticks based on micros()
void ESP32MidiPlayer::_advanceTickTime() {
    uint64_t now = micros();

    // Handle potential micros() rollover (every ~71 minutes)
    // A simple check: if 'now' is less than 'last', assume rollover.
    // Note: This isn't perfectly robust if loop iterations are very long,
    // but usually sufficient for Arduino.
    uint64_t deltaMicros;
    if (now >= _lastEventMicros) {
        deltaMicros = now - _lastEventMicros;
    } else {
        // Rollover occurred
        deltaMicros = (UINT64_MAX - _lastEventMicros) + 1 + now;
        _log(MidiLogLevel::DEBUG, "micros() rollover detected.");
    }


    // Calculate microseconds per tick
    // Avoid division by zero if division is somehow invalid
    if (_division == 0) {
         if (!_divisionWarningLogged){ // Log only once
            _log(MidiLogLevel::ERROR, "MIDI division (TPQN) is zero! Cannot calculate timing. Defaulting to 96.");
            _divisionWarningLogged = true;
         }
        _division = 96;
    }
    // Use double for precision in calculation, especially for high TPQN or slow tempos
    double microsPerTick = (double)_microsecondsPerQuarterNote / _division;

    if (microsPerTick > 0) {
        // Calculate how many ticks should have passed
        uint64_t ticksElapsed = (uint64_t)(deltaMicros / microsPerTick);

        if (ticksElapsed > 0) {
            _currentTick += ticksElapsed;
            // Adjust _lastEventMicros precisely, avoiding drift accumulation
            // Use the calculated microsPerTick for the adjustment
            _lastEventMicros += (uint64_t)(ticksElapsed * microsPerTick);

            // Minor adjustment: If rollover occurred, _lastEventMicros might now be
            // slightly off due to calculation order. Re-syncing is complex.
            // The current approach minimizes drift but might have tiny jumps near rollover.
             // _log(MidiLogLevel::VERBOSE, "Advanced %llu ticks based on %llu us. New tick: %llu", ticksElapsed, deltaMicros, _currentTick);
        }
    }
     // Optional: Log if microsPerTick is zero (should only happen if tempo is 0, which is invalid MIDI)
     else if (_microsecondsPerQuarterNote == 0) {
         if (!_tempoWarningLogged) { // Log only once
            _log(MidiLogLevel::WARN, "Tempo is zero (usPerQN = 0), timing stalled.");
            _tempoWarningLogged = true;
         }
     }
}

// Find the track with the smallest nextEventTick that hasn't ended
int ESP32MidiPlayer::_findTrackWithNextEvent() const {
    int nextTrack = -1;
    uint64_t earliestTick = UINT64_MAX;

    for (int i = 0; i < _tracks.size(); ++i) {
        if (!_tracks[i].endOfTrackReached) {
             // If multiple tracks have the same earliest tick, prefer lower track index (standard practice)
            if (_tracks[i].nextEventTick < earliestTick) {
                earliestTick = _tracks[i].nextEventTick;
                nextTrack = i;
            }
        }
    }
    return nextTrack;
}


// Process the next due MIDI event from the correct track
void ESP32MidiPlayer::_processNextEvent() {
    int trackIdx = _findTrackWithNextEvent();
    if (trackIdx < 0) {
         _log(MidiLogLevel::DEBUG, "processNextEvent called but no track found with pending events.");
         return; // Should not happen if called correctly after check in tick()
    }

    TrackInfo& track = _tracks[trackIdx];

    // This event's tick timestamp (was calculated previously)
    uint64_t eventTick = track.nextEventTick;

    // Log the offset *before* reading anything for this event
    uint32_t eventStartOffset = track.currentOffset;

    // Read the first byte (status or data1)
    uint8_t firstByte = _readUint8(track.currentOffset);
    if (!_midiFile) return; // ReadUint8 might stop playback on error

     _log(MidiLogLevel::DEBUG, "T%d @ Tick %llu (Offset %u): Read first byte: 0x%02X",
         trackIdx, eventTick, eventStartOffset, firstByte);


    uint8_t statusByte;
    uint8_t data1_from_running = 0; // Store data1 if read here
    bool runningStatus = false;

    // Handle MIDI Running Status
    if (firstByte < 0x80) { // Data byte instead of status byte
        if (track.lastStatusByte == 0 || track.lastStatusByte < 0x80 || track.lastStatusByte >= 0xF0) {
             _log(MidiLogLevel::ERROR, "T%d @ Tick %llu (Offset %u): Running status error: Invalid or missing previous status byte (0x%02X).",
                  trackIdx, eventTick, eventStartOffset, track.lastStatusByte);
              track.endOfTrackReached = true; // Mark as finished to avoid corrupt data loops
              _finishedTracks++;
              return;
        }
        statusByte = track.lastStatusByte; // Reuse the last status byte
        data1_from_running = firstByte;    // This byte is actually the first data byte
        runningStatus = true;
         _log(MidiLogLevel::DEBUG, "T%d @ Tick %llu: Applying Running Status. Reusing Status 0x%02X, Data1 = 0x%02X (%u)",
              trackIdx, eventTick, statusByte, data1_from_running, data1_from_running);
    } else {
        // It's a new status byte
        statusByte = firstByte;
        // update running status *only* if it's a Channel Voice/Mode message (0x8n to 0xEn)
        if (statusByte >= 0x80 && statusByte <= 0xEF) {
            if(track.lastStatusByte != statusByte) {
                 _log(MidiLogLevel::DEBUG, "T%d @ Tick %llu: New Status Byte 0x%02X (Updating lastStatusByte)",
                      trackIdx, eventTick, statusByte);
                 track.lastStatusByte = statusByte;
            } else {
                 // Repeated status byte. Technically NOT running status, but good to know.
                 _log(MidiLogLevel::DEBUG, "T%d @ Tick %llu: Repeated Status Byte 0x%02X (lastStatusByte unchanged)",
                      trackIdx, eventTick, statusByte);
            }
        } else {
            // Meta (0xFF) or System Common/Realtime (0xF0-0xF7) don't affect channel running status
             _log(MidiLogLevel::DEBUG, "T%d @ Tick %llu: Meta/SysEx/System Status Byte 0x%02X (lastStatusByte unchanged)",
                  trackIdx, eventTick, statusByte);
            // Standard MIDI Spec: System messages (F0-F7) cancel running status. FF (Meta) does NOT.
            // Let's implement cancellation for F0-F7 for correctness, although it's rare to see running status after these.
            if (statusByte >= 0xF0 && statusByte <= 0xF7) {
                 _log(MidiLogLevel::DEBUG, "T%d @ Tick %llu: System Message 0x%02X -> Cancelling Running Status.", trackIdx, eventTick, statusByte);
                 track.lastStatusByte = 0; // Cancel running status
            }
        }
        runningStatus = false;
    }

    // Process based on status byte type
    if (statusByte == META_EVENT) { // 0xFF
        _log(MidiLogLevel::DEBUG, "T%d @ Tick %llu: Handling Meta Event (0xFF)", trackIdx, eventTick);
        _handleMetaEvent(trackIdx, track.currentOffset);
    } else if (statusByte == SYSEX_START || statusByte == SYSEX_END) { // 0xF0, 0xF7
         _log(MidiLogLevel::DEBUG, "T%d @ Tick %llu: Handling SysEx Event (0x%02X)", trackIdx, eventTick, statusByte);
         _handleSysexEvent(trackIdx, statusByte, track.currentOffset);
    } else if (statusByte >= 0x80 && statusByte <= 0xEF) { // Channel Voice/Mode Messages (8n, 9n, An, Bn, Cn, Dn, En)
        _log(MidiLogLevel::DEBUG, "T%d @ Tick %llu: Handling MIDI Event (0x%02X), Running Status = %s",
             trackIdx, eventTick, statusByte, runningStatus ? "true" : "false");
        _handleMidiEvent(trackIdx, statusByte, track.currentOffset, runningStatus, data1_from_running);
    } else if (statusByte >= 0xF1 && statusByte <= 0xFE) { // Other System Common / Realtime (excl F0, F7, FF)
         _log(MidiLogLevel::WARN, "T%d @ Tick %llu (Offset %u): Ignoring System message 0x%02X (not handled).",
              trackIdx, eventTick, eventStartOffset, statusByte );
         // These messages typically don't have data bytes or lengths defined in the MTrk chunk in the same way.
         // Standard practice is often to ignore them in simple file players. If needed, add specific handlers.
         // We already read the status byte, so just proceed to read the next delta time.
         track.lastStatusByte = 0; // Cancel running status as per spec.
    } else {
         // This case should ideally not be reached if all ranges are covered.
         _log(MidiLogLevel::WARN, "T%d @ Tick %llu (Offset %u): Ignoring unknown/unhandled status byte 0x%02X",
              trackIdx, eventTick, eventStartOffset, statusByte );
    }


    // If the track hasn't ended BY THE HANDLER ABOVE, read the delta-time for its *next* event
    if (!track.endOfTrackReached) {
        uint32_t nextDeltaOffset = track.currentOffset; // Log offset before reading delta
        uint32_t nextDelta = _readVariableLengthQuantity(track.currentOffset);
        if (!_midiFile) return; // Check if VLQ read failed

        uint64_t nextTick = eventTick + nextDelta; // Schedule relative to the current event's tick
        _log(MidiLogLevel::DEBUG, "T%d @ Tick %llu (Read delta at offset %u): Read next delta %u -> Next event scheduled for Tick %llu (Offset after delta: %u)",
             trackIdx, eventTick, nextDeltaOffset, nextDelta, nextTick, track.currentOffset);
        track.nextEventTick = nextTick; // Schedule the *next* event
    } else {
         _log(MidiLogLevel::DEBUG, "T%d @ Tick %llu: Track ended, not reading next delta.", trackIdx, eventTick);
    }
}

// --- Event Handlers ---

// Handles Channel Voice messages (0x80-0xEF)
void ESP32MidiPlayer::_handleMidiEvent(uint8_t trackIndex, uint8_t statusByte, uint32_t& trackOffset, bool runningStatusUsed, uint8_t data1_val) {
    uint8_t command = statusByte & 0xF0;
    uint8_t channel = statusByte & 0x0F;
    uint8_t data1 = data1_val; // Use value passed if running status was used
    uint8_t data2 = 0;
    uint32_t offsetBeforeDataRead = trackOffset; // For logging

    _log(MidiLogLevel::DEBUG, "_handleMidiEvent T%d: Status=0x%02X, Ch=%u, Cmd=0x%02X, Running=%s, Offset=%u",
         trackIndex, statusByte, channel, command, runningStatusUsed ? "true" : "false", trackOffset);


    // Determine how many data bytes to read based on the command
    // and whether running status already provided data1
    switch (command) {
        case 0x80: // Note Off (2 bytes total)
        case 0x90: // Note On (2 bytes total)
        case 0xA0: // Polyphonic Key Pressure (Aftertouch) (2 bytes total)
        case 0xB0: // Control Change (2 bytes total)
        case 0xE0: // Pitch Bend Change (2 bytes total)
            if (!runningStatusUsed) {
                data1 = _readUint8(trackOffset);
                if (!_midiFile) return; // Check read status
                 _log(MidiLogLevel::DEBUG, "_handleMidiEvent T%d: Read Data1 = 0x%02X (%u)", trackIndex, data1, data1);
            } else {
                 // data1 was already set from data1_val passed in
                 _log(MidiLogLevel::DEBUG, "_handleMidiEvent T%d: Using Data1 = 0x%02X (%u) from running status", trackIndex, data1, data1);
            }
            // Always read data2 for these 2-byte messages
            data2 = _readUint8(trackOffset);
            if (!_midiFile) return; // Check read status
            _log(MidiLogLevel::DEBUG, "_handleMidiEvent T%d: Read Data2 = 0x%02X (%u)", trackIndex, data2, data2);
            break;

        case 0xC0: // Program Change (1 byte total)
        case 0xD0: // Channel Pressure (Aftertouch) (1 byte total)
            if (!runningStatusUsed) {
                data1 = _readUint8(trackOffset); // Read data1 if not already read via running status
                if (!_midiFile) return; // Check read status
                _log(MidiLogLevel::DEBUG, "_handleMidiEvent T%d: Read Data1 = 0x%02X (%u)", trackIndex, data1, data1);
            } else {
                 // data1 was already set from data1_val passed in
                 _log(MidiLogLevel::DEBUG, "_handleMidiEvent T%d: Using Data1 = 0x%02X (%u) from running status", trackIndex, data1, data1);
            }
            // data2 is not used for these commands (remains 0)
            break;
        default:
            // This should theoretically not be reached if _processNextEvent filters correctly
            _log(MidiLogLevel::WARN, "_handleMidiEvent T%d: Unexpected MIDI command 0x%02X in status 0x%02X", trackIndex, command, statusByte);
            return; // Don't try to process further or call callbacks
    }

     _log(MidiLogLevel::DEBUG, "_handleMidiEvent T%d: Final Data Bytes: Data1=0x%02X (%u), Data2=0x%02X (%u). Offset after read: %u",
          trackIndex, data1, data1, data2, data2, trackOffset);


    // --- Call appropriate callback (if registered) ---
    switch (command) {
        case 0x80: // Note Off
             _log(MidiLogLevel::DEBUG, "CALL T%d: NoteOff Ch=%u Note=%u Vel=%u", trackIndex, channel + 1, data1, data2);
            if (_noteOffCallback) _noteOffCallback(channel, data1, data2);
            break;
        case 0x90: // Note On
            if (data2 == 0) { // Velocity 0 is Note Off
                 _log(MidiLogLevel::DEBUG, "CALL T%d: NoteOff (Vel 0) Ch=%u Note=%u Vel=%u", trackIndex, channel + 1, data1, data2);
                if (_noteOffCallback) _noteOffCallback(channel, data1, 0);
            } else {
                 _log(MidiLogLevel::DEBUG, "CALL T%d: NoteOn Ch=%u Note=%u Vel=%u", trackIndex, channel + 1, data1, data2);
                if (_noteOnCallback) _noteOnCallback(channel, data1, data2);
            }
            break;
        case 0xA0: // Polyphonic Key Pressure
             _log(MidiLogLevel::DEBUG, "CALL T%d: PolyPressure Ch=%u Note=%u Pressure=%u", trackIndex, channel + 1, data1, data2);
            // if (_polyPressureCallback) _polyPressureCallback(channel, data1, data2); // Example if you add this
            break;
        case 0xB0: // Control Change
             _log(MidiLogLevel::DEBUG, "CALL T%d: ControlChange Ch=%u CC=%u Val=%u", trackIndex, channel + 1, data1, data2);
            if (_controlChangeCallback) _controlChangeCallback(channel, data1, data2);
            break;
        case 0xC0: // Program Change
             _log(MidiLogLevel::DEBUG, "CALL T%d: ProgramChange Ch=%u Prog=%u", trackIndex, channel + 1, data1);
            if (_programChangeCallback) _programChangeCallback(channel, data1);
            break;
        case 0xD0: // Channel Pressure
            _log(MidiLogLevel::DEBUG, "CALL T%d: ChannelPressure Ch=%u Pressure=%u", trackIndex, channel + 1, data1);
            // if (_channelPressureCallback) _channelPressureCallback(channel, data1); // Example
            break;
        case 0xE0: // Pitch Bend
            {
                // Combine LSB (data1) and MSB (data2) - Both are 7-bit
                int16_t bendValue = (((int16_t)data2 & 0x7F) << 7) | (data1 & 0x7F);
                // MIDI pitch bend is 0 to 16383, centered at 8192. Convert to -8192 to +8191
                bendValue -= 8192;
                 _log(MidiLogLevel::DEBUG, "CALL T%d: PitchBend Ch=%u Val=%d (Raw LSB=0x%02X MSB=0x%02X Combined=%u)", trackIndex, channel + 1, bendValue, data1, data2, bendValue+8192);
                if (_pitchBendCallback) _pitchBendCallback(channel, bendValue);
            }
            break;
    }
}


void ESP32MidiPlayer::_handleMetaEvent(uint8_t trackIndex, uint32_t& trackOffset) {
    uint8_t metaType = _readUint8(trackOffset);
     if (!_midiFile) return;
    uint32_t length = _readVariableLengthQuantity(trackOffset);
     if (!_midiFile) return;

     _log(MidiLogLevel::DEBUG, "T%d Meta Event: Type 0x%02X, Len %u at offset %u", trackIndex, metaType, length, trackOffset - 1 - _getVlqLength(length)); // Adjust offset for log

    uint32_t dataOffset = trackOffset; // Remember where data starts

    switch (metaType) {
        case META_END_OF_TRACK: // 0x2F
            if (!_tracks[trackIndex].endOfTrackReached) {
                _tracks[trackIndex].endOfTrackReached = true;
                _finishedTracks++;
                 _log(MidiLogLevel::INFO, "Track %u reached EndOfTrack (Total finished: %u/%u)", trackIndex, _finishedTracks, _trackCount);
                if (_endOfTrackCallback) {
                    _endOfTrackCallback(trackIndex);
                }
            } else {
                 _log(MidiLogLevel::WARN, "Track %u encountered multiple EndOfTrack events.", trackIndex);
            }
             // EOT should have length 0, but we still need to "consume" the length bytes read by VLQ earlier.
             // No actual data bytes to read here. The offset is already correct.
             // However, MIDI files *can* technically have non-zero length EOTs. Skip data if present.
             if (length > 0) {
                 _log(MidiLogLevel::WARN, "Track %u EndOfTrack meta event has non-zero length %u. Skipping data.", trackIndex, length);
                 trackOffset += length; // Skip declared length
             }
            break;

        case META_TEMPO: // 0x51 Tempo Setting (Microseconds per Quarter Note)
            if (length == 3) {
                uint8_t buffer[3];
                if (_readBytes(trackOffset, buffer, 3) == 3) {
                     uint32_t newTempo = ((uint32_t)buffer[0] << 16) | ((uint32_t)buffer[1] << 8) | buffer[2];
                     trackOffset += 3;
                     // Basic sanity check for tempo
                     if (newTempo == 0) {
                         _log(MidiLogLevel::WARN, "Track %u requested Tempo of 0 us/qn (invalid). Ignoring change.", trackIndex);
                     } else {
                         _microsecondsPerQuarterNote = newTempo;
                          double bpm = 60000000.0 / _microsecondsPerQuarterNote;
                         _log(MidiLogLevel::DEBUG, "Tempo changed to %u us/qn (%.2f BPM)", _microsecondsPerQuarterNote, bpm);
                         if (_tempoChangeCallback) {
                             _tempoChangeCallback(_microsecondsPerQuarterNote);
                         }
                         _tempoWarningLogged = false; // Reset warning flag if tempo becomes valid again
                     }
                } else {
                     _log(MidiLogLevel::ERROR, "Error reading Tempo data for track %u", trackIndex);
                     trackOffset = dataOffset + length; // Attempt to skip to end of event
                     if (!_midiFile) return; // readBytes might have stopped
                }
            } else {
                 _log(MidiLogLevel::WARN, "Invalid Tempo meta event length %u (expected 3) on track %u. Skipping.", length, trackIndex);
                 trackOffset += length; // Skip data
            }
            break;

        case META_TIME_SIGNATURE: // 0x58
            if (length == 4) {
                uint8_t buffer[4];
                 if (_readBytes(trackOffset, buffer, 4) == 4) {
                    trackOffset += 4;
                     uint8_t numerator = buffer[0];
                     uint8_t denominator_pow2 = buffer[1]; // Denominator is 2^denominator_pow
                     uint8_t clocks_per_metronome = buffer[2]; // MIDI clocks per metronome click
                     uint8_t num_32nd_notes_per_beat = buffer[3]; // Number of 32nd notes per MIDI quarter note (usually 8)

                     // Basic validation
                     if (numerator == 0) {
                         _log(MidiLogLevel::WARN, "Track %u Time Signature has zero numerator. Using 4/4.", trackIndex);
                         numerator = 4;
                         denominator_pow2 = 2; // 2^2 = 4
                     }

                     uint16_t denominator = (1 << denominator_pow2);
                      _log(MidiLogLevel::DEBUG, "Time Signature: %u/%u, Clocks/Met: %u, 32nds/QN: %u", numerator, denominator, clocks_per_metronome, num_32nd_notes_per_beat);

                     if (_timeSignatureCallback) {
                         // Pass the raw denominator power value as some synths might use it directly
                         _timeSignatureCallback(numerator, denominator_pow2, clocks_per_metronome, num_32nd_notes_per_beat);
                     }
                 } else {
                    _log(MidiLogLevel::ERROR, "Error reading Time Signature data for track %u", trackIndex);
                     trackOffset = dataOffset + length; // Attempt to skip
                      if (!_midiFile) return;
                 }
            } else {
                _log(MidiLogLevel::WARN, "Invalid Time Signature meta event length %u (expected 4) on track %u. Skipping.", length, trackIndex);
                trackOffset += length; // Skip data
            }
            break;

        // Add cases for other common meta events if needed (Text, Copyright, Track Name, etc.)
         case META_TRACK_NAME: // 0x03 Sequence/Track Name
            {
                char nameBuffer[length + 1]; // +1 for null terminator
                if (length > 0) {
                    if (_readBytes(trackOffset, (uint8_t*)nameBuffer, length) == length) {
                        nameBuffer[length] = '\0'; // Null terminate
                        _log(MidiLogLevel::INFO, "Track %u Name: \"%s\"", trackIndex, nameBuffer);
                        // Optional: Add a callback for track name if needed
                        // if (_trackNameCallback) _trackNameCallback(trackIndex, nameBuffer);
                    } else {
                        _log(MidiLogLevel::WARN, "Could not read Track Name data for track %u", trackIndex);
                         if (!_midiFile) return;
                    }
                } else {
                     _log(MidiLogLevel::DEBUG, "Track %u has empty Track Name event.", trackIndex);
                }
                trackOffset += length; // Advance offset past data (or by 0 if length was 0)
            }
            break;
        // Example: Add case 0x01 (Text), 0x02 (Copyright), etc. similarly if desired

        default:
            // Unknown or unhandled meta event, just skip over its data
             _log(MidiLogLevel::DEBUG, "Skipping unhandled Meta Event Type 0x%02X, Length %u on track %u", metaType, length, trackIndex);
            trackOffset += length;
            // Sanity check seek position
             if (trackOffset > _midiFile.size()) {
                  _log(MidiLogLevel::ERROR, "Error skipping meta event 0x%02X: Offset %u exceeds file size %u.", metaType, trackOffset, _midiFile.size());
                  stop();
                  return;
             }
             if (_midiFile && !_midiFile.seek(trackOffset)) { // Ensure file pointer is updated
                  _log(MidiLogLevel::ERROR, "Failed to seek past meta event 0x%02X data.", metaType);
                  stop();
                  return;
             }
            break;
    }

     // Ensure offset is correct even if handler didn't read all bytes (e.g., due to error)
     // This is slightly risky if the handler logic is wrong, but helps prevent getting stuck.
     if (trackOffset < dataOffset + length) {
          _log(MidiLogLevel::WARN,"Meta event handler for type 0x%02X did not consume all %u bytes. Forcing skip.", metaType, length);
          trackOffset = dataOffset + length;
     }
}

void ESP32MidiPlayer::_handleSysexEvent(uint8_t trackIndex, uint8_t type, uint32_t& trackOffset) {
    // SysEx events (F0) and "escape" sequences (F7) have a VLQ length field
    uint32_t length = _readVariableLengthQuantity(trackOffset);
     if (!_midiFile) return;

     _log(MidiLogLevel::DEBUG, "SysEx event (Type 0x%02X), Length %u on track %u - Skipping data.", type, length, trackIndex);

    // Optional: Read data and provide via callback if needed
    // uint8_t* sysExData = new uint8_t[length];
    // if (sysExData && _readBytes(trackOffset, sysExData, length) == length) {
    //    // Add a SysEx callback if you need to handle this data
    //    // if (_sysExCallback) _sysExCallback(type, sysExData, length);
    //    delete[] sysExData;
    // } else {
    //    _log(MidiLogLevel::ERROR, "Error reading SysEx data or allocating memory for track %u", trackIndex);
    //    delete[] sysExData; // Safe to delete nullptr
    //    // Consider stopping playback if SysEx data is critical and read failed
    //    trackOffset = // try to determine end of event based on initial offset + VLQ length + data length? Difficult.
    //    stop(); // Safer to stop if read fails mid-event
    //    return;
    // }

    trackOffset += length; // Skip SysEx data

     // Sanity check seek position
     if (trackOffset > _midiFile.size()) {
          _log(MidiLogLevel::ERROR, "Error skipping SysEx event 0x%02X: Offset %u exceeds file size %u.", type, trackOffset, _midiFile.size());
          stop();
          return;
     }
     if (_midiFile && !_midiFile.seek(trackOffset)) { // Ensure file pointer is updated
          _log(MidiLogLevel::ERROR, "Failed to seek past SysEx event 0x%02X data.", type);
          stop();
          return;
     }
     // SysEx cancels running status
     _tracks[trackIndex].lastStatusByte = 0;
}


// Internal logging helper (modified for levels)
void ESP32MidiPlayer::_log(MidiLogLevel level, const char* format, ...) {
    // 1. Check if a callback is registered
    // 2. Check if the message's level is severe enough to be logged
    if (_logCallback && level <= _currentLogLevel && _currentLogLevel != MidiLogLevel::NONE) {
        // Increased buffer size slightly for potential long filenames or debug messages
        char buffer[256];
        va_list args;
        va_start(args, format);
        vsnprintf(buffer, sizeof(buffer), format, args);
        va_end(args);

        // Ensure null termination even if message was truncated
        buffer[sizeof(buffer) - 1] = '\0';

        // Call the user's callback, passing the level and the formatted message
        _logCallback(level, buffer);
    }
}