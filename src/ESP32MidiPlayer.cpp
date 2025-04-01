#include "ESP32MidiPlayer.h"
#include <stdarg.h>
#include <limits>
#include <queue>

ESP32MidiPlayer::ESP32MidiPlayer() {}

ESP32MidiPlayer::~ESP32MidiPlayer() {
    closeFile();
}

void ESP32MidiPlayer::setLogger(MidiLogLevel level, MidiLogCallback callback) {
    _logLevel = level;
    _logCallback = callback;
    // Note: Cannot call the const _log from a non-const function directly if it modifies members,
    // but okay here as setLogger modifies logging config, not playback state.
    // If _log needed to be non-const, calculateMsPerTick couldn't call it.
    _log(MIDI_LOG_INFO, "Logger initialized with level: %d", level);
}

// *** ADDED const qualifier here ***
void ESP32MidiPlayer::_log(MidiLogLevel level, const char* format, ...) const {
    // Check against the member _logLevel (reading is fine in const function)
    if (level <= _logLevel && level > MIDI_LOG_NONE) {
        char logBuffer[256];
        va_list args;
        va_start(args, format);
        vsnprintf(logBuffer, sizeof(logBuffer), format, args);
        va_end(args);

        // Using member _logCallback (reading is fine in const function)
        if (_logCallback) {
            // Calling external callback doesn't violate const-ness of 'this' object
            _logCallback(level, logBuffer);
        } else {
            const char* levelStr;
            switch (level) {
                case MIDI_LOG_FATAL:   levelStr = "FATAL"; break;
                case MIDI_LOG_ERROR:   levelStr = "ERROR"; break;
                case MIDI_LOG_WARN:    levelStr = "WARN";  break;
                case MIDI_LOG_INFO:    levelStr = "INFO";  break;
                case MIDI_LOG_DEBUG:   levelStr = "DEBUG"; break;
                case MIDI_LOG_VERBOSE: levelStr = "VERBOSE"; break;
                default:               levelStr = "UNKNOWN"; break;
            }
            // Calling external Serial.printf doesn't violate const-ness
            Serial.printf("[MIDI %s] %s\n", levelStr, logBuffer);
        }
    }
}

bool ESP32MidiPlayer::begin() {
    _log(MIDI_LOG_DEBUG, "Attempting to mount LittleFS");
    if (!LittleFS.begin(true)) {
        _log(MIDI_LOG_FATAL, "LittleFS mount failed");
        _currentState = MidiPlayerState::ERROR;
        return false;
    }
    _log(MIDI_LOG_INFO, "LittleFS mounted successfully");
    return true;
}

void ESP32MidiPlayer::closeFile() {
    if (_midiFile) {
        _log(MIDI_LOG_DEBUG, "Closing MIDI file: %s", _currentFilename ? _currentFilename : "unknown");
        _midiFile.close();
        _log(MIDI_LOG_INFO, "MIDI file closed successfully");
    }
}

void ESP32MidiPlayer::resetPlayback() {
    _log(MIDI_LOG_DEBUG, "Resetting playback state");
    _currentTick = 0;
    _lastTickMillis = 0;
    _eventProcessed = false;
    _songStartMillis = 0;
    _pauseStartMillis = 0;
    _activeTracks = _trackStates.size();

    while (!_eventQueue.empty()) {
        _eventQueue.pop();
    }
    _log(MIDI_LOG_VERBOSE, "Event queue cleared");

    for (size_t i = 0; i < _trackStates.size(); i++) {
        TrackState& track = _trackStates[i];
        track.currentOffset = track.trackChunkStart;
        track.nextEventTick = 0;
        track.runningStatus = 0;
        track.finished = false;
        track.bufferFill = 0;
        track.bufferPos = 0;
        _log(MIDI_LOG_VERBOSE, "Track %u reset: offset=%u", i, track.currentOffset);
    }
    _log(MIDI_LOG_INFO, "Playback reset completed, initial active tracks: %u", _activeTracks);
}

