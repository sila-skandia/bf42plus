#include "replay.h"

#include "bf/gameevent.h"
#include "bf/generic.h"
#include "bf/object.h"
#include "bf/stl.h"
#include "bf/ui.h"
#include "debug.h"
#include "hooks.h"
#include "settings.h"
#include "util.h"
#include "version.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <format>
#include <map>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static const int SAMPLE_HZ = 10;
static const int FLUSH_INTERVAL_MS = 2000;
// 2: "raw" events carry "size" and exactly the payload, not a fixed 48 bytes.
// 3: hit points ("a" records, "maxhp"/"crit" on "o"), chat as displayed
//    ("chat"), template names on object-creation events, one file per join
//    instead of closing at map end.
// 4: the engine events a replay can use are named instead of dumped (the
//    world clock, projectile pools, the rules, the server, the round-end
//    tallies; bfstats features/round-replay-capture/README.md §11.3), kills
//    name their weapon, a player record carries the hull a seat belongs to
//    and the player's trigger flags, every round the client fires is an "f"
//    record, and a hull's turrets ("jn"/"j"), its engines ("g") and every
//    soldier's animation states, aim and held item ("st", with the state
//    table once in "anim") are sampled with the objects.
// 5: a moving part's and an engine's id is the recorder's own, numbered once
//    per file: their networkables have none (getID() is 0 for every child;
//    v4 files keyed them all 0). "jn" adds the part's position in its root's
//    frame, which tells a hull's identically named guns apart. Added within
//    5 (a reader that does not know it skips it): "tk", the two sides'
//    tickets whenever they change; and for a file that begins after the join
//    (recording switched on mid-round), the join's own events at its head,
//    then each player's latest "createPlayer" (the soldier and kit he had at
//    the join), every object made before the file and not destroyed since
//    ("createObject", in the order they came), the projectile pools still
//    standing ("projPool") and each player's latest kit ("pickupKit"), each
//    with "ago", the seconds before the file began that it arrived, and
//    "roster", who was playing when it began.
static const int FORMAT_VERSION = 5;

static FILE* g_file = nullptr;
static LARGE_INTEGER g_qpcFreq = {};
static LARGE_INTEGER g_qpcStart = {};
static double g_lastSampleTime = -1.0;
static DWORD g_lastFlushTick = 0;
static bool g_fileHasContent = false;   // anything written after the header

struct ObjectState {
    Pos3 pos;
    Quat rot;
    int occupant;   // player id or -1
    bool seenThisSample;
    bool hasArmor;
    float hitPoints;
    int lastHitPlayer;
};
static std::map<uint16_t, ObjectState> g_objects;

struct PlayerState {
    int team;
    int vehicleNetId;   // -1 if none
    int triggers;       // 1 fire held, 2 altfire held: the server's flags on BFPlayer
};
static std::map<int, PlayerState> g_players;

struct ControlPointState {
    int team;
};
static std::map<int, ControlPointState> g_controlPoints;

// v4: a hull's moving parts, its engines and its soldiers' bodies, holding
// what was last written. A soldier is a root and keys by its network id; a
// part or an engine is a child object, whose networkable carries no id (a
// root's ghost carries its children's state), so it keys by the id the
// recorder gives it (childIdOf).
struct PartState {
    Quat rel;       // rotation relative to the root object
};
static std::map<int, PartState> g_parts;
struct EngineState {
    float revs;
    float throttle;
    int flags;
    int gear;
};
static std::map<int, EngineState> g_engines;
struct ChildId {
    int id;
    uint16_t root;      // the root's network id
    const void* tmpl;
};
static std::map<const void*, ChildId> g_childIds;
static int g_nextChildId = 1;
struct SoldierState {
    int lower;
    int upper;
    int item;
    int bits;
    float pitch;
    float twist;
};
static std::map<uint16_t, SoldierState> g_soldiers;
static bool g_animStatesWritten = false;
// The two sides' tickets as last written (-1: nothing yet).
static int g_tickets[2] = { -1, -1 };

// What the last join said, for a file that begins after it. Recording can be
// switched on at any time (plus.recordReplays 1), and a file that begins
// mid-round has missed the join: the server, its mod, the level and its game
// type, the rules, and every player's createPlayer, which is where names come
// from. The join's events are kept as they pass whether or not a file is open,
// and a file that begins after them opens with them and with who is playing
// (replay_start). A new join (0x1A) lets the last one's go.
struct HeldEvent {
    int type;
    DWORD tick;         // GetTickCount() when it arrived
    std::string line;   // the record as written, at t 0
};
static std::vector<HeldEvent> g_heldJoin;

// The objects the join made, and every one since, held the same way and let
// go with the join's events. CreateObject (0x07) comes once for every object,
// the join's database sending every one that stands, but the sampler names
// only what the server replicates, within the view distance: a file begun
// after the join would know a carrier across the map only as the root id of
// its turrets and engines. Held: each object's createObject until a
// DestroyObject (0x06) for its id; each projectile pool (0x05) until one for
// its first id (a pool goes all at once); and each player's latest pickupKit
// (0x23) until that kit is destroyed or he leaves (0x0C), a player id passing
// to whoever joins next. And each player's latest createPlayer (0x08) until
// he leaves: the join's database gives a player already spawned his soldier
// and his kit there (vehNetId, kitNetId), and sends no pickupKit for him.
struct HeldRecord {
    uint32_t order;     // arrival, across all four
    DWORD tick;         // GetTickCount() when it arrived
    uint16_t netId;     // the object, the pool's first id, the kit, or the player's own
    std::string line;   // the record as written, at t 0
};
static std::map<int, HeldRecord> g_heldPlayers;        // by player id
static std::map<uint16_t, HeldRecord> g_heldCreates;   // by network id
static std::map<uint16_t, HeldRecord> g_heldPools;     // by first network id
static std::map<int, HeldRecord> g_heldKits;           // by player id
static uint32_t g_heldOrder = 0;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static double replayNow()
{
    if (g_qpcFreq.QuadPart == 0) return 0.0;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - g_qpcStart.QuadPart) / (double)g_qpcFreq.QuadPart;
}

// Escape an ISO-8859-1 byte string into a JSON string literal (without quotes).
static std::string jsonEscape(const char* s, size_t len)
{
    std::string out;
    out.reserve(len + 8);
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20 || c >= 0x7F) {
                    char buf[8];
                    _snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                }
                else out += (char)c;
        }
    }
    return out;
}

static std::string jsonEscape(const std::string& s) { return jsonEscape(s.data(), s.size()); }

