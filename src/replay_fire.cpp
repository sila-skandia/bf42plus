// Every round the client fires, as an `f` record, and the `fire` event for
// the recording player's own trigger.

#include "replay.h"
#include "replay_internal.h"

#include "bf/generic.h"
#include "bf/object.h"
#include "debug.h"
#include "hooks.h"

#include <windows.h>

#include <cstring>
#include <format>
#include <string>

void replay_onFire(int playerId, bool isPrimary, bool isSecondary)
{
    if (!replayFileOpen()) return;
    double t = replayNow();
    Pos3 pos(0, 0, 0);
    Vec3 dir(0, 0, 1);
    std::string weaponName;
    BFPlayer* player = BFPlayer::getFromID(playerId);
    if (player) {
        if (auto vehicle = player->getVehicle()) {
            auto& m = vehicle->getAbsoluteTransformation();
            pos = Pos3(m.position.x, m.position.y, m.position.z);
            dir = Vec3(m.c.x, m.c.y, m.c.z);
            if (auto tmpl = vehicle->getTemplate()) {
                weaponName = tmpl->getName();
            }
        }
    }
    writeLine(std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"fire\",\"pid\":{},\"kind\":{},\"weapon\":\"{}\",\"pos\":[{:.2f},{:.2f},{:.2f}],\"dir\":[{:.3f},{:.3f},{:.3f}]}}",
        t, playerId, isPrimary ? 1 : 2, jsonEscape(weaponName), pos.x, pos.y, pos.z, dir.x, dir.y, dir.z));
}

// ---------------------------------------------------------------------------
// Every round the client fires
// ---------------------------------------------------------------------------
//
// The client fires every weapon it simulates through one function,
// `FireArms::Fire` (BF1942.exe 0x0053D7B0, the twin of lnxded 0x0828A090):
// its own player's from input, and every other player's -- human or bot --
// from the fire and altfire flags the server replicates on each BFPlayer
// (+0x184/+0x185, applied by the BFPlayer networkable's setNetUpdate
// 0x00407710 at 0x00407B9B/0x00407BAF), which GameClient's per-tick loop
// 0x004B90D0 turns into handleMessage(6)/(7) on the player's vehicle for every
// player whose vehicle has live ghost data (0x004B9143..0x004B9179). So a
// hook where Fire commits a round sees every shot within the client's range,
// the bots' included.
//
// The hook sits at 0x0053DCB1 (`mov edx,[ebp+264h]`, 6 bytes), which every
// round-committing path reaches through 0x0053DCAA (nine jumps) after the
// barrel loop has run, and which no path that fires nothing reaches
// (overheat, fire delay, empty magazine, the medic's and engineer's tools).
// There: ebp is the FireArms, ebx the IPlayer* Fire was handed (the firing
// BFPlayer; null on the auto-fire path), [esp+10h] the FireArmsTemplate and
// [esp+3Ch..7Bh] the Mat4 the round was fired along (the camera's for a
// fireInCameraDof weapon, else the weapon's own, copied at 0x0053DA29).
// eax holds the fire timer's address for the original instruction, so the
// hook saves every register.

struct FireRecord {
    char weapon[64];
    int rootNetId;
    int playerId;
    bool local;
    Mat4 m;
};