bool ESP32MidiPlayer::loadFile(const char* filename) {
    _log(MIDI_LOG_INFO, "Loading MIDI file: %s", filename);
    stop();

    _currentFilename = filename;
    _midiFile = LittleFS.open(filename, "r");
    if (!_midiFile) {
        _log(MIDI_LOG_ERROR, "Failed to open file: %s", filename);
        _currentState = MidiPlayerState::ERROR;
        _currentFilename = nullptr;
        return false;
    }

    _log(MIDI_LOG_DEBUG, "File opened: %s, size=%u bytes", filename, _midiFile.size());

    if (_midiFile.size() < 14) {
        _log(MIDI_LOG_FATAL, "File too small for MIDI header: %u bytes", _midiFile.size());
        closeFile();
        _currentState = MidiPlayerState::ERROR;
        _currentFilename = nullptr;
        return false;
    }

    char chunkId[5] = {0};
    if (_midiFile.readBytes(chunkId, 4) != 4 || strcmp(chunkId, "MThd") != 0) {
        _log(MIDI_LOG_FATAL, "Invalid MIDI header: %s", chunkId);
        closeFile();
        _currentState = MidiPlayerState::ERROR;
        _currentFilename = nullptr;
        return false;
    }

    uint32_t headerLength;
    if (_midiFile.readBytes((char*)&headerLength, 4) != 4) {
        _log(MIDI_LOG_ERROR, "Failed to read header length");
        closeFile();
        return false;
    }
    headerLength = __builtin_bswap32(headerLength);

    if (headerLength < 6) {
        _log(MIDI_LOG_ERROR, "Header length too short: %u", headerLength);
        return false;
    }

    uint16_t midiFormat, numTracks, timeDivision;
    if (_midiFile.readBytes((char*)&midiFormat, 2) != 2 ||
        _midiFile.readBytes((char*)&numTracks, 2) != 2 ||
        _midiFile.readBytes((char*)&timeDivision, 2) != 2) {
        _log(MIDI_LOG_ERROR, "Failed to read MIDI header data");
        closeFile();
        return false;
    }
    midiFormat = __builtin_bswap16(midiFormat);
    numTracks = __builtin_bswap16(numTracks);
    timeDivision = __builtin_bswap16(timeDivision);

    if (headerLength > 6) {
        _log(MIDI_LOG_DEBUG, "Skipping extra header bytes: %u", headerLength - 6);
        _midiFile.seek(headerLength - 6, SeekCur);
    }

    _log(MIDI_LOG_INFO, "MIDI header parsed - Format=%u, Tracks=%u, Division=%u",
         midiFormat, numTracks, timeDivision);

    if (timeDivision & 0x8000) {
        _log(MIDI_LOG_FATAL, "SMPTE time division not supported: %u", timeDivision);
        closeFile();
        _currentState = MidiPlayerState::ERROR;
        _currentFilename = nullptr;
        return false;
    }
    _ticksPerQuarterNote = (timeDivision > 0) ? timeDivision : 96;

    _trackStates.clear();
    _trackStates.reserve(numTracks);

    for (uint16_t i = 0; i < numTracks; ++i) {
        bool mtrkFound = false;
        while (_midiFile.available() >= 8) {
            size_t currentPos = _midiFile.position();
            if (_midiFile.readBytes(chunkId, 4) != 4) {
                _log(MIDI_LOG_ERROR, "Failed to read chunk ID for track %u", i);
                break;
            }

            if (strcmp(chunkId, "MTrk") == 0) {
                mtrkFound = true;
                break;
            } else {
                uint32_t chunkLength;
                if (_midiFile.readBytes((char*)&chunkLength, 4) != 4) break;
                chunkLength = __builtin_bswap32(chunkLength);
                _log(MIDI_LOG_DEBUG, "Skipping chunk '%s' at offset %u, length=%u",
                     chunkId, currentPos, chunkLength);
                if (!_midiFile.seek(chunkLength, SeekCur)) break;
            }
        }

        if (!mtrkFound) {
            _log(MIDI_LOG_WARN, "No 'MTrk' found for track %u of %u", i, numTracks);
            break;
        }

        uint32_t trackLength;
        if (_midiFile.readBytes((char*)&trackLength, 4) != 4) {
            _log(MIDI_LOG_ERROR, "Failed to read length for track %u", i);
            break;
        }
        trackLength = __builtin_bswap32(trackLength);

        TrackState ts;
        ts.trackChunkStart = _midiFile.position();
        ts.trackChunkEnd = ts.trackChunkStart + trackLength;
        ts.currentOffset = ts.trackChunkStart;
        _trackStates.push_back(ts);

        _log(MIDI_LOG_DEBUG, "Track %u: start=%u, length=%u, end=%u",
             i, ts.trackChunkStart, trackLength, ts.trackChunkEnd);

        if (!_midiFile.seek(ts.trackChunkEnd, SeekSet)) {
            _log(MIDI_LOG_WARN, "Seek failed after track %u at %u", i, ts.trackChunkEnd);
            break;
        }
    }

    if (_trackStates.empty()) {
        _log(MIDI_LOG_ERROR, "No valid tracks found in file");
        closeFile();
        _currentState = MidiPlayerState::ERROR;
        _currentFilename = nullptr;
        return false;
    }

    _activeTracks = _trackStates.size();

    if (!initializeTrackStates()) {
        _log(MIDI_LOG_ERROR, "Failed to initialize track states");
        closeFile();
        _currentState = MidiPlayerState::ERROR;
        _currentFilename = nullptr;
        return false;
    }

    _currentState = MidiPlayerState::STOPPED;
    _log(MIDI_LOG_INFO, "File %s loaded successfully, %u tracks initially active", filename, _activeTracks);
    return true;
}