// Copy bytes out of a game-owned struct without trusting its size. Used to dump
// the payload of events whose layout we have not mapped yet.
static size_t safeCopy(void* dst, const void* src, size_t len)
{
    __try {
        memcpy(dst, src, len);
        return len;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static std::string hexDump(const void* src, size_t len)
{
    // Large enough for the biggest event the engine defines: SetLevel, 158 bytes.
    uint8_t buf[256];
    if (len > sizeof(buf)) len = sizeof(buf);
    size_t got = safeCopy(buf, src, len);
    std::string out;
    out.reserve(got * 2);
    static const char* hex = "0123456789abcdef";
    for (size_t i = 0; i < got; i++) {
        out += hex[buf[i] >> 4];
        out += hex[buf[i] & 15];
    }
    return out;
}

static std::string asciiDump(const void* src, size_t len)
{
    uint8_t buf[256];
    if (len > sizeof(buf)) len = sizeof(buf);
    size_t got = safeCopy(buf, src, len);
    std::string out;
    out.reserve(got);
    for (size_t i = 0; i < got; i++) {
        uint8_t c = buf[i];
        if (c >= 0x20 && c < 0x7F) out += (char)c;
        else out += '.';
    }
    return out;
}

static FILE* g_unknownLogFile = nullptr;
static void logUnknownEventDebug(int type, const uint8_t* payload, size_t size, double t)
{
    if (!g_unknownLogFile) {
        CreateDirectoryA("replays", nullptr);
        g_unknownLogFile = fopen("replays\\unknown_events.log", "a+b");
        if (!g_unknownLogFile) return;
    }
    time_t now = time(nullptr);
    struct tm lt;
    localtime_s(&lt, &now);
    char timeStr[64];
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%dT%H:%M:%S", &lt);

    size_t dumpLen = size > 12 ? size - 12 : 48;
    std::string hex = hexDump(payload, dumpLen);
    std::string ascii = asciiDump(payload, dumpLen);

    fprintf(g_unknownLogFile, "[%s] t=%.3fs type=0x%02X (%d) size=%zu payload=%s ascii=\"%s\"\n",
        timeStr, t, type, type, size, hex.c_str(), ascii.c_str());
    fflush(g_unknownLogFile);
}

// sizeof() of a game event class, read out of the engine's own code rather than
// guessed. Every GameEventMaker::createEvent in BF1942.exe opens with
//     mov ecx, sizeof(T)
//     call GameEvent::allocate
// -- true of all 51 makers the exe registers, and in agreement with every struct
// size gameevent.h static_asserts (bfstats features/round-replay-capture/
// event_sizes.py). Returns 0 when there is no maker or its code has a different
// shape, e.g. a maker added by another DLL, so the caller never trusts a bad read.
static size_t eventSize(int type)
{
    static const uintptr_t GAMEEVENT_ALLOCATE = 0x004A6290;
    static int16_t cache[256];
    static bool cacheReady = false;
    if (!cacheReady) {
        std::fill(std::begin(cache), std::end(cache), (int16_t)-1);
        cacheReady = true;
    }
    const bool cacheable = type >= 0 && type < 256;
    if (cacheable && cache[type] >= 0) return (size_t)cache[type];

    size_t size = 0;
    if (auto maker = GameEvent_getEventMaker((GameEventID)type)) {
        uintptr_t vtable = 0, createEvent = 0;
        uint8_t code[10];
        if (safeCopy(&vtable, maker, sizeof(vtable)) == sizeof(vtable)
            && safeCopy(&createEvent, (void*)(vtable + 4), sizeof(createEvent)) == sizeof(createEvent)
            && safeCopy(code, (void*)createEvent, sizeof(code)) == sizeof(code)
            && code[0] == 0xB9 && code[5] == 0xE8) {
            int32_t rel;
            uint32_t imm;
            memcpy(&rel, code + 6, sizeof(rel));
            memcpy(&imm, code + 1, sizeof(imm));
            if (createEvent + 10 + rel == GAMEEVENT_ALLOCATE && imm >= 12 && imm <= 4096) size = imm;
        }
    }
    if (cacheable) cache[type] = (int16_t)size;
    return size;
}

// Armor, the component that holds an object's hit points. Layout confirmed in
// BF1942.exe (vtable 0x008DC7A8) and against the Linux server's symbols, where
// the same getters sit in the same slots reading the same offsets:
//     slot  5 getMaxHitPoints    fld [ecx+3Ch]
//     slot  7 getHitPoints       fld [ecx+38h]
//     slot 11 getCriticalDamage  fld [ecx+0F0h]
//     slot 30 getLastHitPlayer   mov eax, [ecx+14h]
// IObject::queryComponent(IID_IArmor, IID_IArmor) returns it; the client makes
// that exact call itself at 84 sites.
static const unsigned int IID_IArmor = 0xc4a4;

struct ArmorFields {
    float hitPoints;
    float maxHitPoints;
    float criticalDamage;   // below this the object burns and loses hit points on its own
    int lastHitPlayer;      // -1 on the client even for kills: set server-side, not replicated
};

// True when this vtable's getters read the offsets readArmor reads directly.
// Checked once per vtable, so an Armor subclass with a different layout is
// skipped rather than misread.
static bool armorVtableMatches(uintptr_t vtable)
{
    static std::map<uintptr_t, bool> checked;
    auto it = checked.find(vtable);
    if (it != checked.end()) return it->second;

    struct Getter {
        int slot;
        uint8_t code[7];
        size_t length;
    };
    static const Getter expected[] = {
        { 5,  { 0xD9, 0x41, 0x3C, 0xC3 }, 4 },
        { 7,  { 0xD9, 0x41, 0x38, 0xC3 }, 4 },
        { 11, { 0xD9, 0x81, 0xF0, 0x00, 0x00, 0x00, 0xC3 }, 7 },
        { 30, { 0x8B, 0x41, 0x14, 0xC3 }, 4 },
    };
    bool ok = true;
    for (auto& getter : expected) {
        uintptr_t fn = 0;
        uint8_t code[7];
        if (safeCopy(&fn, (void*)(vtable + 4 * getter.slot), sizeof(fn)) != sizeof(fn)
            || safeCopy(code, (void*)fn, getter.length) != getter.length
            || memcmp(code, getter.code, getter.length) != 0) {
            ok = false;
            break;
        }
    }
    checked[vtable] = ok;
    if (!ok) debuglogt("replay: armor vtable %p has unexpected getters, its hit points are not recorded\n", (void*)vtable);
    return ok;
}

static bool readArmor(IObject* obj, ArmorFields& out)
{
    void* armor = obj->queryComponent(IID_IArmor, IID_IArmor);
    if (!armor) return false;
    uintptr_t vtable = 0;
    if (safeCopy(&vtable, armor, sizeof(vtable)) != sizeof(vtable) || !armorVtableMatches(vtable)) return false;
    const uint8_t* base = (const uint8_t*)armor;
    return safeCopy(&out.hitPoints, base + 0x38, 4) == 4
        && safeCopy(&out.maxHitPoints, base + 0x3C, 4) == 4
        && safeCopy(&out.criticalDamage, base + 0xF0, 4) == 4
        && safeCopy(&out.lastHitPlayer, base + 0x14, 4) == 4;
}

// Object-creation events (0x07: u32 templateId, u16 networkId, u8, vec3
// position, vec3 rotation) identify their object only by template id. Name it
// while the template table is at hand, so objects the client never receives
// updates for -- anything beyond the ~520 m relevance radius -- are named too.
static std::string rawEventExtraFields(int type, const uint8_t* payload, size_t size)
{
    if (type != 0x07 || size < 12 + sizeof(uint32_t)) return std::string();
    uint32_t templateId = 0;
    if (safeCopy(&templateId, payload, sizeof(templateId)) != sizeof(templateId)) return std::string();
    auto tmpl = ObjectTemplateManager_getTemplate(templateId);
    if (!tmpl) return std::string();
    return std::format(",\"tmpl\":\"{}\"", jsonEscape(std::string(tmpl->getName())));
}

// --- decoded engine events ------------------------------------------------
//
// Layouts read out of the Linux server's own event classes (registered by id
// in GameEvent::initEvents 0x0811bee0, whose in-memory layout the client's
// GameClient::processEvent 0x004933D0 reads at the same offsets) and checked
// against real recordings and the server's own event log; bfstats
// features/round-replay-capture/README.md §11.3 has the table and the
// evidence. Offsets are into the payload, after the 12-byte header. Every
// read goes through safeCopy, and an event whose size the engine does not
// confirm is dumped raw as before.

template <class T>
static T payloadAt(const uint8_t* payload, size_t offset)
{
    T value{};
    safeCopy(&value, payload + offset, sizeof(T));
    return value;
}

static std::string payloadString(const uint8_t* payload, size_t offset, size_t max)
{
    char buf[80] = {};
    if (max >= sizeof(buf)) max = sizeof(buf) - 1;
    size_t got = safeCopy(buf, payload + offset, max);
    return jsonEscape(buf, strnlen(buf, got));
}

// The name of an object template by its network id, or "" -- for a weapon id
// in a kill, a pool's projectile, a round-end tally's key. Template ids are
// the load order of this client's templates, so only a name survives the
// recording; guarded because a kill's weapon field is garbage for kinds that
// do not set it.
static ObjectTemplate* templateById(uint32_t tid)
{
    __try {
        return ObjectTemplateManager_getTemplate(tid);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

static std::string templateNameOf(uint32_t tid)
{
    if (tid == 0 || tid > 0x00FFFFFF) return std::string();
    ObjectTemplate* tmpl = templateById(tid);
    return tmpl ? jsonEscape(std::string(tmpl->getName())) : std::string();
}

// A decoded event as a JSON record, or "" to fall back to the raw dump.
static std::string decodeEvent(int type, const uint8_t* p, size_t size, double t)
{
    switch (type) {
        case 0x04:  // SimulationEvent: u8 running, f32 world time at this join
            if (size != 17) break;
            return std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"simStart\",\"running\":{},\"worldTime\":{:.3f}}}",
                t, payloadAt<uint8_t>(p, 0), payloadAt<float>(p, 1));
        case 0x05: {  // CreateMultipleObjectsEvent: a kit's projectile pool
            if (size != 22) break;
            uint32_t tid = payloadAt<uint32_t>(p, 0);
            return std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"projPool\",\"tid\":{},\"tmpl\":\"{}\",\"netId\":{},\"count\":{}}}",
                t, tid, templateNameOf(tid), payloadAt<uint16_t>(p, 4), payloadAt<int32_t>(p, 6));
        }
        case 0x13: {  // MapEvent: one map-rotation entry
            if (size != 112) break;
            static const char* modes[] = { "", "ctf", "conquest", "tdm", "coop", "objective" };
            uint8_t mode = payloadAt<uint8_t>(p, 3);
            return std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"mapList\",\"add\":{},\"map\":\"{}\",\"mode\":\"{}\",\"mod\":\"{}\"}}",
                t, payloadAt<uint8_t>(p, 0), payloadString(p, 4, 64), mode < 6 ? modes[mode] : "", payloadString(p, 68, 32));
        }
        case 0x14:  // ChallengeEvent: the join's hash challenge; the token is dropped
            if (size != 52) break;
            return std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"challenge\",\"mod\":\"{}\",\"xpack\":{}}}",
                t, payloadString(p, 10, 16), payloadAt<int32_t>(p, 27));
        case 0x16:  // GameRulesEvent
            if (size != 31) break;
            return std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"gameRules\",\"extViews\":{},\"noseCam\":{},\"soldierFF\":{:.3f},\"ticketRatio\":{:.3f},\"timeLimit\":{},\"worldTime\":{},\"crosshair\":{}}}",
                t, payloadAt<uint8_t>(p, 0), payloadAt<uint8_t>(p, 1), payloadAt<float>(p, 2), payloadAt<float>(p, 6),
                payloadAt<uint32_t>(p, 10), payloadAt<uint32_t>(p, 14), payloadAt<uint8_t>(p, 18));
        case 0x1A:  // ServerInfoEvent: the first event of a join
            if (size != 63) break;
            return std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"serverInfo\",\"mapId\":\"{}\",\"mod\":\"{}\",\"gameId\":\"{}\"}}",
                t, payloadString(p, 0, 16), payloadString(p, 17, 16), payloadString(p, 34, 16));
        case 0x1B:  // ServerInfoEvent2: the server's name
            if (size != 45) break;
            return std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"serverName\",\"name\":\"{}\"}}",
                t, payloadString(p, 0, 32));
        case 0x29:  // TimerSyncEvent, every 10 s
            if (size != 20) break;
            return std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"clock\",\"timeLimit\":{},\"worldTime\":{}}}",
                t, payloadAt<uint32_t>(p, 0), payloadAt<uint32_t>(p, 4));
        case 0x30:  // StatsKillsEvent / StatsShotsEvent / StatsHitsEvent: one
        case 0x31:  // player's round tally by template, at the round's end
        case 0x32: {
            if (size != 74) break;
            static const char* stats[] = { "destroyed", "fired", "hit" };
            int rows = payloadAt<uint8_t>(p, 0) + 1;
            if (rows > 10) rows = 10;
            std::string out;
            for (int i = 0; i < rows; i++) {
                uint32_t tid = payloadAt<uint32_t>(p, 2 + 4 * i);
                if (!out.empty()) out += ',';
                out += std::format("{{\"tid\":{},\"tmpl\":\"{}\",\"n\":{}}}", tid, templateNameOf(tid), payloadAt<uint16_t>(p, 42 + 2 * i));
            }
            return std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"roundStats\",\"stat\":\"{}\",\"pid\":{},\"rows\":[{}]}}",
                t, stats[type - 0x30], payloadAt<uint8_t>(p, 1), out);
        }
        case 0x37:  // SessionIdEvent: this client's connection id
            if (size != 76) break;
            return std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"sessionId\",\"id\":\"{}\"}}",
                t, payloadString(p, 0, 64));
        case 0x3C:  // HitFromPosEvent: this client's soldier was hurt
            // lnxded GameServer::_giveDamage 0x0814b870 sends it to the
            // damaged player's client alone: `dir` the sector the damage came
            // from, 45 degrees each (0 within 22.5 of ahead, 4 behind; the
            // cos 22.5 threshold is 0x086c0b68), `strength` 255 x the damage
            // over the victim's own vtable+0x14 value, truncated (0x0814ba31):
            // the hit indicator the HUD draws.
            if (size != 14) break;
            return std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"hitFrom\",\"dir\":{},\"strength\":{}}}",
                t, payloadAt<uint8_t>(p, 0), payloadAt<uint8_t>(p, 1));
        default:
            break;
    }
    return std::string();
}

