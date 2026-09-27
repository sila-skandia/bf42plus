#pragma once

#include <string>
#include <vector>

#include "util.h"

#include "bf/ui.h"
#include "bf/stl.h"

#include "SimpleIni.h"


struct Setting {
    const wchar_t* section;
    const wchar_t* name;
    const wchar_t* comment;
    int resourceid;
    bool dirty = false;
    virtual void load(const CSimpleIni& ini) = 0;
    virtual void save(CSimpleIni& ini) = 0;
protected:
    Setting(const wchar_t* section, const wchar_t* name, const wchar_t* comment, int resourceid) :
        section(section), name(name), comment(comment), resourceid(resourceid) {};
};

struct StringSetting : public Setting {
    std::wstring value;
    StringSetting(const wchar_t* section, const wchar_t* name, const wchar_t* comment, int resourceid, std::wstring value) :
        Setting(section, name, comment, resourceid), value(value) {};
    virtual void load(const CSimpleIni& ini);
    virtual void save(CSimpleIni& ini);
};

struct BoolSetting : public Setting {
    bool value;
    BoolSetting(const wchar_t* section, const wchar_t* name, const wchar_t* comment, int resourceid, bool value) :
        Setting(section, name, comment, resourceid), value(value) {};
    virtual void load(const CSimpleIni& ini);
    virtual void save(CSimpleIni& ini);
    operator bool() const { return value; };
};

struct IntSetting : public Setting {
    int value;
    IntSetting(const wchar_t* section, const wchar_t* name, const wchar_t* comment, int resourceid, int value) :
        Setting(section, name, comment, resourceid), value(value) {};
    virtual void load(const CSimpleIni& ini);
    virtual void save(CSimpleIni& ini);
    operator int() const { return value; };
};

struct FloatSetting : public Setting {
    double value;
    FloatSetting(const wchar_t* section, const wchar_t* name, const wchar_t* comment, int resourceid, double value) :
        Setting(section, name, comment, resourceid), value(value) {};
    virtual void load(const CSimpleIni& ini);
    virtual void save(CSimpleIni& ini);
    operator int() const { return value; };
};

struct ColorSetting : public Setting {
    uint32_t value;
    ColorSetting(const wchar_t* section, const wchar_t* name, const wchar_t* comment, int resourceid, uint32_t value) :
        Setting(section, name, comment, resourceid), value(value) {};
    virtual void load(const CSimpleIni& ini);
    virtual void save(CSimpleIni& ini);
    operator uint32_t() const { return value; };
};

// space separated list of colors
struct ColorListSetting : public Setting {
    std::vector<uint32_t> value;
    ColorListSetting(const wchar_t* section, const wchar_t* name, const wchar_t* comment, int resourceid, std::initializer_list<uint32_t> values) :
        Setting(section, name, comment, resourceid), value(values) {};
    virtual void load(const CSimpleIni& ini);
    virtual void save(CSimpleIni& ini);
};

struct EnumSetting : public Setting {
    std::vector<std::string> possibleValues;
    std::string value;
    EnumSetting(const wchar_t* section, const wchar_t* name, const wchar_t* comment, int resourceid, const std::string& value, std::initializer_list<std::string> possibleValues) :
        Setting(section, name, comment, resourceid), value(value), possibleValues(possibleValues) {};
    virtual void load(const CSimpleIni& ini);
    virtual void save(CSimpleIni& ini);
    bool isValidValue(std::string value) const { return std::ranges::find(possibleValues.begin(), possibleValues.end(), value) != possibleValues.end(); };
    const std::vector<std::string>& getPossibleValues() const { return possibleValues; };
};

class Settings {
    CSimpleIni ini;
    std::vector<Setting*> settings;
public:
    Settings();
    bool load();
    bool save(bool force);
    // Store a buddy color in the settings file.
    // Buddy colors are stored in a separate [buddycolors] section.
    // If color is set to InvalidColor, the entry is removed for the given player name.
    void setBuddyColor(const std::wstring& name, uint32_t color) {
        if (color != InvalidColor) ini.SetValue(L"buddycolors", name.c_str(), ISO88591ToWideString(GetStringFromColor(color)).c_str(), 0, true);
        else ini.Delete(L"buddycolors", name.c_str(), false);
    };