bool ESP32MidiPlayer::initializeTrackStates() {
    if (!_midiFile) {
        _log(MIDI_LOG_ERROR, "No MIDI file open for track initialization");
        return false;
    }

    _log(MIDI_LOG_DEBUG, "Initializing %u track states and populating event queue", _trackStates.size());
    _activeTracks = 0;

    for (size_t i = 0; i < _trackStates.size(); i++) {
        TrackState& track = _trackStates[i];
        track.currentOffset = track.trackChunkStart;
        track.bufferFill = 0;
        track.bufferPos = 0;
        track.runningStatus = 0;
        track.finished = false;

        if (track.currentOffset >= track.trackChunkEnd) {
            _log(MIDI_LOG_WARN, "Track %u empty or invalid: start=%u, end=%u",
                 i, track.trackChunkStart, track.trackChunkEnd);
            track.finished = true;
            continue;
        }

        if (!_midiFile.seek(track.currentOffset, SeekSet)) {
            _log(MIDI_LOG_ERROR, "Seek failed for track %u at %u", i, track.currentOffset);
            return false;
        }

        unsigned long deltaTime = readVariableLengthQuantity(track);
        if (track.finished) {
            _log(MIDI_LOG_DEBUG, "Track %u finished at initial delta time read", i);
            continue;
        }

        track.nextEventTick = deltaTime;

        _eventQueue.push({track.nextEventTick, i});
        _activeTracks++;

        _log(MIDI_LOG_VERBOSE, "Track %u initialized: first event at tick %lu, added to queue", i, track.nextEventTick);
    }
    _log(MIDI_LOG_INFO, "Track states initialized, event queue populated, active tracks: %u", _activeTracks);
    return true;
}

void ESP32MidiPlayer::play() {
    if (_currentState == MidiPlayerState::PLAYING) {
        _log(MIDI_LOG_DEBUG, "Already playing, ignoring play command");
        return;
    }

    if (_trackStates.empty()) {
        _log(MIDI_LOG_ERROR, "No MIDI data loaded for playback");
        _currentState = MidiPlayerState::ERROR;
        return;
    }

    if (!_midiFile) {
        if (!_currentFilename) {
            _log(MIDI_LOG_ERROR, "No filename available to reopen file");
            _currentState = MidiPlayerState::ERROR;
            return;
        }
        _midiFile = LittleFS.open(_currentFilename, "r");
        if (!_midiFile) {
            _log(MIDI_LOG_ERROR, "Failed to reopen file: %s", _currentFilename);
            _currentState = MidiPlayerState::ERROR;
            return;
        }
        _log(MIDI_LOG_DEBUG, "Reopened file %s for playback", _currentFilename);
    }

    resetPlayback();

    if (!initializeTrackStates()) {
        _log(MIDI_LOG_ERROR, "Failed to re-initialize tracks for playback");
        closeFile();
        _currentState = MidiPlayerState::ERROR;
        return;
    }

    if (_eventQueue.empty()) {
        _log(MIDI_LOG_INFO, "No events found in tracks, playback finished immediately.");
        _currentState = MidiPlayerState::FINISHED;
        return;
    }

    _songStartMillis = millis();
    _currentTick = 0;
    _lastTickMillis = _songStartMillis;

    _currentState = MidiPlayerState::PLAYING;
    _currentBPM = 60000000.0f / _microsecondsPerQuarterNote;
    _log(MIDI_LOG_INFO, "Playback started, Initial BPM=%f, ticks/qn=%lu",
         _currentBPM, _ticksPerQuarterNote);
}

void ESP32MidiPlayer::stop() {
    if (_currentState != MidiPlayerState::STOPPED) {
        _log(MIDI_LOG_INFO, "Stopping playback");
        resetPlayback();
        _currentState = MidiPlayerState::STOPPED;
        closeFile();
    }
}

void ESP32MidiPlayer::pause() {
    if (_currentState == MidiPlayerState::PLAYING) {
        _currentState = MidiPlayerState::PAUSED;
        _pauseStartMillis = millis();
        _log(MIDI_LOG_INFO, "Playback paused at tick %lu", _currentTick);
    }
}

void ESP32MidiPlayer::resume() {
    if (_currentState == MidiPlayerState::PAUSED) {
        unsigned long pausedDuration = millis() - _pauseStartMillis;
        _songStartMillis += pausedDuration;
        _lastTickMillis += pausedDuration;
        _currentState = MidiPlayerState::PLAYING;
        _log(MIDI_LOG_INFO, "Playback resumed at tick %lu after %lu ms pause",
             _currentTick, pausedDuration);
    }
}

