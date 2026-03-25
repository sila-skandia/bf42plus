// Ghidra headless script to apply labels from bf42plus source code
// Usage: analyzeHeadless /path/to/project ProjectName -process BF1942.exe -postScript ghidra_label.java
//@category bf42plus

import ghidra.app.script.GhidraScript;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.address.Address;

public class ghidra_label extends GhidraScript {

    private void label(long addrLong, String name) throws Exception {
        Address addr = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(addrLong);
        try {
            currentProgram.getSymbolTable().createLabel(addr, name, SourceType.USER_DEFINED);
            println(String.format("Labeled 0x%08X -> %s", addrLong, name));
        } catch (Exception e) {
            println(String.format("Failed 0x%08X -> %s: %s", addrLong, name, e.getMessage()));
        }
    }

    @Override
    public void run() throws Exception {

        // ===== Global Data Pointers =====
        label(0x0097D76CL, "pPlayerManager");
        label(0x0097D764L, "pObjectManager");
        label(0x0097D768L, "pObjectTemplateManager");
        label(0x009A99D4L, "RendPCDX8_singleton");
        label(0x009A99D8L, "g_TextureManager");
        label(0x00971EACL, "g_pSetup");
        label(0x009A2170L, "g_Locale");
        label(0x009A2320L, "g_debug_callback");
        label(0x009A9428L, "g_console_run_result");
        label(0x009AB610L, "ConsoleObjects_singleton");
        label(0x009AB868L, "pRenderView");
        label(0x00973AB8L, "BfMenu_singleton");
        label(0x0095F8D4L, "g_pIGame");
        label(0x00957C30L, "g_masterServerAddr1");
        label(0x00957DF8L, "g_masterServerAddr2");
        label(0x00A5C630L, "g_MainUI");
        label(0x00A5F1A8L, "SpawnScreen_singleton");
        label(0x009A9390L, "g_pGameClient");

        // ===== Game Functions =====
        label(0x00502E60L, "calcStringHashValueNoCase");
        label(0x00473430L, "MD5Digest");
        label(0x00581520L, "Locale__getAnsi");
        label(0x005815B0L, "Locale__getWide");
        label(0x0040F660L, "setRotation");
        label(0x0040ECB0L, "Game__addPlayerInput");
        label(0x00443F10L, "getStringFromRegistry");
        label(0x006C5840L, "getLandSeaSensitivity");
        label(0x006C5800L, "getInfantrySensitivity");
        label(0x006C5820L, "getLandSeaInvert");
        label(0x006C57E0L, "getInfantryInvert");

        // ===== Renderer =====
        label(0x004611D0L, "Renderer__drawDebugText");
        label(0x00440790L, "convertWorldPosToScreenPos");
        label(0x00543110L, "SupplyDepot__isHealing");
        label(0x005431B0L, "SupplyDepot__isRepairing");
        label(0x005431E0L, "SupplyDepot__isReloading");

        // ===== UI / BfMenu =====
        label(0x0045CE60L, "BfMenu__getLocalPlayer");
        label(0x0045D1A0L, "BfMenu__stringToWide");
        label(0x006A8F10L, "BfMenu__addGameInfoMessage");
        label(0x006A8E10L, "BfMenu__addPlayerChatMessage");
        label(0x006A8F80L, "BfMenu__addRadioChatMessage");
        label(0x006A89C0L, "BfMenu__addChatMessageInternal");
        label(0x006A88E0L, "BfMenu__setCenterKillMessage");
        label(0x006A7D00L, "BfMenu__outputConsole");
        label(0x00468680L, "BfMap__isPlayerIDInBuddyList");
        label(0x0046A2A0L, "BfMap__addPlayerToBuddyListByID");
        label(0x006A7E90L, "BfMenu__showDisconnectMessage");
        label(0x006A7EE0L, "BfMenu__hideDisconnectMessage");
        label(0x006A76B0L, "BfMenu__setInfoMessage");
        label(0x006A87D0L, "BfMenu__setServerMessage");
        label(0x006A9340L, "BfMenu__addToIgnoreList");
        label(0x006A90B0L, "BfMenu__removeFromIgnoreList");
        label(0x006A7D80L, "BfMenu__setStatusMessage");
        label(0x006A7DD0L, "BfMenu__clearStatusMessage");
        label(0x006CCBC0L, "SpawnScreen__setSpawnMessage");
        label(0x006CCE20L, "SpawnScreenStuff__setVisible");

        // ===== GameEvent System =====
        label(0x004B34B0L, "GameEventManager__getNextRcvdEvent");
        label(0x004A6290L, "GameEvent__allocate");
        label(0x004869D0L, "GameEvent__deallocate");
        label(0x004A7D70L, "GameEvent__registerEventMaker");
        label(0x004A7BD0L, "GameEvent__getEventMaker");

        // ===== GameClient Event Processing =====
        label(0x004933D0L, "GameClient__processEvent");
        label(0x00493B07L, "GameClient__processEvent_afterCreatePlayer");

        // ===== Network / BitStream =====
        label(0x00582610L, "BitStream__readBits");
        label(0x00582AB0L, "BitStream__writeBool");
        label(0x00582AF0L, "BitStream__readBool");
        label(0x005829C0L, "BitStream__writeUnsigned");
        label(0x005829E0L, "BitStream__readUnsigned");
        label(0x00582C90L, "BitStream__writeFullVector");
        label(0x00583180L, "BitStream__readFullVector");

        // ===== Console =====
        label(0x005AC970L, "GameConsole__func1");
        label(0x005ACC30L, "GameConsole__func2");
        label(0x0044ABC0L, "Console__patchTarget");

        // ===== Debug =====
        label(0x005821E0L, "Debug__setDebugCallback");
        label(0x00582470L, "turnOffAllDebug");
        label(0x005821A0L, "Debug__logStream");
        label(0x00582270L, "Debug__logStreamEnd");

        // ===== Object System =====
        label(0x00541F50L, "Projectile__resetProjectile");
        label(0x00580EB0L, "Object__getName");

        // ===== System =====
        label(0x00581D90L, "System__getMHZ");
        label(0x00601664L, "MemoryPool__alloc");
        label(0x006B0DB0L, "__changeResolution");

        // ===== Game Functions (decompiled processEvent cases) =====
        label(0x00491980L, "GameClient__getPlayerFromEvent");
        label(0x004930D0L, "GameClient__createObject");
        label(0x00491180L, "GameClient__createObject2");
        label(0x00492BC0L, "GameClient__processCase0xC");
        label(0x004B4C20L, "GameClient__processCase0x2B");
        label(0x004B3C90L, "GameClient__syncFunc");
        label(0x00499C40L, "GameClient__func_499C40");
        label(0x00499CE0L, "GameClient__func_499CE0");
        label(0x00499CF0L, "GameClient__func_499CF0");
        label(0x00490750L, "GameClient__func_490750");
        label(0x004B7B70L, "GameClient__func_4B7B70");
        label(0x004B88E0L, "GameClient__func_4B88E0");
        label(0x004B8CE0L, "GameClient__func_4B8CE0");
        label(0x004B4800L, "GameClient__func_4B4800");
        label(0x00467C10L, "GameClient__func_467C10");
        label(0x00407600L, "GameClient__func_407600");
        label(0x00407660L, "GameClient__func_407660");
        label(0x0040BCF0L, "GameClient__func_40BCF0");
        label(0x00408F80L, "GameClient__scoreFunc1");
        label(0x00408FF0L, "GameClient__scoreFunc2");
        label(0x00409060L, "GameClient__scoreFunc3");
        label(0x00406A20L, "GameClient__func_406A20");

        // ===== Spawn/Death Screen =====
        label(0x006D84A0L, "SpawnScreen__func_6D84A0");
        label(0x006D8060L, "SpawnScreen__func_6D8060");
        label(0x006D5340L, "SpawnScreen__func_6D5340");
        label(0x006D6450L, "SpawnScreen__func_6D6450");
        label(0x006D2790L, "SpawnScreen__func_6D2790");
        label(0x006D3400L, "SpawnScreen__func_6D3400");

        // ===== Loading Screen =====
        label(0x006B42F0L, "LoadingScreen__showDisconnectMessage");
        label(0x006B4910L, "LoadingScreen__func_6B4910");
        label(0x006B4030L, "LoadingScreen__func_6B4030");

        // ===== Kill/Score Messages =====
        label(0x006A9020L, "BfMenu__addKillMessage");
        label(0x006A9DE0L, "BfMenu__func_6A9DE0");
        label(0x006ACB60L, "BfMenu__func_6ACB60");

        // ===== STL / MSVC Runtime =====
        label(0x008C3520L, "msvc_sprintf");
        label(0x008C3134L, "msvc_stl_func1");
        label(0x008C3220L, "msvc_stl_func2");

        // ===== Patched Locations =====
        label(0x005389C9L, "patch__Particle_handleUpdate_crashfix");
        label(0x006DFB7EL, "patch__scoreboard_axis_col_width1");
        label(0x006DFB8BL, "patch__scoreboard_axis_col_width2");
        label(0x006DFAC0L, "patch__scoreboard_allied_col_width1");
        label(0x006DFACDL, "patch__scoreboard_allied_col_width2");
        label(0x006DFA7FL, "patch__scoreboard_allied_col_width3");
        label(0x006DFB3DL, "patch__scoreboard_axis_col_width3");
        label(0x004B6FFDL, "patch__server_ping");
        label(0x004B6F6CL, "patch__ping_interval");
        label(0x00490F4CL, "patch__GameClient_disconnect_udp");
        label(0x00490F00L, "GameClient__disconnect");
        label(0x00494DFAL, "patch__empty_maplist_fix");
        label(0x00632478L, "patch__ForegroundLockTimeout");
        label(0x004B77C7L, "patch__mine_warning_fix");
        label(0x004F85C7L, "patch__glitchy_projectile_pickup_fix");
        label(0x0048FFB6L, "patch__debug_restart_code");
        label(0x006D316DL, "patch__radio_playvoice_crash_jmp");
        label(0x006D310BL, "patch__radio_playvoice_crash");
        label(0x006CDE8AL, "patch__healthbar_fix");
        label(0x004931C5L, "patch__CreateObjectEvent_crashfix1");
        label(0x00493271L, "patch__CreateObjectEvent_crashfix2");
        label(0x004932D2L, "patch__CreateObjectEvent_crashfix3");
        label(0x004112BDL, "patch__skip_static_objects_jmp");
        label(0x00411170L, "patch__Game_load_static_objects");
        label(0x004B4045L, "patch__network_error_debug");
        label(0x004B403FL, "patch__GameEvent_invalid_id_crashfix");

        // ===== Nametag / HUD Patches =====
        label(0x00440F90L, "patch__lower_nametag_close");
        label(0x004416CBL, "patch__nametag_add_id_no_hp");
        label(0x0044153AL, "patch__nametag_add_id_with_hp");
        label(0x004411D0L, "patch__nametag_buddy_color");
        label(0x006E082AL, "patch__scoreboard_buddy_color");
        label(0x006A8A54L, "patch__chat_buddy_color");
        label(0x006DFF74L, "patch__scoreboard_add_buddy_btn");

        // ===== Spawn Screen Patches =====
        label(0x006D846BL, "patch__skip_spawn_screen_jmp");
        label(0x006D8461L, "patch__skip_spawn_screen_join");
        label(0x004946D4L, "patch__skip_spawn_screen_death_jmp");
        label(0x004946CAL, "patch__skip_spawn_screen_death");
        label(0x006CD610L, "patch__skip_spawn_status_update");
        label(0x006D8420L, "patch__skip_briefing_jmp");
        label(0x006D8740L, "patch__skip_briefing");

        // ===== Enemy Nametag Patches =====
        label(0x00441B57L, "patch__enemy_nametag_skip_jmp");
        label(0x00441AA7L, "patch__enemy_nametag_soldier");
        label(0x00441AF0L, "patch__enemy_nametag_pco");

        // ===== Hit Indicator Patches =====
        label(0x008D5A7CL, "const_0_0333f");
        label(0x00495A24L, "patch__hit_indicator_time");
        label(0x006AEBA8L, "patch__hit_indicator_alpha");
        label(0x006ADF69L, "patch__force_spawn_text");
        label(0x00495FD2L, "patch__force_disconnect_msg");

        // ===== Server Message Patches =====
        label(0x006A88B1L, "patch__server_msg_output_disable");
        label(0x006A88CEL, "patch__server_msg_output_disable_jmp");

        // ===== Renderer Patches =====
        label(0x004670A5L, "patch__renderer_draw_hook");
        label(0x00460A92L, "patch__texture_handler_registration");
        label(0x004676B4L, "patch__screenshot_name");
        label(0x004634B2L, "patch__screenshot_counter_init_disable");

        // ===== Resolution / Display Patches =====
        label(0x0045FBD0L, "patch__display_settings_string");
        label(0x006B1083L, "patch__screen_resolution_fix");
        label(0x0063F00EL, "RendPCDX8__getDisplaySettingID");
        label(0x0063F018L, "patch__screen_resolution_fix2");
        label(0x0045DD69L, "patch__menu_resolution");
        label(0x0063F217L, "patch__fpu_precision1");
        label(0x0063F0C1L, "patch__fpu_precision2");

        // ===== Misc Patches =====
        label(0x004904F0L, "patch__drop_actions");
        label(0x00462826L, "patch__showfps_precision1");
        label(0x004628E6L, "patch__showfps_precision2");
        label(0x0040CD5EL, "patch__cdkey_validation1");
        label(0x00459EEDL, "patch__cdkey_validation2");
        label(0x0049510AL, "patch__cdkey_validation3");
        label(0x0045831DL, "patch__cdkey_blacklist_disable");
        label(0x0045F0C9L, "patch__version_display_menu");
        label(0x0069F4AEL, "patch__serverlist_mod_column");
        label(0x0069F971L, "patch__serverlist_column_index");
        label(0x00401784L, "patch__serverlist_grey_version1");
        label(0x004017CEL, "patch__serverlist_grey_version2");

        // ===== Server List =====
        label(0x007D42A0L, "meme_ListBoxData__addColumnNoWidth");
        label(0x004B6FC0L, "RestartServerPinger__init");

        // ===== Other Functions =====
        label(0x0045D110L, "BfMenu__func_45D110");
        label(0x004B6F30L, "BfMenu__func_4B6F30");
        label(0x005810A0L, "String__func_5810A0");
        label(0x005C1280L, "File__CRC_func");
        label(0x00459E20L, "Map__validateFunc");
        label(0x00454480L, "Map__func_454480");
        label(0x00454DD0L, "Map__func_454DD0");
        label(0x00447980L, "Map__func_447980");
        label(0x00401290L, "Map__func_401290");
        label(0x00580EF0L, "String__func_580EF0");
        label(0x0045A100L, "Map__func_45A100");
        label(0x00459850L, "Map__func_459850");
        label(0x00443200L, "ModList__getList");
        label(0x00444500L, "Mod__checkFunc");
        label(0x00445FA0L, "Level__func_445FA0");
        label(0x0046F4C0L, "Level__func_46F4C0");
        label(0x004A55E0L, "Level__func_4A55E0");
        label(0x004F7910L, "LinkedList__getFirst");
        label(0x004F7080L, "Vehicle__func_4F7080");
        label(0x0044C2B0L, "Game__func_44C2B0");
        label(0x004444C0L, "Game__func_4444C0");
        label(0x004452C0L, "Game__func_4452C0");
        label(0x004067C0L, "Game__func_4067C0");
        label(0x0081FA20L, "Game__func_81FA20");
        label(0x0048A710L, "Player__func_48A710");
        label(0x005467E0L, "Packet__func_5467E0");
        label(0x007444A0L, "Packet__func_7444A0");
        label(0x006E4290L, "Player__setAliveState");
        label(0x004F54A0L, "Game__func_4F54A0");

        // ===== DllMain / Entry =====
        label(0x00804DA6L, "WinMain_call_in_DllMain");

        // ===== Return Address Checks =====
        label(0x006AC21BL, "ret__chatInput1");
        label(0x006AC488L, "ret__chatInput2");
        label(0x00491C4DL, "ret__GameClient_handleChatMessage");
        label(0x0049469EL, "ret__setCenterKillMessage");
        label(0x00418DE4L, "ret__removeFromIgnoreList");
        label(0x0046B130L, "ret__buddyColor_allied1");
        label(0x0046B802L, "ret__buddyColor_allied2");
        label(0x0046B165L, "ret__buddyColor_axis1");
        label(0x0046B852L, "ret__buddyColor_axis2");

        // ===== Executable Range =====
        label(0x00401000L, "exe_text_start");
        label(0x008C3000L, "exe_text_end");

        println("bf42plus labeling complete! Applied labels to ~180 addresses.");
    }
}