// The network id of the root object `obj` hangs under (itself for a root),
// or -1: a seat's hull for the player record.
static int rootNetIdOf(IObject* obj)
{
    __try {
        IObject* root = obj;
        for (int depth = 0; root && root->getParent() && depth < 16; depth++) root = root->getParent();
        auto net = root ? root->getNetworkable() : nullptr;
        return net ? (int)net->getID() : -1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// Rotation of the object's absolute transform as a unit quaternion. Rows a, b, c
// of Mat4 are taken as the X, Y, Z basis vectors. If the viewer ends up mirrored
// the fix belongs there, not here: this is what the engine holds.
static Quat toQuat(const Mat4& m)
{
    float m00 = m.a.x, m01 = m.a.y, m02 = m.a.z;
    float m10 = m.b.x, m11 = m.b.y, m12 = m.b.z;
    float m20 = m.c.x, m21 = m.c.y, m22 = m.c.z;
    float trace = m00 + m11 + m22;
    Quat q;
    if (trace > 0.0f) {
        float s = sqrtf(trace + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (m12 - m21) / s;
        q.y = (m20 - m02) / s;
        q.z = (m01 - m10) / s;
    }
    else if (m00 > m11 && m00 > m22) {
        float s = sqrtf(1.0f + m00 - m11 - m22) * 2.0f;
        q.w = (m12 - m21) / s;
        q.x = 0.25f * s;
        q.y = (m10 + m01) / s;
        q.z = (m20 + m02) / s;
    }
    else if (m11 > m22) {
        float s = sqrtf(1.0f + m11 - m00 - m22) * 2.0f;
        q.w = (m20 - m02) / s;
        q.x = (m10 + m01) / s;
        q.y = 0.25f * s;
        q.z = (m21 + m12) / s;
    }
    else {
        float s = sqrtf(1.0f + m22 - m00 - m11) * 2.0f;
        q.w = (m01 - m10) / s;
        q.x = (m20 + m02) / s;
        q.y = (m21 + m12) / s;
        q.z = 0.25f * s;
    }
    return q;
}

static bool changed(const Pos3& a, const Pos3& b, float eps)
{
    return fabsf(a.x - b.x) > eps || fabsf(a.y - b.y) > eps || fabsf(a.z - b.z) > eps;
}

static bool changed(const Quat& a, const Quat& b, float eps)
{
    return fabsf(a.x - b.x) > eps || fabsf(a.y - b.y) > eps || fabsf(a.z - b.z) > eps || fabsf(a.w - b.w) > eps;
}

static void writeLine(const std::string& line)
{
    if (!g_file) return;
    fwrite(line.data(), 1, line.size(), g_file);
    fputc('\n', g_file);
    g_fileHasContent = true;

    DWORD tick = GetTickCount();
    if (tick - g_lastFlushTick > FLUSH_INTERVAL_MS) {
        fflush(g_file);
        g_lastFlushTick = tick;
    }
}

// ---------------------------------------------------------------------------
// File lifecycle
// ---------------------------------------------------------------------------

// Who is playing, as `[pid, team, ai, name, local]` (local 1 for the recording
// player): for a file that begins after the join, whose players' createPlayer
// events all went by before it.
static void writeRoster(double t)
{
    auto players = BFPlayer::getPlayers();
    if (!players) return;
    const BFPlayer* local = BFPlayer::getLocal();
    std::string out;
    for (auto it = players->begin(); it != players->end(); ++it) {
        BFPlayer* player = *it;
        if (!player) continue;
        if (!out.empty()) out += ',';
        out += std::format("[{},{},{},\"{}\",{}]", player->getId(), player->getTeam(),
            player->getIsAIPlayer() ? 1 : 0, jsonEscape(std::string(player->getName())), player == local ? 1 : 0);
    }
    if (!out.empty()) writeLine(std::format("{{\"k\":\"roster\",\"t\":{:.3f},\"p\":[{}]}}", t, out));
}

// A held record written into the file with "ago", the seconds before `now`
// that it arrived.
static void writeHeld(const std::string& line, DWORD tick, DWORD now)
{
    std::string out = line;
    out.insert(out.size() - 1, std::format(",\"ago\":{:.3f}", (now - tick) / 1000.0));
    writeLine(out);
}

// Held records in the order they arrived.
template <class Key>
static void writeHeldInOrder(const std::map<Key, HeldRecord>& held, DWORD now)
{
    std::vector<const HeldRecord*> ordered;
    ordered.reserve(held.size());
    for (const auto& kv : held) ordered.push_back(&kv.second);
    std::sort(ordered.begin(), ordered.end(), [](const HeldRecord* a, const HeldRecord* b) { return a->order < b->order; });
    for (const HeldRecord* record : ordered) writeHeld(record->line, record->tick, now);
}

static bool replay_start()
{
    if (g_file) return true;
    if (!g_settings.recordReplays) return false;

    CreateDirectoryA("replays", nullptr);

    time_t t = time(nullptr);
    struct tm lt;
    localtime_s(&lt, &t);
    char name[128];
    strftime(name, sizeof(name), "replays\\replay_%Y%m%d-%H%M%S.ndjson", &lt);

    g_file = fopen(name, "wb");
    if (!g_file) {
        debuglogt("replay: failed to open %s\n", name);
        return false;
    }
    setvbuf(g_file, nullptr, _IOFBF, 1 << 16);

    QueryPerformanceFrequency(&g_qpcFreq);
    QueryPerformanceCounter(&g_qpcStart);
    g_lastSampleTime = -1.0;
    g_lastFlushTick = GetTickCount();
    g_objects.clear();
    g_players.clear();
    g_controlPoints.clear();
    g_parts.clear();
    g_engines.clear();
    g_childIds.clear();
    g_nextChildId = 1;
    g_soldiers.clear();
    g_animStatesWritten = false;
    g_tickets[0] = g_tickets[1] = -1;

    char iso[64];
    strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%S", &lt);
    writeLine(std::format("{{\"k\":\"h\",\"v\":{},\"plus\":\"{}\",\"start\":\"{}\",\"hz\":{}}}",
        FORMAT_VERSION, jsonEscape(WideStringToISO88591(build_version)), iso, SAMPLE_HZ));
    g_fileHasContent = false;

    debuglogt("replay: recording to %s\n", name);
    BfMenu::getSingleton()->outputConsole(std::string("plus: recording replay to ") + name);

    // A file that begins after the join opens with what the join said, the
    // players, the objects standing, their projectile pools and the players'
    // kits, and who is playing. One that begins with the join holds nothing
    // yet (its 0x1A let the last join's go) and records the join as it comes.
    if (!g_heldJoin.empty() || !g_heldPlayers.empty() || !g_heldCreates.empty() || !g_heldPools.empty()
        || !g_heldKits.empty()) {
        const DWORD now = GetTickCount();
        for (const auto& held : g_heldJoin) writeHeld(held.line, held.tick, now);
        writeHeldInOrder(g_heldPlayers, now);
        writeHeldInOrder(g_heldCreates, now);
        writeHeldInOrder(g_heldPools, now);
        writeHeldInOrder(g_heldKits, now);
        writeRoster(replayNow());
        debuglogt("replay: began after the join: %zu of its events, %zu players, %zu objects, %zu pools, %zu kits and the roster written\n",
            g_heldJoin.size(), g_heldPlayers.size(), g_heldCreates.size(), g_heldPools.size(), g_heldKits.size());
    }
    return true;
}

void replay_stop()
{
    if (!g_file) return;
    writeLine(std::format("{{\"k\":\"end\",\"t\":{:.3f}}}", replayNow()));
    fclose(g_file);
    g_file = nullptr;
    debuglogt("replay: stopped\n");
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

static std::string setLevelLine(GameEvent* event, double t)
{
    auto ev = reinterpret_cast<SetLevelEvent*>(event);
    size_t lvlLen = strnlen(ev->levelPath, sizeof(ev->levelPath));
    size_t modeLen = strnlen(ev->gameModeFile, sizeof(ev->gameModeFile));
    return std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"setLevel\",\"level\":\"{}\",\"mode\":\"{}\"}}",
        t, jsonEscape(ev->levelPath, lvlLen), jsonEscape(ev->gameModeFile, modeLen));
}

// The template is looked up under SEH: this runs for every object whether or
// not recording is on (holdObjectEvent).
static std::string createObjectLine(GameEvent* event, double t)
{
    auto ev = reinterpret_cast<CreateObjectEvent*>(event);
    auto tmpl = templateById(ev->templateId);
    std::string tmplName = tmpl ? jsonEscape(std::string(tmpl->getName())) : std::string();
    return std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"createObject\",\"tid\":{},\"netId\":{},\"tmpl\":\"{}\",\"pos\":[{:.2f},{:.2f},{:.2f}],\"rot\":[{:.2f},{:.2f},{:.2f}]}}",
        t, ev->templateId, ev->objectNetId, tmplName, ev->position.x, ev->position.y, ev->position.z, ev->rotation.x, ev->rotation.y, ev->rotation.z);
}

static std::string pickupKitLine(GameEvent* event, double t)
{
    auto ev = reinterpret_cast<PickupKitEvent*>(event);
    return std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"pickupKit\",\"pid\":{},\"netId\":{}}}", t, ev->playerID, ev->kitNetId);
}

static std::string createPlayerLine(GameEvent* event, double t)
{
    auto ev = reinterpret_cast<CreatePlayerEvent*>(event);
    size_t nameLen = strnlen(ev->name, sizeof(ev->name));
    return std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"createPlayer\",\"pid\":{},\"name\":\"{}\",\"team\":{},\"ai\":{},\"netId\":{},\"vehNetId\":{},\"camNetId\":{},\"kitNetId\":{}}}",
        t, ev->playerID, jsonEscape(ev->name, nameLen), ev->team, ev->isAI ? 1 : 0,
        ev->playerNetworkID, ev->vehicleNetworkID, ev->cameraNetworkID, ev->kitNetworkID);
}

