#pragma once
#include <cstdint>

class GameEvent;

// Round replay recorder.
//
// When g_settings.recordReplays is on, every GameEvent received from the server
// and a periodic sample of every networked object's transform is appended as
// newline-delimited JSON to replays/replay_<timestamp>.ndjson in the game
// directory. Nothing is decoded from the network bitstream: the sampler reads
// the world after the client has applied the server's updates.

// Called from GameEventManager::getNextRcvdEvent_hook for every received event,
// before the event is handled (so events the DLL swallows are still logged).
// Also called with recording off: the join's own events (the server, the
// level, the rules) are kept, so a file begun mid-round still names them.
void replay_onEvent(GameEvent* event);

// Called once per rendered frame while in-game. Throttles itself to the sample rate.
void replay_onFrame();

// Called for every line the chat box displays, including the recording
// player's own, which never arrive as events.
void replay_onChat(const wchar_t* text, size_t length, int playerId, int team);

// Called when a player fires a weapon.
void replay_onFire(int playerId, bool isPrimary, bool isSecondary);

// Close the current file, if any.
void replay_stop();

// Install the recorder's own code hooks (every round fired, FireArms::Fire).
void replay_hook_init();