MidiPlayerState ESP32MidiPlayer::getState() const {
    return _currentState;
}

bool ESP32MidiPlayer::refillTrackBuffer(TrackState& track) {
    if (!_midiFile) {
        _log(MIDI_LOG_ERROR, "No MIDI file handle available for buffer refill");
        return false;
    }

    _log(MIDI_LOG_VERBOSE, "Refilling buffer for track at offset %u", track.currentOffset);

    if (_midiFile.position() != track.currentOffset) {
        if (!_midiFile.seek(track.currentOffset, SeekSet)) {
            _log(MIDI_LOG_ERROR, "Failed to seek to offset %u", track.currentOffset);
            track.finished = true;
            // _activeTracks adjustment handled elsewhere
            return false;
        }
        _log(MIDI_LOG_DEBUG, "Seeked to position %u for buffer refill", track.currentOffset);
    }

    size_t bytesToRead = MIDI_PLAYER_TRACK_BUFFER_SIZE;
    if (track.currentOffset + bytesToRead > track.trackChunkEnd) {
        bytesToRead = track.trackChunkEnd - track.currentOffset;
        _log(MIDI_LOG_DEBUG, "Adjusted bytes to read to %u (track end approaching)", bytesToRead);
    }

    if (bytesToRead == 0) {
        _log(MIDI_LOG_DEBUG, "Reached track end at offset %u", track.currentOffset);
        track.bufferFill = 0;
        track.bufferPos = 0;
        // Don't mark finished here, let parseEvent/readVLQ handle EOF condition
        return false;
    }

    size_t bytesRead = _midiFile.read(track.buffer, bytesToRead);
    track.bufferFill = bytesRead;
    track.bufferPos = 0;
    track.currentOffset += bytesRead; // Update offset based on actual read

    _log(MIDI_LOG_VERBOSE, "Read %u bytes into buffer, new offset=%u", bytesRead, track.currentOffset);

    if (bytesRead < bytesToRead) {
         // Reached EOF during read, or actual read error
         if (track.currentOffset != track.trackChunkEnd) {
              _log(MIDI_LOG_WARN, "Partial read: %u of %u bytes expected, offset=%u. File end likely reached.",
                   bytesRead, bytesToRead, track.currentOffset);
         }
         // Don't mark finished here, let consumer handle lack of data
    }

    return bytesRead > 0; // Indicate if any bytes were read
}

bool ESP32MidiPlayer::readTrackByte(TrackState& track, byte& outByte) {
    if (track.finished) {
        _log(MIDI_LOG_VERBOSE, "Attempted to read from finished track");
        return false;
    }

    if (track.bufferPos >= track.bufferFill) {
        if (!refillTrackBuffer(track)) {
            // Refill failed (likely EOF or read error)
            if (track.currentOffset >= track.trackChunkEnd && track.bufferFill == 0) {
                 _log(MIDI_LOG_DEBUG, "Track completed at offset %u (EOF detected during readTrackByte)", track.currentOffset);
                 // Don't mark finished here, let caller (readVLQ/parseEvent) decide based on context
            } else {
                 _log(MIDI_LOG_ERROR, "Buffer refill failed or returned no data at offset %u", track.currentOffset);
                 // Don't mark finished here, let caller handle parse/read failure
            }
            return false; // Indicate byte could not be read
        }
         // Buffer refilled, fall through to read the byte
    }

    // Check again if buffer has data after potential refill
    if (track.bufferPos >= track.bufferFill) {
         _log(MIDI_LOG_ERROR, "Buffer refill succeeded but buffer is still empty? Offset: %u", track.currentOffset);
         return false; // Should not happen, but guard against it
    }

    outByte = track.buffer[track.bufferPos++];
    _log(MIDI_LOG_VERBOSE, "Read byte 0x%02X from buffer position %u", outByte, track.bufferPos - 1);
    return true; // Byte successfully read
}


unsigned long ESP32MidiPlayer::readVariableLengthQuantity(TrackState& track) {
    unsigned long value = 0;
    byte currentByte;
    int bytesRead = 0;
    const int maxBytes = 4; // Protect against malformed VLQ

    _log(MIDI_LOG_VERBOSE, "Reading VLQ starting at buffer pos %u (file offset ~%u)",
         track.bufferPos, track.currentOffset - track.bufferFill + track.bufferPos);

    do {
        if (bytesRead >= maxBytes) {
            _log(MIDI_LOG_ERROR, "VLQ exceeds maximum %d bytes at offset ~%u. Malformed MIDI?",
                 maxBytes, track.currentOffset - track.bufferFill + track.bufferPos);
            track.finished = true; // Treat as fatal track error
            return 0; // Return dummy value
        }

        if (!readTrackByte(track, currentByte)) {
            _log(MIDI_LOG_WARN, "Failed to read VLQ byte %d at offset ~%u. End of track reached prematurely?",
                 bytesRead + 1, track.currentOffset - track.bufferFill + track.bufferPos);
            // If EOF is expected (e.g., end of track chunk), this might be okay if value is 0
            // but generally indicates an issue or truncated track.
            track.finished = true; // Mark finished if VLQ cannot be completed
            return value; // Return potentially partial value
        }
        value = (value << 7) | (currentByte & 0x7F);
        bytesRead++;
    } while (currentByte & 0x80);

    _log(MIDI_LOG_DEBUG, "Read VLQ: %lu (took %d bytes)", value, bytesRead);
    return value;
}


