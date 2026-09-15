#include "pch.h"
#include "replay.h"
#include <map>

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static const int SAMPLE_HZ = 10;
static const int FLUSH_INTERVAL_MS = 2000;
// 2: "raw" events carry "size" and exactly the payload, not a fixed 48 bytes.
// 3: hit points ("a" records, "maxhp"/"crit" on "o"), chat as displayed
//    ("chat"), template names on object-creation events, one file per join
//    instead of closing at map end.
static const int FORMAT_VERSION = 3;

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
};
static std::map<int, PlayerState> g_players;

struct ControlPointState {
    int team;
};
static std::map<int, ControlPointState> g_controlPoints;

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

    char iso[64];
    strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%S", &lt);
    writeLine(std::format("{{\"k\":\"h\",\"v\":{},\"plus\":\"{}\",\"start\":\"{}\",\"hz\":{}}}",
        FORMAT_VERSION, jsonEscape(WideStringToISO88591(get_build_version())), iso, SAMPLE_HZ));
    g_fileHasContent = false;

    debuglogt("replay: recording to %s\n", name);
    BfMenu::getSingleton()->outputConsole(std::string("plus: recording replay to ") + name);
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

void replay_onEvent(GameEvent* event)
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
        case BF_CreatePlayerEvent: {
            auto ev = reinterpret_cast<CreatePlayerEvent*>(event);
            size_t nameLen = strnlen(ev->name, sizeof(ev->name));
            writeLine(std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"createPlayer\",\"pid\":{},\"name\":\"{}\",\"team\":{},\"ai\":{},\"netId\":{},\"vehNetId\":{},\"camNetId\":{},\"kitNetId\":{}}}",
                t, ev->playerID, jsonEscape(ev->name, nameLen), ev->team, ev->isAI ? 1 : 0,
                ev->playerNetworkID, ev->vehicleNetworkID, ev->cameraNetworkID, ev->kitNetworkID));
            break;
        }
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
            writeLine(std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"score\",\"kind\":{},\"pid\":{},\"victim\":{},\"weapon\":{},\"bodypart\":{}}}",
                t, (uint32_t)ev->eventid, ev->playerid, ev->victimpid, ev->weapon, ev->bodypart));
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
        case BF_HUDTextEvent:
        case BF_CreateStaticObjectEvent:
        case BF_UpdateStaticObjectEvent:
            // bf42plus extensions and HUD text, not replay material.
            break;
        default: {
            // Unknown layout (SetLevel included): dump the whole payload so the
            // struct can be mapped from a real recording. Without a size from the
            // engine, fall back to a fixed prefix and say so with "size":null --
            // trailing bytes of that dump may lie past the end of the event.
            size_t size = eventSize(type);
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

// ---------------------------------------------------------------------------
// Sampler
// ---------------------------------------------------------------------------

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
        if (auto vehicle = player->getVehicle()) {
            if (auto net = vehicle->getNetworkable()) s.vehicleNetId = net->getID();
        }
        int pid = player->getId();
        auto prev = g_players.find(pid);
        if (prev != g_players.end() && prev->second.team == s.team && prev->second.vehicleNetId == s.vehicleNetId) continue;
        g_players[pid] = s;
        if (!out.empty()) out += ',';
        out += std::format("[{},{},{}]", pid, s.team, s.vehicleNetId);
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
