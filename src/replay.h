#pragma once
#include <cstdint>

class GameEvent;

// Round replay recorder (phase 1: world-state sampler).
//
// When g_settings.recordReplays is on, every GameEvent received from the server
// and a periodic sample of every networked object's transform is appended as
// newline-delimited JSON to replays/replay_<timestamp>.ndjson in the game
// directory. Nothing is decoded from the network bitstream: the sampler reads
// the world after the client has applied the server's updates.
//
// See bfstats features/round-replay-capture/README.md for the format and plan.

// Called from GameEventManager::getNextRcvdEvent_hook for every received event,
// before the event is handled (so events the DLL swallows are still logged).
void replay_onEvent(GameEvent* event);

// Called once per rendered frame while in-game. Throttles itself to the sample rate.
void replay_onFrame();

// Called for every line the chat box displays, including the recording
// player's own, which never arrive as events.
void replay_onChat(const wchar_t* text, size_t length, int playerId, int team);

// Close the current file, if any.
void replay_stop();
