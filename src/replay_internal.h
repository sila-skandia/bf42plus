#pragma once
// The round replay recorder's own seams. replay.h is what the rest of the DLL
// calls; this is what the recorder's files share with one another:
//
//   replay.cpp          the file, its clock and the entry points replay.h declares
//   replay_events.cpp   every game event the server sends, as an `e` record
//   replay_join.cpp     the join's events and objects held for a file begun mid-round
//   replay_objects.cpp  each sample: root objects, players, control points, tickets
//   replay_parts.cpp    each sample: turrets, engines, soldiers' bodies and kit part heat
//   replay_fire.cpp     every round the client fires, and the hook that sees them
//
// docs/replay-format.md describes every record these write.

#include "bf/generic.h"

#include <cstddef>
#include <cstdint>
#include <string>

class GameEvent;

// ---------------------------------------------------------------------------
// The file (replay.cpp)
// ---------------------------------------------------------------------------

// Seconds since the file opened, by the wall clock; 0 with no file.
double replayNow();

// A file is open.
bool replayFileOpen();

// Append one record. Flushes every couple of seconds.
void writeLine(const std::string& line);

// Escape an ISO-8859-1 byte string into a JSON string literal (without quotes).
std::string jsonEscape(const char* s, size_t len);
std::string jsonEscape(const std::string& s);

// Copy bytes out of game-owned memory under SEH; the bytes copied, 0 on a fault.
size_t safeCopy(void* dst, const void* src, size_t len);

// One value read out of game-owned memory at an offset, zero on a fault.
template <class T>
T readAt(const void* base, size_t offset)
{
    T value{};
    safeCopy(&value, (const uint8_t*)base + offset, sizeof(T));
    return value;
}

inline uintptr_t vtableOf(const void* obj)
{
    return readAt<uintptr_t>(obj, 0);
}

// ---------------------------------------------------------------------------
// Events (replay_events.cpp)
// ---------------------------------------------------------------------------

// Write the event as its `e` record. The file is open.
void recordEvent(GameEvent* event);

// sizeof() of the game's event class for this type, read out of the engine's
// own code; 0 when it cannot be confirmed.
size_t eventSize(int type);

// A decoded event as a JSON record, or "" when the recorder has no layout for it.
std::string decodeEvent(int type, const uint8_t* payload, size_t size, double t);

// The records for the events the join module holds, at time t.
std::string setLevelLine(GameEvent* event, double t);
std::string createObjectLine(GameEvent* event, double t);
std::string pickupKitLine(GameEvent* event, double t);
std::string createPlayerLine(GameEvent* event, double t);

// ---------------------------------------------------------------------------
// The join (replay_join.cpp)
// ---------------------------------------------------------------------------

// A new join: what the last one said, and what it made, no longer hold.
void forgetJoin();

// Keep what a file begun after this event would need from it. Called for
// every event, whether or not a file is open.
void holdJoinEvent(GameEvent* event);
void holdObjectEvent(GameEvent* event);

// Open a file that begins after the join with what is held and who is
// playing. Does nothing when nothing is held.
void writeHeldJoin();

// ---------------------------------------------------------------------------
// The samples (replay_objects.cpp, replay_parts.cpp)
// ---------------------------------------------------------------------------

// Forget what was last written; a new file starts from nothing.
void resetObjectSampler();
void resetPartSampler();

// One sample each. replay_onFrame calls them in the order the format
// document gives for the records within a sample.
void sampleObjects(double t);
void sampleParts(double t);
void sampleTickets(double t);
void samplePlayers(double t);
void sampleControlPoints(double t);

// Rotation of a transform as a unit quaternion, and change tests with a
// tolerance, shared by the two samplers.
Quat toQuat(const Mat4& m);
bool changed(const Pos3& a, const Pos3& b, float eps);
bool changed(const Quat& a, const Quat& b, float eps);