// Keep the join's events a file that begins after them opens with
// (g_heldJoin), the latest of each kind. Not the world clock (0x04): its time
// is the join's, and moved to the file's start it would be wrong.
static void holdJoinEvent(GameEvent* event)
{
    const int type = event->getType();
    std::string line;
    switch (type) {
        case BF_SetLevelEvent:
            line = setLevelLine(event, 0.0);
            break;
        case 0x14:  // ChallengeEvent: the mod and pack the server runs
        case 0x16:  // GameRulesEvent
        case 0x1A:  // ServerInfoEvent: the server's mod
        case 0x1B: {  // ServerInfoEvent2: the server's name
            const size_t size = eventSize(type);
            if (size) line = decodeEvent(type, (const uint8_t*)event + 12, size, 0.0);
            break;
        }
        default:
            return;
    }
    if (line.empty()) return;
    const HeldEvent held{ type, GetTickCount(), line };
    for (auto& h : g_heldJoin) {
        if (h.type == type) {
            h = held;
            return;
        }
    }
    g_heldJoin.push_back(held);
}

// Keep the players, the objects standing, their pools and the players' kits
// for a file that begins after them (g_heldPlayers, g_heldCreates,
// g_heldPools, g_heldKits). A record for an id already held replaces it and
// takes its new place in the order.
static void holdObjectEvent(GameEvent* event)
{
    const int type = event->getType();
    const uint8_t* payload = (const uint8_t*)event + 12;
    switch (type) {
        case BF_CreatePlayerEvent: {
            auto ev = reinterpret_cast<CreatePlayerEvent*>(event);
            g_heldPlayers[ev->playerID] = HeldRecord{ g_heldOrder++, GetTickCount(), ev->playerNetworkID, createPlayerLine(event, 0.0) };
            break;
        }
        case BF_CreateObjectEvent: {
            const uint16_t id = reinterpret_cast<CreateObjectEvent*>(event)->objectNetId;
            g_heldCreates[id] = HeldRecord{ g_heldOrder++, GetTickCount(), id, createObjectLine(event, 0.0) };
            break;
        }
        case 0x05: {  // CreateMultipleObjectsEvent: a kit's projectile pool
            const size_t size = eventSize(type);
            std::string line = size ? decodeEvent(type, payload, size, 0.0) : std::string();
            if (line.empty()) break;
            const uint16_t first = payloadAt<uint16_t>(payload, 4);
            g_heldPools[first] = HeldRecord{ g_heldOrder++, GetTickCount(), first, std::move(line) };
            break;
        }
        case BF_PickupKitEvent: {
            auto ev = reinterpret_cast<PickupKitEvent*>(event);
            g_heldKits[ev->playerID] = HeldRecord{ g_heldOrder++, GetTickCount(), ev->kitNetId, pickupKitLine(event, 0.0) };
            break;
        }
        case BF_DestroyObjectEvent: {
            const uint16_t id = reinterpret_cast<DestroyObjectEvent*>(event)->objectNetId;
            g_heldCreates.erase(id);
            g_heldPools.erase(id);
            std::erase_if(g_heldKits, [id](const auto& kv) { return kv.second.netId == id; });
            break;
        }
        case BF_DestroyPlayerEvent: {
            const int pid = reinterpret_cast<DestroyPlayerEvent*>(event)->playerid;
            g_heldPlayers.erase(pid);
            g_heldKits.erase(pid);
            break;
        }
        default:
            break;
    }
}