// Everything the record needs, read under SEH: a pointer the engine hands a
// hook is trusted no further than a copy.
static bool readFire(IObject* fireArms, BFPlayer* player, const Mat4* m, FireRecord& out)
{
    __try {
        out.weapon[0] = 0;
        ObjectTemplate* tmpl = fireArms->getTemplate();
        if (tmpl) {
            const char* name = tmpl->getName().c_str();
            strncpy_s(out.weapon, name, _TRUNCATE);
        }
        IObject* root = fireArms;
        for (int depth = 0; root->getParent() && depth < 16; depth++) root = root->getParent();
        auto net = root->getNetworkable();
        out.rootNetId = net ? (int)net->getID() : -1;
        out.playerId = player ? player->getId() : -1;
        out.local = player && player == BFPlayer::getLocal();
        memcpy(&out.m, m, sizeof(Mat4));
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void __stdcall replay_onFireArms(IObject* fireArms, BFPlayer* player, const Mat4* m)
{
    if (!replayFileOpen() || !fireArms || !m) return;
    FireRecord r;
    if (!readFire(fireArms, player, m, r)) return;
    // Rows a, b, c of Mat4 are the X, Y, Z axes; a round leaves along +Z.
    writeLine(std::format("{{\"k\":\"f\",\"t\":{:.3f},\"id\":{},\"pid\":{},\"w\":\"{}\",\"p\":[{:.2f},{:.2f},{:.2f}],\"d\":[{:.3f},{:.3f},{:.3f}]{}}}",
        replayNow(), r.rootNetId, r.playerId, jsonEscape(r.weapon, strnlen(r.weapon, sizeof(r.weapon))),
        r.m.position.x, r.m.position.y, r.m.position.z, r.m.c.x, r.m.c.y, r.m.c.z,
        r.local ? ",\"local\":1" : ""));
}

static void* s_replayOnFireArms = (void*)&replay_onFireArms;

// The recording player's own radio. The server forwards a radio message to
// everyone but its sender, whose game plays it from the radio menu's send
// path instead (team radio 0x006D41C0, shouts 0x006D3320: the message to the
// server, then the menu's own receive, 0x006D3400 / 0x006D2790, with the local
// player). So a recording heard everybody's calls but its own. Hooked just
// before that local receive, it is written as the `radio` event the others
// arrive as, with `own` set.
static int readPlayerId(BFPlayer* player)
{
    __try {
        return player ? player->getId() : -1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

static void __stdcall replay_onLocalRadio(BFPlayer* player, int msg, int global)
{
    if (!replayFileOpen() || !player) return;
    int pid = readPlayerId(player);
    if (pid < 0) return;
    writeLine(std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"radio\",\"pid\":{},\"msg\":{},\"global\":{},\"own\":1}}",
        replayNow(), pid, msg & 0xFFFF, global ? 1 : 0));
}

static void* s_replayOnLocalRadio = (void*)&replay_onLocalRadio;

// `mov edx, [esi+0Ch]; push edx; push edi`: the message and the local player
// pushed for the menu's own receive, in both send paths.
static const uint8_t RADIO_SEND_BYTES[] = { 0x8B, 0x56, 0x0C, 0x52, 0x57 };

static bool radioSiteIs(uintptr_t addr)
{
    uint8_t have[sizeof(RADIO_SEND_BYTES)];
    return safeCopy(have, (void*)addr, sizeof(have)) == sizeof(have) && memcmp(have, RADIO_SEND_BYTES, sizeof(have)) == 0;
}

static void replay_hook_radio_init()
{
    if (!radioSiteIs(0x006D4244) || !radioSiteIs(0x006D33A4)) {
        debuglogt("replay: the radio menu's send is not the expected code, the player's own radio is not recorded\n");
        return;
    }
    // Team radio (the send path at 0x006D41C0).
    BEGIN_ASM_CODE(team)
        pushfd
        pushad
        push 1                          // over the team radio
        push dword ptr [esi + 0x0C]     // the message
        push edi                        // the local player
        mov eax, s_replayOnLocalRadio
        call eax                        // __stdcall, pops its three arguments
        popad
        popfd
    MOVE_CODE_AND_ADD_CODE(team, 0x006D4244, 5, HOOK_ADD_ORIGINAL_AFTER);
    // Shouts, heard within about 70 m (the send path at 0x006D3320).
    BEGIN_ASM_CODE(shout)
        pushfd
        pushad
        push 0
        push dword ptr [esi + 0x0C]
        push edi
        mov eax, s_replayOnLocalRadio
        call eax
        popad
        popfd
    MOVE_CODE_AND_ADD_CODE(shout, 0x006D33A4, 5, HOOK_ADD_ORIGINAL_AFTER);
}

void replay_hook_init()
{
    replay_hook_radio_init();
    // The bytes the hook moves, checked before anything is patched: another
    // build of BF1942.exe (or another DLL's patch here) leaves Fire alone.
    static const uint8_t expected[] = { 0x8B, 0x95, 0x64, 0x02, 0x00, 0x00 };
    uint8_t have[sizeof(expected)];
    if (safeCopy(have, (void*)0x0053DCB1, sizeof(have)) != sizeof(have)
        || memcmp(have, expected, sizeof(expected)) != 0) {
        debuglogt("replay: FireArms::Fire is not the expected code, shots are not recorded\n");
        return;
    }
    BEGIN_ASM_CODE(fire)
        pushfd
        pushad
        lea eax, [esp + 0x24 + 0x3C]   // the Mat4 Fire aimed along (past pushad and pushfd)
        push eax
        push ebx                        // the firing IPlayer*
        push ebp                        // the FireArms
        mov eax, s_replayOnFireArms
        call eax                        // __stdcall, pops its three arguments
        popad
        popfd
    MOVE_CODE_AND_ADD_CODE(fire, 0x0053DCB1, 6, HOOK_ADD_ORIGINAL_AFTER);
}