float ESP32MidiPlayer::calculateMsPerTick() const {
    if (_ticksPerQuarterNote > 0 && _microsecondsPerQuarterNote > 0) {
        return static_cast<float>(_microsecondsPerQuarterNote) / (_ticksPerQuarterNote * 1000.0f);
    } else {
        // *** This call is now valid because _log is const ***
        _log(MIDI_LOG_WARN, "Invalid timing division Ticks:%lu usPerQN:%f, using default 120 BPM",
            _ticksPerQuarterNote, _microsecondsPerQuarterNote);
        return 5.208333f; // Default 120 BPM, 96 TPQN
    }
}

bool ESP32MidiPlayer::parseEvent(TrackState& track, MidiEventData& eventData) {
    if (track.finished) {
        _log(MIDI_LOG_VERBOSE, "Cannot parse event: track already finished");
        return false;
    }

    byte statusByte;
    size_t eventStartOffset = track.currentOffset - track.bufferFill + track.bufferPos;
    _log(MIDI_LOG_VERBOSE, "Parsing event at file offset ~%u", eventStartOffset);

    // Ensure there's at least one byte available for status/running status
    if (track.bufferPos >= track.bufferFill) {
        if (!refillTrackBuffer(track) || track.bufferPos >= track.bufferFill) {
             _log(MIDI_LOG_WARN, "Failed to read first byte of event at offset ~%u. End of track?", eventStartOffset);
             track.finished = true; // Assume end of track if cannot read event byte
             return false;
        }
    }

    byte peekByte = track.buffer[track.bufferPos];
    if (peekByte < 0x80) { // Running status
        if (track.runningStatus == 0) {
            _log(MIDI_LOG_ERROR, "Invalid data byte 0x%02X without running status at offset ~%u",
                 peekByte, eventStartOffset);
            track.finished = true;
            return false;
        }
        statusByte = track.runningStatus;
        _log(MIDI_LOG_VERBOSE, "Using running status 0x%02X", statusByte);
    } else { // New status byte
        if (!readTrackByte(track, statusByte)) {
             _log(MIDI_LOG_ERROR, "Failed to read status byte at offset ~%u", eventStartOffset);
             track.finished = true; // Cannot proceed without status
             return false;
        }
        if (statusByte >= 0x80 && statusByte <= 0xEF) { // Voice/Mode messages set running status
            track.runningStatus = statusByte;
            _log(MIDI_LOG_VERBOSE, "Set running status to 0x%02X", statusByte);
        } else {
             track.runningStatus = 0; // System messages clear running status
             _log(MIDI_LOG_VERBOSE, "System message 0x%02X received, cleared running status", statusByte);
        }
    }

    eventData.status = statusByte;
    eventData.data1 = 0;
    eventData.data2 = 0;

    byte eventType = statusByte & 0xF0;

    switch (eventType) {
        case 0x80: // Note Off
        case 0x90: // Note On
        case 0xA0: // Polyphonic Key Pressure
        case 0xB0: // Control Change
        case 0xE0: // Pitch Bend
            if (!readTrackByte(track, eventData.data1) || !readTrackByte(track, eventData.data2)) {
                _log(MIDI_LOG_ERROR, "Failed to read 2 data bytes for 0x%02X at offset ~%u",
                     statusByte, eventStartOffset);
                track.finished = true; return false;
            }
            _log(MIDI_LOG_DEBUG, "Parsed event 0x%02X: Ch=%u D1=%u D2=%u",
                 statusByte, statusByte & 0x0F, eventData.data1, eventData.data2);
            break;

        case 0xC0: // Program Change
        case 0xD0: // Channel Pressure
            if (!readTrackByte(track, eventData.data1)) {
                _log(MIDI_LOG_ERROR, "Failed to read 1 data byte for 0x%02X at offset ~%u",
                     statusByte, eventStartOffset);
                track.finished = true; return false;
            }
            _log(MIDI_LOG_DEBUG, "Parsed event 0x%02X: Ch=%u D1=%u",
                 statusByte, statusByte & 0x0F, eventData.data1);
            break;

        case 0xF0: // System messages
            switch (statusByte) {
                case 0xF0: { // System Exclusive Start
                    byte sysexByte;
                    unsigned long sysexLen = 0;
                    _log(MIDI_LOG_DEBUG, "Parsing Sysex event at offset ~%u", eventStartOffset);
                    // Skip Sysex data until F7
                    while (true) {
                        if (!readTrackByte(track, sysexByte)) {
                            _log(MIDI_LOG_ERROR, "Sysex event truncated (missing F7) at offset ~%u", eventStartOffset);
                            track.finished = true; return false;
                        }
                        sysexLen++;
                        if (sysexByte == 0xF7) break;
                    }
                    eventData.status = 0; // Mark as non-channel event
                    _log(MIDI_LOG_DEBUG, "Completed Sysex event parsing (skipped %lu data bytes)", sysexLen -1);
                    break;
                }
                case 0xF7: // Could be Sysex end or standalone Authorization message (both often skipped)
                     _log(MIDI_LOG_DEBUG, "Standalone 0xF7 encountered at offset ~%u (likely EOX or auth, skipping)", eventStartOffset);
                     // Some formats might require reading a length here, but typically not for simple EOX.
                     // Assuming it's just EOX marking end of F0 block.
                     eventData.status = 0;
                     break;

                case 0xFF: { // Meta Event
                    byte metaType;
                    if (!readTrackByte(track, metaType)) {
                        _log(MIDI_LOG_ERROR, "Failed to read meta type at offset ~%u", eventStartOffset);
                        track.finished = true; return false;
                    }
                    unsigned long metaLen = readVariableLengthQuantity(track);
                    if (track.finished) { // readVLQ failed
                        _log(MIDI_LOG_ERROR, "Track ended during meta length read at ~%u", eventStartOffset);
                        return false; // Already marked finished by readVLQ
                    }

                    eventData.data1 = metaType; // Store meta type for potential external handling?

                    size_t metaDataStartOffset = track.currentOffset - track.bufferFill + track.bufferPos;

                    if (metaType == 0x51 && metaLen == 3) { // Tempo Change
                        byte b1, b2, b3;
                        if (!readTrackByte(track, b1) || !readTrackByte(track, b2) || !readTrackByte(track, b3)) {
                            _log(MIDI_LOG_ERROR, "Failed to read 3 tempo data bytes at offset ~%u", metaDataStartOffset);
                            track.finished = true; return false;
                        }
                        _microsecondsPerQuarterNote = (static_cast<unsigned long>(b1) << 16) |
                                                      (static_cast<unsigned long>(b2) << 8) | b3;
                        _currentBPM = 60000000.0f / _microsecondsPerQuarterNote;
                        _log(MIDI_LOG_INFO, "Tempo meta event parsed: %f BPM (%lu us/qn) at tick %lu",
                             _currentBPM, (unsigned long)_microsecondsPerQuarterNote, _currentTick); // Log with current tick context

                    } else if (metaType == 0x2F) { // End of Track
                        track.finished = true; // Mark track finished
                        _log(MIDI_LOG_DEBUG, "EndOfTrack meta event parsed at tick %lu, offset ~%u", _currentTick, eventStartOffset);
                        // Skip any potential data in the EOT event (should be length 0 usually)
                        for (unsigned long k = 0; k < metaLen; ++k) { byte dummy; if (!readTrackByte(track, dummy)) { _log(MIDI_LOG_WARN,"Read failure skipping EOT data"); break;} }

                    } else { // Other Meta Events (Sequence Name, Text, etc.) - Skip payload
                        _log(MIDI_LOG_DEBUG, "Skipping meta event type 0x%02X, length=%lu at offset ~%u",
                             metaType, metaLen, eventStartOffset);
                        bool skipOk = true;
                        for (unsigned long k = 0; k < metaLen; ++k) {
                            byte dummy;
                            if (!readTrackByte(track, dummy)) {
                                _log(MIDI_LOG_ERROR, "Failed to skip %lu bytes of meta payload (type 0x%02X) at offset ~%u",
                                     metaLen, metaType, metaDataStartOffset);
                                track.finished = true; skipOk = false; break;
                            }
                        }
                        if (!skipOk) return false;
                    }
                    eventData.status = 0; // Mark all meta events as non-channel events
                    break; // End of 0xFF case
                }
                // Add cases for F1, F2, F3 etc. if needed, otherwise default handles them
                default:
                    _log(MIDI_LOG_WARN, "Unhandled system message 0x%02X at offset ~%u. Skipping.",
                         statusByte, eventStartOffset);
                    // Attempt to skip based on expected lengths (difficult without full spec)
                    // For now, just mark as non-channel event and hope for the best
                    eventData.status = 0;
                    break;
            }
            break; // End of 0xF0 case

        default: // Should not happen if status byte handling is correct
            _log(MIDI_LOG_FATAL, "Unexpected event type 0x%02X (status 0x%02X) at offset ~%u",
                 eventType, statusByte, eventStartOffset);
            track.finished = true;
            return false;
    }

    return true; // Event structure parsed successfully
}