static void recordEvent(GameEvent* event);

void replay_onEvent(GameEvent* event)
{
    // A new join: what the last one said, and what it made, no longer hold.
    // Let go first, so a file this event starts does not open with the last
    // server's level, players or objects.
    if (event->getType() == 0x1A) {
        g_heldJoin.clear();
        g_heldPlayers.clear();
        g_heldCreates.clear();
        g_heldPools.clear();
        g_heldKits.clear();
    }
    recordEvent(event);
    holdJoinEvent(event);
    holdObjectEvent(event);
}

static void recordEvent(GameEvent* event)
{
    if (!g_settings.recordReplays) return;
    // A join sequence starts a new file; every join recorded so far opens with
    // event 0x1A. Map end is deliberately not the boundary: the server keeps
    // sending that map's teardown after GameStatus(ENDMAP), and the client
    // relaunches between maps anyway, closing the file on its way out.
    if (g_file && g_fileHasContent && event->getType() == 0x1A) replay_stop();
    if (!g_file && !replay_start()) return;

    const double t = replayNow();
    const int type = event->getType();
    // Payload starts after vtable, sequenceNumber, nextEvent.
    const uint8_t* payload = (const uint8_t*)event + 12;

    switch (type) {
        case BF_CreatePlayerEvent:
            writeLine(createPlayerLine(event, t));
            break;
        case BF_DestroyPlayerEvent: {
            auto ev = reinterpret_cast<DestroyPlayerEvent*>(event);
            writeLine(std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"destroyPlayer\",\"pid\":{}}}", t, ev->playerid));
            break;
        }
        case BF_SetTeamEvent: {
            auto ev = reinterpret_cast<SetTeamEvent*>(event);
            writeLine(std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"setTeam\",\"pid\":{},\"team\":{}}}", t, ev->playerid, ev->teamid));
            break;
        }
        case BF_ScoreMsgEvent: {
            auto ev = reinterpret_cast<ScoreMsgEvent*>(event);
            // A kill's weapon is the killing object's template id (the weapon
            // on foot, the hull when mounted); other kinds leave it unset.
            const bool kill = ev->eventid == SE_KILL || ev->eventid == SE_TK;
            std::string weaponName = kill && ev->weapon > 0 ? templateNameOf((uint32_t)ev->weapon) : std::string();
            writeLine(std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"score\",\"kind\":{},\"pid\":{},\"victim\":{},\"weapon\":{},\"bodypart\":{}{}}}",
                t, (uint32_t)ev->eventid, ev->playerid, ev->victimpid, ev->weapon, ev->bodypart,
                weaponName.empty() ? std::string() : std::format(",\"weaponName\":\"{}\"", weaponName)));
            break;
        }
        case BF_ChatFragmentEvent: {
            auto ev = reinterpret_cast<ChatFragmentEvent*>(event);
            size_t len = ev->textLength > sizeof(ev->text) ? sizeof(ev->text) : ev->textLength;
            writeLine(std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"chat\",\"pid\":{},\"first\":{},\"global\":{},\"server\":{},\"total\":{},\"text\":\"{}\"}}",
                t, ev->senderID, ev->firstFragment ? 1 : 0, ev->broadcast ? 1 : 0, ev->serverMessage ? 1 : 0,
                ev->totalLength, jsonEscape(ev->text, len)));
            break;
        }
        case BF_RadioMessageEvent: {
            auto ev = reinterpret_cast<RadioMessageEvent*>(event);
            writeLine(std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"radio\",\"pid\":{},\"msg\":{},\"global\":{}}}",
                t, ev->playerid, ev->messageID, ev->broadcast ? 1 : 0));
            break;
        }
        case BF_GameStatusEvent: {
            auto ev = reinterpret_cast<GameStatusEvent*>(event);
            writeLine(std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"gameStatus\",\"status\":{}}}", t, (int)ev->newStatus));
            break;
        }
        case BF_VoteEvent: {
            auto ev = reinterpret_cast<VoteEvent*>(event);
            writeLine(std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"vote\",\"action\":{},\"type\":{},\"pid\":{},\"target\":{},\"yes\":{},\"required\":{}}}",
                t, (uint32_t)ev->action, (uint32_t)ev->type, ev->playerID, ev->target, ev->yesCount, ev->votesRequired));
            break;
        }
        case BF_WelcomeMsgEvent: {
            auto ev = reinterpret_cast<WelcomeMsgEvent*>(event);
            size_t len = ev->length > sizeof(ev->message) ? sizeof(ev->message) : ev->length;
            writeLine(std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"welcome\",\"id\":{},\"text\":\"{}\"}}",
                t, ev->id, jsonEscape(ev->message, len)));
            break;
        }
        case BF_SpecialGameEvent: {
            auto ev = reinterpret_cast<SpecialGameEvent*>(event);
            writeLine(std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"special\",\"action\":{}}}", t, ev->action));
            break;
        }
        case BF_DataBaseCompleteEvent:
            writeLine(std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"dbComplete\"}}", t));
            break;
        case BF_DestroyObjectEvent: {
            auto ev = reinterpret_cast<DestroyObjectEvent*>(event);
            writeLine(std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"destroyObject\",\"netId\":{}}}", t, ev->objectNetId));
            break;
        }
        case BF_PlayerControlObjectEvent: {
            auto ev = reinterpret_cast<PlayerControlObjectEvent*>(event);
            writeLine(std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"control\",\"pid\":{},\"netId\":{}}}", t, ev->playerID, ev->objectNetId));
            break;
        }
        case BF_EnterVehicleEvent: {
            auto ev = reinterpret_cast<EnterVehicleEvent*>(event);
            writeLine(std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"enterVehicle\",\"pid\":{},\"netId\":{}}}", t, ev->playerID, ev->vehicleNetId));
            break;
        }
        case BF_ExitVehicleEvent: {
            auto ev = reinterpret_cast<ExitVehicleEvent*>(event);
            if (!ev->flag) {
                writeLine(std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"exitVehicle\",\"pid\":{}}}", t, ev->playerID));
            }
            break;
        }
        case BF_PickupKitEvent:
            writeLine(pickupKitLine(event, t));
            break;
        case BF_CreateObjectEvent:
            writeLine(createObjectLine(event, t));
            break;
        case BF_SetLevelEvent:
            writeLine(setLevelLine(event, t));
            break;
        case BF_HUDTextEvent:
        case BF_CreateStaticObjectEvent:
        case BF_UpdateStaticObjectEvent:
            // bf42plus extensions and HUD text, not replay material.
            break;
        default: {
            size_t size = eventSize(type);
            std::string decoded = size ? decodeEvent(type, payload, size, t) : std::string();
            if (!decoded.empty()) {
                writeLine(decoded);
                break;
            }
            logUnknownEventDebug(type, payload, size, t);
            if (size) {
                writeLine(std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"raw\",\"type\":{},\"size\":{}{},\"raw\":\"{}\"}}",
                    t, type, size, rawEventExtraFields(type, payload, size), hexDump(payload, size - 12)));
            }
            else {
                writeLine(std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"raw\",\"type\":{},\"size\":null,\"raw\":\"{}\"}}",
                    t, type, hexDump(payload, 48)));
            }
            break;
        }
    }
}

