#pragma once
#include <windows.h>

#define PIPE_NAME L"\\\\.\\pipe\\EliteHotkeyServicePipeV5"
#define REG_KEY_PATH L"SOFTWARE\\EliteSoftware\\Hotkeys\\ServiceV5"

struct HotkeyEntry {
    UINT modifiers;
    UINT vk;
    wchar_t name[64];
    wchar_t command[256];
    bool runAsSystem;
    bool blockFullscreen;
    bool runAsCommand;
    bool overrideNative;
    bool enabled;
};

struct IpcMessage {
    int type; // 1 = GetConfig, 2 = SetConfig, 3 = ExecuteCommand
    int count; // For SetConfig/GetConfig array size
};