void ESP32MidiPlayer::update() {
    if (_currentState != MidiPlayerState::PLAYING) {
        return;
    }

    if (_eventQueue.empty()) {
        if (_activeTracks > 0) {
             _log(MIDI_LOG_WARN, "Event queue empty but active track count is %u, fixing.", _activeTracks);
             _activeTracks = 0;
        }
        if (_currentState == MidiPlayerState::PLAYING) { // Prevent multiple Finished logs
            _log(MIDI_LOG_INFO, "Event queue empty, ending playback at tick %lu", _currentTick);
            _currentState = MidiPlayerState::FINISHED;
        }
        return;
    }

    unsigned long currentMillis = millis();
    _eventProcessed = false;

    while (!_eventQueue.empty()) {
        EventQueueEntry nextEventInfo = _eventQueue.top();
        unsigned long nextEventTick = nextEventInfo.first;
        size_t trackIndex = nextEventInfo.second;

        unsigned long tickDelta = nextEventTick - _currentTick;
        float currentMsPerTick = calculateMsPerTick();
        unsigned long millisDelta = static_cast<unsigned long>(tickDelta * currentMsPerTick);
        unsigned long targetMillis = _lastTickMillis + millisDelta;

        if (currentMillis >= targetMillis) {
            _eventQueue.pop();

            TrackState& currentTrack = _trackStates[trackIndex];

            // Check if track was somehow finished before processing
            if (currentTrack.finished) {
                 _log(MIDI_LOG_DEBUG, "Skipping event for already finished track %u at tick %lu", trackIndex, nextEventTick);
                 continue; // Get next event from queue
            }

            _currentTick = nextEventTick;
            _lastTickMillis = targetMillis; // Use calculated target time as the new reference

            size_t eventDataOffset = currentTrack.currentOffset - currentTrack.bufferFill + currentTrack.bufferPos;

            // File Position Check/Seek (If necessary - ensure buffer logic matches file state)
            if (!_midiFile || (_midiFile.position() != eventDataOffset)) {
                 _log(MIDI_LOG_VERBOSE, "Seeking for track %u event data. Current pos: %u, Target offset: %u",
                      trackIndex, _midiFile ? _midiFile.position() : 0, eventDataOffset);
                if (!_midiFile || !_midiFile.seek(eventDataOffset, SeekSet)) {
                    _log(MIDI_LOG_ERROR, "Seek failed for track %u event data at offset %u", trackIndex, eventDataOffset);
                    currentTrack.finished = true;
                } else {
                    currentTrack.bufferFill = 0;
                    currentTrack.bufferPos = 0;
                    currentTrack.currentOffset = eventDataOffset;
                    _log(MIDI_LOG_VERBOSE, "Seek successful for track %u, buffer invalidated.", trackIndex);
                }
            }

            if (currentTrack.finished) { // Check again after potential seek failure
                 _log(MIDI_LOG_WARN, "Skipping processing for track %u at tick %lu due to prior error/finish.", trackIndex, _currentTick);
                 continue;
            }

            _log(MIDI_LOG_VERBOSE, "Processing track %u event at tick %lu (target real time: %lu ms)",
                 trackIndex, _currentTick, targetMillis);

            MidiEventData eventData;
            if (!parseEvent(currentTrack, eventData)) {
                _log(MIDI_LOG_ERROR, "Event parsing failed for track %u at tick %lu, offset ~%u. Marking track finished.",
                     trackIndex, _currentTick, eventDataOffset);
                // parseEvent should have set currentTrack.finished = true
                if (!currentTrack.finished) { // Defensive check
                     _log(MIDI_LOG_WARN,"parseEvent returned false but didn't mark track %u finished?", trackIndex);
                     currentTrack.finished = true;
                }
                continue;
            }

            // Check if track finished during parsing (EndOfTrack meta)
            if (currentTrack.finished) {
                 _log(MIDI_LOG_DEBUG, "Track %u finished by event at tick %lu", trackIndex, _currentTick);
                 continue; // Don't read next delta or re-queue
            }

            // Read delta for the *next* event on this track
            unsigned long nextDelta = readVariableLengthQuantity(currentTrack);

            if (currentTrack.finished) { // readVLQ failed or hit EOF
                 _log(MIDI_LOG_DEBUG, "Track %u finished while reading next delta time after tick %lu", trackIndex, _currentTick);
                 // Don't re-queue
            } else {
                // Schedule the next event for this track
                currentTrack.nextEventTick = _currentTick + nextDelta;
                _eventQueue.push({currentTrack.nextEventTick, trackIndex});
                _log(MIDI_LOG_VERBOSE, "Track %u next event scheduled at tick %lu (delta=%lu), re-queued",
                     trackIndex, currentTrack.nextEventTick, nextDelta);
            }

            // Handle the successfully parsed event (if it's a channel message)
            if (eventData.status != 0 && (eventData.status & 0xF0) != 0xF0) {
                _currentEventType = eventData.status & 0xF0;
                _currentChannel = eventData.status & 0x0F;
                _currentData1 = eventData.data1;
                _currentData2 = eventData.data2;
                _eventProcessed = true;
                _log(MIDI_LOG_DEBUG, "Channel Event ready: Tick=%lu Type=0x%02X Ch=%u Data1=%u Data2=%u",
                     _currentTick, _currentEventType, _currentChannel, _currentData1, _currentData2);
                break; // Allow main loop to process this event
            } else {
                _log(MIDI_LOG_VERBOSE, "Non-channel event processed. Continuing update loop if needed.");
                 // Continue processing more events if they are due within this update cycle
            }

        } else {
            // Next event in queue is not due yet
            _log(MIDI_LOG_VERBOSE, "Next event tick %lu (track %u) not due yet (due at %lu ms, current %lu ms). Waiting.",
                 nextEventTick, trackIndex, targetMillis, currentMillis);
            break; // Exit the while loop for this update() call
        }
    } // End while (!_eventQueue.empty())

    // Recalculate active tracks after processing events for this cycle
    size_t currentActive = 0;
    for(const auto& track : _trackStates) {
        if (!track.finished) {
            currentActive++;
        }
    }

    // Only log if the count actually changed
    if (currentActive != _activeTracks) {
         _log(MIDI_LOG_DEBUG, "Active track count updated from %u to %u", _activeTracks, currentActive);
        _activeTracks = currentActive;
    }

    // Check for finish condition if all tracks are now inactive
     if (_activeTracks == 0 && _currentState == MidiPlayerState::PLAYING) {
        if (!_eventQueue.empty()) {
             _log(MIDI_LOG_WARN, "All tracks marked finished, but event queue still has %u entries. Clearing.", _eventQueue.size());
             while(!_eventQueue.empty()) _eventQueue.pop();
        }
        _log(MIDI_LOG_INFO, "Playback completed: All tracks finished at tick %lu", _currentTick);
        _currentState = MidiPlayerState::FINISHED;
     }
}