void replay_onFire(int playerId, bool isPrimary, bool isSecondary)
{
    if (!g_file) return;
    const double t = replayNow();
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
    if (!g_file || !fireArms || !m) return;
    FireRecord r;
    if (!readFire(fireArms, player, m, r)) return;
    // Rows a, b, c of Mat4 are the X, Y, Z axes; a round leaves along +Z.
    writeLine(std::format("{{\"k\":\"f\",\"t\":{:.3f},\"id\":{},\"pid\":{},\"w\":\"{}\",\"p\":[{:.2f},{:.2f},{:.2f}],\"d\":[{:.3f},{:.3f},{:.3f}]{}}}",
        replayNow(), r.rootNetId, r.playerId, jsonEscape(r.weapon, strnlen(r.weapon, sizeof(r.weapon))),
        r.m.position.x, r.m.position.y, r.m.position.z, r.m.c.x, r.m.c.y, r.m.c.z,
        r.local ? ",\"local\":1" : ""));
}

static void* s_replayOnFireArms = (void*)&replay_onFireArms;

void replay_hook_init()
{
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

// ---------------------------------------------------------------------------
// Sampler
// ---------------------------------------------------------------------------

// The fire and altfire flags the server replicates on every BFPlayer (+0x184,
// +0x185; written by the BFPlayer networkable's setNetUpdate at 0x00407B9B and
// 0x00407BAF, read by GameClient's per-tick fire loop at 0x004B9143): the
// server's trigger timeline for every player, the bots' included.
static int playerTriggers(BFPlayer* player)
{
    uint8_t flags[2] = {};
    if (safeCopy(flags, (const uint8_t*)player + 0x184, sizeof(flags)) != sizeof(flags)) return 0;
    return (flags[0] ? 1 : 0) | (flags[1] ? 2 : 0);
}

static void samplePlayers(double t)
{
    auto players = BFPlayer::getPlayers();
    if (!players) return;

    std::string out;
    for (auto it = players->begin(); it != players->end(); ++it) {
        BFPlayer* player = *it;
        if (!player) continue;
        PlayerState s;
        s.team = player->getTeam();
        s.vehicleNetId = -1;
        s.triggers = playerTriggers(player);
        int rootNetId = -1;
        if (auto vehicle = player->getVehicle()) {
            if (auto net = vehicle->getNetworkable()) s.vehicleNetId = net->getID();
            rootNetId = rootNetIdOf(vehicle);
        }
        int pid = player->getId();
        auto prev = g_players.find(pid);
        if (prev != g_players.end() && prev->second.team == s.team && prev->second.vehicleNetId == s.vehicleNetId
            && prev->second.triggers == s.triggers) continue;
        g_players[pid] = s;
        if (!out.empty()) out += ',';
        // v4: the hull the controlled object hangs under, and the seat's place
        // after it (a hull's nested PlayerControlObjects take the ids after
        // its own; 0 is the root seat, and the soldier and camera are roots).
        const int seat = rootNetId >= 0 && s.vehicleNetId >= rootNetId ? s.vehicleNetId - rootNetId : 0;
        out += std::format("[{},{},{},{},{},{}]", pid, s.team, s.vehicleNetId, rootNetId, seat, s.triggers);
    }
    if (!out.empty()) writeLine(std::format("{{\"k\":\"p\",\"t\":{:.3f},\"p\":[{}]}}", t, out));
}

static void sampleControlPoints(double t)
{
    auto& controlPoints = ObjectManager_getControlPointVector();
    for (auto it = controlPoints.begin(); it != controlPoints.end(); ++it) {
        auto cp = *it;
        if (!cp) continue;
        int id = cp->getID();
        int team = *(int*)((uintptr_t)cp + 0x190);   // same offset renderer.cpp reads for the 3D map
        auto prev = g_controlPoints.find(id);
        if (prev != g_controlPoints.end() && prev->second.team == team) continue;
        bool isNew = prev == g_controlPoints.end();
        g_controlPoints[id].team = team;
        auto& pos = cp->getAbsolutePosition();
        if (isNew) {
            auto& cpName = *(bfs::string*)((uintptr_t)cp->getTemplate() + 0x2D0);
            writeLine(std::format("{{\"k\":\"cp\",\"t\":{:.3f},\"id\":{},\"name\":\"{}\",\"tmpl\":\"{}\",\"pos\":[{:.2f},{:.2f},{:.2f}],\"team\":{}}}",
                t, id, jsonEscape(std::string(cpName)), jsonEscape(std::string(cp->getTemplate()->getName())),
                pos.x, pos.y, pos.z, team));
        }
        else {
            writeLine(std::format("{{\"k\":\"cp\",\"t\":{:.3f},\"id\":{},\"team\":{}}}", t, id, team));
        }
    }
}

static void sampleObjects(double t)
{
    for (auto& kv : g_objects) kv.second.seenThisSample = false;

    std::string out;
    std::string armorOut;
    auto addArmor = [&armorOut](uint16_t netId, const ArmorFields& armor) {
        if (!armorOut.empty()) armorOut += ',';
        armorOut += std::format("[{},{:.1f},{}]", netId, armor.hitPoints, armor.lastHitPlayer);
    };
    auto& allObjs = ObjectManager_getAllRegisteredObjects();
    for (auto node = allObjs.head->left; node != allObjs.head; node = node->next()) {
        auto obj = node->pair.second;
        if (!obj) continue;
        // Root objects only (children ride with their parent), skip disabled ones.
        uint32_t flags = obj->getFlags();
        if (!(flags & 0x02000000) || (flags & 1)) continue;
        auto net = obj->getNetworkable();
        if (!net) continue;

        uint16_t netId = net->getID();
        auto& m = obj->getAbsoluteTransformation();
        Pos3 pos(m.position.x, m.position.y, m.position.z);
        Quat rot = toQuat(m);
        ArmorFields armor{};
        const bool hasArmor = readArmor(obj, armor);

        auto prev = g_objects.find(netId);
        if (prev == g_objects.end()) {
            auto tmpl = obj->getTemplate();
            // Max hit points and the critical-damage threshold are fixed per
            // object, so they ride on the first-seen record.
            std::string armorFields = hasArmor
                ? std::format(",\"maxhp\":{:.1f},\"crit\":{:.1f}", armor.maxHitPoints, armor.criticalDamage)
                : std::string();
            writeLine(std::format("{{\"k\":\"o\",\"t\":{:.3f},\"id\":{},\"gid\":{},\"tmpl\":\"{}\",\"tid\":{},\"team\":{}{}}}",
                t, netId, obj->getID(), tmpl ? jsonEscape(std::string(tmpl->getName())) : std::string(),
                tmpl ? tmpl->getId() : 0, obj->getTeam(), armorFields));
            g_objects[netId] = ObjectState{ pos, rot, -1, true, hasArmor, armor.hitPoints, armor.lastHitPlayer };
            if (!out.empty()) out += ',';
            out += std::format("[{},{:.2f},{:.2f},{:.2f},{:.3f},{:.3f},{:.3f},{:.3f}]",
                netId, pos.x, pos.y, pos.z, rot.x, rot.y, rot.z, rot.w);
            if (hasArmor) addArmor(netId, armor);
            continue;
        }

        auto& s = prev->second;
        s.seenThisSample = true;
        // Hit points change on every hit, and fall steadily on their own once
        // below the critical-damage threshold (burning), so this is also the
        // record of smoke, fire and the moment of destruction.
        if (hasArmor && (!s.hasArmor || fabsf(armor.hitPoints - s.hitPoints) > 0.05f || armor.lastHitPlayer != s.lastHitPlayer)) {
            s.hasArmor = true;
            s.hitPoints = armor.hitPoints;
            s.lastHitPlayer = armor.lastHitPlayer;
            addArmor(netId, armor);
        }
        if (!changed(s.pos, pos, 0.01f) && !changed(s.rot, rot, 0.002f)) continue;
        s.pos = pos;
        s.rot = rot;
        if (!out.empty()) out += ',';
        out += std::format("[{},{:.2f},{:.2f},{:.2f},{:.3f},{:.3f},{:.3f},{:.3f}]",
            netId, pos.x, pos.y, pos.z, rot.x, rot.y, rot.z, rot.w);
    }

    if (!out.empty()) writeLine(std::format("{{\"k\":\"s\",\"t\":{:.3f},\"o\":[{}]}}", t, out));
    if (!armorOut.empty()) writeLine(std::format("{{\"k\":\"a\",\"t\":{:.3f},\"a\":[{}]}}", t, armorOut));

    // Objects that disappeared since the last sample.
    for (auto it = g_objects.begin(); it != g_objects.end();) {
        if (!it->second.seenThisSample) {
            writeLine(std::format("{{\"k\":\"d\",\"t\":{:.3f},\"id\":{}}}", t, it->first));
            it = g_objects.erase(it);
        }
        else ++it;
    }
}

// ---------------------------------------------------------------------------
// Moving parts, engines and soldiers' bodies (format v4)
// ---------------------------------------------------------------------------
//
// What the server replicates beyond a root's transform, read where the client
// has applied it (bfstats features/round-replay-capture/README.md §15):
//
//   RotationalBundle (vtable 0x008FE1B0): a turret, a gun base, a hull MG's
//     mount. Its RotationalBundleNetworkable carries the bundle's angles, and
//     RotationalBundle::setState 0x0057D470 turns them into the object's
//     relative transform every frame on the client, so its absolute rotation
//     against its root's is the traverse and elevation.
//   Engine (vtable 0x008FE4A0): the EngineNetworkable's running flag (+0x15C),
//     disabled flag (+0x15D) and throttle servo angle (+0x124), and on its
//     PhysicsEngine (+0x60, vtable 0x008FDEC0) the revs (+0xA0) -- what the
//     engine note is played from -- and the gear (+0xBC).
//   BFSoldier (vtable 0x008EB128): the BFSoldierNetworkable's aim pitch
//     (+0x2B0) and torso twist (+0x2B4), the lower and upper body animation
//     machines' state indices (+0x2E0, +0x324), the held item (+0x3E8) and the
//     state bits (+0x416). A state's name and flags come out of the global
//     state table (*0x009C9664, the one getCurrentStateFlags 0x00613440
//     reads), written once per file: stance is the lower state's flags (0x20
//     crouching, 0x40 lying), firing and reloading the upper state's name.
//
// Every vtable slot and code signature is checked once before any of it is
// read (partsVerified), and every read goes through safeCopy, so another build
// of the game records v3's objects and nothing of this.

static const uintptr_t VT_ROTATIONAL_BUNDLE = 0x008FE1B0;
static const uintptr_t VT_ENGINE = 0x008FE4A0;
static const uintptr_t VT_PHYSICS_ENGINE = 0x008FDEC0;
static const uintptr_t VT_SOLDIER = 0x008EB128;
static const uintptr_t ANIM_STATE_TABLE = 0x009C9664;

template <class T>
static T readAt(const void* base, size_t offset)
{
    T value{};
    safeCopy(&value, (const uint8_t*)base + offset, sizeof(T));
    return value;
}

static uintptr_t vtableOf(const void* obj)
{
    return readAt<uintptr_t>(obj, 0);
}

static bool partsVerified()
{
    static int verified = -1;
    if (verified >= 0) return verified == 1;
    struct Slot { uintptr_t vtable; int slot; uintptr_t target; };
    static const Slot slots[] = {
        { VT_ROTATIONAL_BUNDLE, 20, 0x0057D010 },   // RotationalBundle::handleUpdate
        { VT_ROTATIONAL_BUNDLE, 15, 0x004A2250 },   // getAbsoluteTransformation
        { VT_ENGINE, 20, 0x0057E1D0 },              // Engine::handleUpdate
        { VT_PHYSICS_ENGINE, 36, 0x0057BFB0 },      // PhysicsEngine::updatePhysics
        { VT_SOLDIER, 37, 0x00500190 },
        { VT_SOLDIER, 15, 0x004A2250 },
    };
    struct Code { uintptr_t at; uint8_t bytes[16]; size_t length; };
    static const Code code[] = {
        // Engine::handleUpdate's T1 = [esi+124h] / maxRotation.z
        { 0x0057E296, { 0xD9, 0x86, 0x24, 0x01, 0x00, 0x00, 0xD8, 0x70, 0x08 }, 9 },
        // PhysicsEngine::updatePhysics reading the revs at +0xA0
        { 0x0057BFBE, { 0xD9, 0x86, 0xA0, 0x00, 0x00, 0x00 }, 6 },
        // getCurrentStateFlags: [machine+20h], then the table at 0x009C9664
        { 0x00613440, { 0x8B, 0x49, 0x20, 0x85, 0xC9, 0x7D, 0x03, 0x33, 0xC0, 0xC3, 0x8B, 0x15, 0x64, 0x96, 0x9C, 0x00 }, 16 },
    };
    bool ok = true;
    for (auto& s : slots) {
        if (readAt<uintptr_t>((void*)s.vtable, 4 * s.slot) != s.target) ok = false;
    }
    for (auto& c : code) {
        uint8_t have[16] = {};
        if (safeCopy(have, (void*)c.at, c.length) != c.length || memcmp(have, c.bytes, c.length) != 0) ok = false;
    }
    verified = ok ? 1 : 0;
    if (!ok) debuglogt("replay: vehicle parts, engines and soldier state do not match this BF1942.exe, not recorded\n");
    return ok;
}

static IObject* rootObjectOf(IObject* obj)
{
    IObject* root = obj;
    for (int depth = 0; root && root->getParent() && depth < 32; depth++) root = root->getParent();
    return root;
}

// A child object's id in this file, by the object: `first` when it is new --
// first seen, or an address the allocator has handed to an object under
// another root or of another template.
static int childIdOf(const void* obj, uint16_t root, const void* tmpl, bool& first)
{
    auto it = g_childIds.find(obj);
    first = it == g_childIds.end() || it->second.root != root || it->second.tmpl != tmpl;
    if (!first) return it->second.id;
    const int id = g_nextChildId++;
    g_childIds[obj] = ChildId{ id, root, tmpl };
    return id;
}

// A point in `m`'s own frame: its offset from m's position on m's rows, the X,
// Y and Z axes toQuat reads.
static Pos3 localPosition(const Mat4& m, const Pos3& p)
{
    const float dx = p.x - m.position.x, dy = p.y - m.position.y, dz = p.z - m.position.z;
    return Pos3(dx * m.a.x + dy * m.a.y + dz * m.a.z,
                dx * m.b.x + dy * m.b.y + dz * m.b.z,
                dx * m.c.x + dy * m.c.y + dz * m.c.z);
}

static Quat quatConjugate(const Quat& q) { return Quat{ -q.x, -q.y, -q.z, q.w }; }

static Quat quatMultiply(const Quat& a, const Quat& b)
{
    return Quat{
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

// An MSVC 7 std::string (bfs::string's layout: allocator, 16-byte buffer or
// pointer, length, capacity) copied out without calling into the engine.
static std::string readEngineString(const void* at)
{
    struct Raw { uint32_t allocator; char buffer[16]; uint32_t length; uint32_t capacity; } raw{};
    if (safeCopy(&raw, at, sizeof(raw)) != sizeof(raw) || raw.length > 256) return std::string();
    char text[257] = {};
    const void* chars = raw.capacity >= 16 ? *(const char* const*)raw.buffer : (const void*)((const uint8_t*)at + 4);
    if (safeCopy(text, chars, raw.length) != raw.length) return std::string();
    return jsonEscape(text, strnlen(text, raw.length));
}

// The animation state table, once per file: index, name, flags.
static void writeAnimStates(double t)
{
    uintptr_t table = readAt<uintptr_t>((void*)ANIM_STATE_TABLE, 0);
    if (!table) return;
    uintptr_t begin = readAt<uintptr_t>((void*)table, 0x18);
    uintptr_t end = readAt<uintptr_t>((void*)table, 0x1C);
    if (!begin || end < begin || (end - begin) / 4 > 4096) return;
    std::string out;
    for (uintptr_t at = begin; at < end; at += 4) {
        uintptr_t state = readAt<uintptr_t>((void*)at, 0);
        if (!state) continue;
        const int index = (int)((at - begin) / 4);
        if (!out.empty()) out += ',';
        out += std::format("[{},\"{}\",{}]", index, readEngineString((void*)(state + 0x130)), readAt<uint32_t>((void*)state, 0x2C));
    }
    writeLine(std::format("{{\"k\":\"anim\",\"t\":{:.3f},\"states\":[{}]}}", t, out));
    g_animStatesWritten = true;
}

// ---------------------------------------------------------------------------
// Tickets
// ---------------------------------------------------------------------------
//
// The client's ScoreManager (the pointer at 0x0097A0D8, created by the class
// factory with class id 0xC4B8 at 0x004830C0; vtable 0x008DAEB0) is the
// server's replicated through ScoreManager::setNetUpdate. Its getTeamScore
// (vtable +0x10, 0x0049FD10) is `this + 0x10 + team x 0x50` for team 1..2,
// as on the server (lnxded 0x081616C0), and a TeamScore's live count is +0x48
// (lnxded TeamScore::setTickets 0x081610A0): what the HUD draws, read at
// 0x004A8C2F and ten more places the same way.

static const uintptr_t SCORE_MANAGER_PTR = 0x0097A0D8;
static const uintptr_t VT_SCORE_MANAGER = 0x008DAEB0;

static void sampleTickets(double t)
{
    static int verified = -1;
    if (verified < 0) {
        verified = readAt<uintptr_t>((void*)VT_SCORE_MANAGER, 0x10) == 0x0049FD10 ? 1 : 0;
        if (!verified) debuglogt("replay: ScoreManager does not match this BF1942.exe, tickets not recorded\n");
    }
    if (verified != 1) return;
    const uintptr_t manager = readAt<uintptr_t>((void*)SCORE_MANAGER_PTR, 0);
    if (!manager || vtableOf((void*)manager) != VT_SCORE_MANAGER) return;
    const int team1 = readAt<int>((void*)manager, 0x10 + 1 * 0x50 + 0x48);
    const int team2 = readAt<int>((void*)manager, 0x10 + 2 * 0x50 + 0x48);
    if (team1 == g_tickets[0] && team2 == g_tickets[1]) return;
    g_tickets[0] = team1;
    g_tickets[1] = team2;
    writeLine(std::format("{{\"k\":\"tk\",\"t\":{:.3f},\"v\":[{},{}]}}", t, team1, team2));
}

static void sampleParts(double t)
{
    if (!partsVerified()) return;
    std::string joints, jointNames, engines, soldiers;
    auto& allObjs = ObjectManager_getAllRegisteredObjects();
    for (auto node = allObjs.head->left; node != allObjs.head; node = node->next()) {
        auto obj = node->pair.second;
        if (!obj) continue;
        const uint32_t flags = obj->getFlags();
        if (flags & 1) continue;   // disabled
        auto net = obj->getNetworkable();
        if (!net) continue;
        const uintptr_t vt = vtableOf(obj);
        const bool root = (flags & 0x02000000) != 0;

        if (root && vt == VT_SOLDIER) {
            SoldierState s{
                readAt<int>(obj, 0x2E0), readAt<int>(obj, 0x324), readAt<uint8_t>(obj, 0x3E8),
                readAt<uint16_t>(obj, 0x416), readAt<float>(obj, 0x2B0), readAt<float>(obj, 0x2B4),
            };
            const uint16_t id = net->getID();
            auto prev = g_soldiers.find(id);
            if (prev != g_soldiers.end() && prev->second.lower == s.lower && prev->second.upper == s.upper
                && prev->second.item == s.item && prev->second.bits == s.bits
                && fabsf(prev->second.pitch - s.pitch) < 1.0f && fabsf(prev->second.twist - s.twist) < 1.0f) continue;
            g_soldiers[id] = s;
            if (!soldiers.empty()) soldiers += ',';
            soldiers += std::format("[{},{},{},{:.1f},{:.1f},{},{}]", id, s.lower, s.upper, s.pitch, s.twist, s.item, s.bits);
            continue;
        }
        if (root) continue;

        if (vt == VT_ROTATIONAL_BUNDLE) {
            IObject* top = rootObjectOf(obj);
            auto topNet = top ? top->getNetworkable() : nullptr;
            if (!topNet || top == obj) continue;
            const Mat4& topM = top->getAbsoluteTransformation();
            const Mat4& partM = obj->getAbsoluteTransformation();
            const Quat rel = quatMultiply(quatConjugate(toQuat(topM)), toQuat(partM));
            const uint16_t rootId = (uint16_t)topNet->getID();
            auto tmpl = obj->getTemplate();
            bool first = false;
            const int id = childIdOf(obj, rootId, tmpl, first);
            if (first) {
                // Named on first sight, with where it sits on its root: the
                // viewer finds the part in the root's model by its template's
                // name, and among same-named parts (a ship's AA guns) by place.
                const Pos3 at = localPosition(topM, partM.position);
                if (!jointNames.empty()) jointNames += ',';
                jointNames += std::format("[{},{},\"{}\",{:.2f},{:.2f},{:.2f}]", rootId, id,
                    tmpl ? jsonEscape(std::string(tmpl->getName())) : std::string(), at.x, at.y, at.z);
            }
            else {
                auto prev = g_parts.find(id);
                if (prev != g_parts.end() && !changed(prev->second.rel, rel, 0.002f)) continue;
            }
            g_parts[id] = PartState{ rel };
            if (!joints.empty()) joints += ',';
            joints += std::format("[{},{},{:.4f},{:.4f},{:.4f},{:.4f}]", rootId, id, rel.x, rel.y, rel.z, rel.w);
            continue;
        }

        if (vt == VT_ENGINE) {
            uintptr_t pe = readAt<uintptr_t>(obj, 0x60);
            if (!pe || vtableOf((void*)pe) != VT_PHYSICS_ENGINE) continue;
            IObject* top = rootObjectOf(obj);
            auto topNet = top ? top->getNetworkable() : nullptr;
            if (!topNet) continue;
            EngineState e{
                readAt<float>((void*)pe, 0xA0), readAt<float>(obj, 0x124),
                (readAt<uint8_t>(obj, 0x15C) ? 1 : 0) | (readAt<uint8_t>(obj, 0x15D) ? 2 : 0),
                readAt<int>((void*)pe, 0xBC),
            };
            const uint16_t rootId = (uint16_t)topNet->getID();
            bool first = false;
            const int id = childIdOf(obj, rootId, obj->getTemplate(), first);
            auto prev = g_engines.find(id);
            if (!first && prev != g_engines.end() && fabsf(prev->second.revs - e.revs) < 0.01f
                && fabsf(prev->second.throttle - e.throttle) < 0.01f && prev->second.flags == e.flags
                && prev->second.gear == e.gear) continue;
            g_engines[id] = e;
            if (!engines.empty()) engines += ',';
            engines += std::format("[{},{:.3f},{:.3f},{},{},{}]", rootId, e.revs, e.throttle, e.flags, e.gear, id);
        }
    }
    if (!soldiers.empty() && !g_animStatesWritten) writeAnimStates(t);
    if (!jointNames.empty()) writeLine(std::format("{{\"k\":\"jn\",\"t\":{:.3f},\"o\":[{}]}}", t, jointNames));
    if (!joints.empty()) writeLine(std::format("{{\"k\":\"j\",\"t\":{:.3f},\"o\":[{}]}}", t, joints));
    if (!engines.empty()) writeLine(std::format("{{\"k\":\"g\",\"t\":{:.3f},\"o\":[{}]}}", t, engines));
    if (!soldiers.empty()) writeLine(std::format("{{\"k\":\"st\",\"t\":{:.3f},\"o\":[{}]}}", t, soldiers));
}

void replay_onFrame()
{
    if (!g_settings.recordReplays) {
        if (g_file) replay_stop();
        return;
    }
    if (!g_file && !replay_start()) return;

    double t = replayNow();
    if (g_lastSampleTime >= 0.0 && t - g_lastSampleTime < 1.0 / SAMPLE_HZ) return;
    g_lastSampleTime = t;

    sampleObjects(t);
    sampleParts(t);
    sampleTickets(t);
    samplePlayers(t);
    sampleControlPoints(t);
}

// Chat as the client displays it. The server never sends players their own
// chat back, so ChatFragment events miss everything the recording player says;
// this catches every line on its way to the chat box instead.
void replay_onChat(const wchar_t* text, size_t length, int playerId, int team)
{
    if (!g_settings.recordReplays || !text) return;
    if (!g_file && !replay_start()) return;
    std::string iso = WideStringToISO88591(std::wstring(text, length));
    writeLine(std::format("{{\"k\":\"chat\",\"t\":{:.3f},\"pid\":{},\"team\":{},\"text\":\"{}\"}}",
        replayNow(), playerId, team, jsonEscape(iso)));
}