    void ignorePlayerName(const std::wstring& name) {
        if (!isPlayerNameIgnored(name)) {
            ini.SetValue(L"ignorelist", L"name", name.c_str(), 0, false);
            save(true);
        }
    };
    void unignorePlayerName(const std::wstring& name) {
        ini.DeleteValue(L"ignorelist", L"name", name.c_str());
        save(true);
    };
    bool isPlayerNameIgnored(const std::wstring& name) {
        std::list <CSimpleIni::Entry> values;
        if (ini.GetAllValues(L"ignorelist", L"name", values)) {
            for (auto& value : values) {
                if (_wcsicmp(name.c_str(), value.pItem) == 0) {
                    return true;
                }
            }
        }
        return false;
    };

    BoolSetting showConnectsInChat = {
        L"general", L"showConnectsInChat",
        L"; Show a message in the status chat when a player connects to the server",
        0, false };
    BoolSetting showIDInChat = {
        L"general", L"showIDInChat",
        L"; Show player IDs in chat messages, name changes and joins/disconnects",
        0, false };
    BoolSetting showIDInKills = {
        L"general", L"showIDInKills",
        L"; Show player IDs in kill messages",
        0, false };
    BoolSetting showIDInNametags = {
        L"general", L"showIDInNametags",
        L"; Show player IDs in nametags",
        0, false };
    BoolSetting showVoteInConsole = {
        L"general", L"showVoteInConsole",
        L"; Show who starts a vote or votes in the console",
        0, false };
    BoolSetting lowerNametags = {
        L"general", L"lowerNametags",
        L"; Gradually lower nametags when u get closer to other players",
        0, true };
    ColorSetting debugTextColor = {
        L"general", L"debugTextColor",
        L"; Sets the color of console.showStats, console.showFPS, etc",
        0, 0xffff00 /*yellow*/ };
    BoolSetting smootherGameplay = {
        L"general", L"smootherGameplay",
        L"; Enables some patches that make the game run smoother. This\n"
        L"; may have a very small performance impact on older systems.\n"
        L"; The patches affect the floating point precision the game runs\n"
        L"; with. When playing in multiplayer the time it takes for the\n"
        L"; server to process your input will be decreased.",
        0, true };
    ColorListSetting presetBuddyColors = {
        L"general", L"presetBuddyColors",
        L"; Sets the colors chosen when the ADD BUDDY button is repeatedly clicked on the scoreboard",
        0, { DefaultBuddyColor /*lime*/, 0x1e90ff /*dodgerblue*/, 0xffff00 /*yellow*/, 0x8a2be2 /*blueviolet*/} };
    BoolSetting correctedLookSensitivity = {
        L"general", L"correctedLookSensitivity",
        L"; The game has higher look left/right sensitivity when you are on-foot and moving\n"
        L"; This option enables a workaround that makes the on-foot sensitivity to always be the same\n"
        L"; WARNING: Enabling this may feel unusual because of the decreased sensitivity!",
        0, false };
    BoolSetting stationaryMGInfSensitivity = {
        L"general", L"stationaryMGInfSensitivity",
        L"; Enables scaling of the mouse sensitivity in stationary MG42/Browning to be the same\n"
        L"; as the infantry sensitivity. Affects PCOs with category VCLand and type VTStationaryMG.",
        0, false };
    BoolSetting dontBlockInputDuringFreeLook = {
        L"general", L"dontBlockInputDuringFreeLook",
        L"; In aircraft, the freelook button normally blocks pitch, roll and yaw\n"
        L"; entirely, so only the throttle still responds while you look around.\n"
        L"; This option keeps your keyboard and joystick flight axes working during\n"
        L"; freelook. The mouse is deliberately left out, so looking around\n"
        L"; never steers the plane at the same time.\n"
        L"; In multiplayer your client stops reporting freelook while you are\n"
        L"; flying, so the look axes stay local and the server has no means to tell\n"
        L"; which way a pilot is looking, that's the only tradeoff.",
        0, true };
    BoolSetting enable3DMineMap = {
        L"general", L"enable3DMineMap",
        L"; Enable 3D map showing friendly mines. In multiplayer it is only enabled if the server allows it.",
        0, true };
    BoolSetting enable3DSupplyMap = {
        L"general", L"enable3DSupplyMap",
        L"; Enable 3D map showing heal, ammo, repair points. In multiplayer it is only enabled if the server allows it.",
        0, true };
    BoolSetting enable3DControlPointMap = {
        L"general", L"enable3DControlPointMap",
        L"; Enable 3D map showing controlpoints. In multiplayer it is only enabled if the server allows it.",
        0, true };
    BoolSetting fasterMapchange = {
        L"general", L"fasterMapchange",
        L"; Restart the game faster when the map is changing.",
        0, true };
    IntSetting wrapChat = {
        L"general", L"wrapChat",
        L"; Wrap chat messages if longer than this value. Set to 0 to disable. A good value is 42.\n"
        L"; Also increasing chat lines from 4 to 6 by typing 'chattext 6' in console is recommended.",
        0, 0 };
    EnumSetting screenshotFormat = {
        L"general", L"screenshotFormat",
        L"; File format for screenshots. Possible values: png jpg",
        0, "png", {"png", "jpg"} };
    FloatSetting hitIndicatorTime = {
        L"general", L"hitIndicatorTime",
        L"; Sets the time it takes for the hit indicator to disappear. Default is 1 second, 0 disables the indicator.",
        0, 1.0 };
    BoolSetting fixAnimatedMeshLighting = {
        L"general", L"fixAnimatedMeshLighting",
        L"; Fixes dynamic lighting on animated meshes (soldiers, vehicles). Normally their\n"
        L"; normals are frozen in the pose the mesh was built in, so the lit side of the mesh\n"
        L"; follows the animation around instead of staying towards the sun.\n"
        L"; The matching vertex shader is built into the mod, shaders.rfa is left alone.",
        0, true };
    BoolSetting disableArchiveOnlyMode = {
        L"general", L"disableArchiveOnlyMode",
        L"; Allows the game to load unpacked archive data from game dir like BF1942_r executable does.",
        0, false };
    FloatSetting maxTimeToBusyWait = {
        L"general", L"maxTimeToBusyWait",
        L"; The game uses busy waiting to synchronize ticks defined by game.lockFps.\n"
        L"; It puts unnecessary load on the CPU, leading to increased laptop battery drain\n"
        L"; and loud fan noises even on modern PCs (e.g. 1ms of actual work, 7ms of waiting).\n"
        L"; This setting controls the maximum time the game is allowed to spend busy-waiting.\n"
        L"; The preceding time to busy-waiting period will be be spent sleeping in 1ms steps.\n"
        L"; Decrease value to reduce CPU load.\n"
        L"; Increase it if you experience stuttering as a result.\n"
        L"; Values of <=1ms will lead to excessive sleeping and may reduce frame rate stability.\n"
        L"; Set to -1.0 for vanilla behaviour.\n"
        L"; Default value: 0.0015 (1.5ms).",
        0, 0.0015 };
    IntSetting minTimerResolution = {
        L"general", L"minTimerResolution",
        L"; Sets minimum timer resolution for maxTimeToBusyWait setting.\n"
        L"; Affects the accuracy of Sleep() function.\n"
        L"; Higher values may lead to stutters. Default: 1 (ms).",
        0, 1 };
#if defined(TRACY_ENABLE)
    BoolSetting profilerAutoStart = {
        L"general", L"profilerAutoStart",
        L"; Activates profiler automatically when game reaches main loop.\n"
        L"; Otherwise needs to be activated using console command Profiler.enable 1\n"
        L"; NOTE: bf42++_r overrides default profiler with Tracy.\n"
        L";       Compatibility with consoleProfiler is not guaranteed.",
        0, false };
#endif
    BoolSetting recordReplays = {
        L"general", L"recordReplays",
        L"; Records every game event the server sends, plus a 10 Hz sample of every\n"
        L"; networked object, to replays/replay_<timestamp>.ndjson while connected to\n"
        L"; a server. A new file is started on each join.\n"
        L"; Can also be toggled in-game with plus.recordReplays 1/0; a file begun\n"
        L"; mid-round still names the level, the server and who is playing.",
        0, false };
};

extern Settings g_settings;


class ObjectTemplate;

class ServerSettings {
public:
    struct Mine3DMap {
        bool allow = false;
        int distance = 30;
        uint32_t color = 0xff1493;
        bfs::string text = "*MINE*";
    } mine3DMap;

    struct SupplyDepot3DMap {
        bool allow = false;
        int distance = 65;
    } supplyDepot3DMap;

    struct ControlPoint3DMap {
        bool allow = false;
        int distanceRadiusFactor = 4;
    } controlPoint3DMap;

    struct Custom3DMap {
        bfs::string templateName;
        bfs::string text = "";
        int distance = 30;
        uint32_t color = 0xFFFF00;
        bool onlySameTeam = true;
        bool showDistance = false;
    };
    std::map<ObjectTemplate*, Custom3DMap> custom3DMaps;

    struct SUI {
        bool openSpawnScreenOnDeath = true;
        bool openSpawnScreenOnJoin = true;
        bool allowFasterRestart = true;
        bool skipBriefingWindow = false;
        bool showEnemyNametags = true;
    } UI;

    void parseFromText(const char* text);
};

extern ServerSettings g_serverSettings;