bool ESP32MidiPlayer::isNoteOn(byte& channel, byte& note, byte& velocity) {
    if (_eventProcessed && _currentEventType == 0x90 && _currentData2 > 0) {
        channel = _currentChannel;
        note = _currentData1;
        velocity = _currentData2;
        _eventProcessed = false;
        _log(MIDI_LOG_DEBUG, "Note On consumed: Ch=%u Note=%u Vel=%u", channel, note, velocity);
        return true;
    }
    return false;
}

bool ESP32MidiPlayer::isNoteOff(byte& channel, byte& note) {
     if (_eventProcessed && (_currentEventType == 0x80 || (_currentEventType == 0x90 && _currentData2 == 0))) {
        channel = _currentChannel;
        note = _currentData1;
        _eventProcessed = false;
        _log(MIDI_LOG_DEBUG, "Note Off consumed: Ch=%u Note=%u", channel, note);
        return true;
    }
    return false;
}

bool ESP32MidiPlayer::isControlChange(byte& channel, byte& controller, byte& value) {
     if (_eventProcessed && _currentEventType == 0xB0) {
        channel = _currentChannel;
        controller = _currentData1;
        value = _currentData2;
        _eventProcessed = false;
        _log(MIDI_LOG_DEBUG, "Control Change consumed: Ch=%u Ctrl=%u Val=%u", channel, controller, value);
        return true;
    }
    return false;
}

bool ESP32MidiPlayer::isProgramChange(byte& channel, byte& program) {
    if (_eventProcessed && _currentEventType == 0xC0) {
        channel = _currentChannel;
        program = _currentData1;
        _eventProcessed = false;
        _log(MIDI_LOG_DEBUG, "Program Change consumed: Ch=%u Prog=%u", channel, program);
        return true;
    }
    return false;
}

bool ESP32MidiPlayer::isPitchBend(byte& channel, int& bendValue) {
    if (_eventProcessed && _currentEventType == 0xE0) {
        channel = _currentChannel;
        bendValue = (static_cast<int>(_currentData2) << 7 | _currentData1) - 8192;
        _eventProcessed = false;
        _log(MIDI_LOG_DEBUG, "Pitch Bend consumed: Ch=%u Bend=%d", channel, bendValue);
        return true;
    }
    return false;
}