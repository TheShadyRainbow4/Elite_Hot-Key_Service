#include <windows.h>
#include <vector>
#include <string>
#include <sddl.h>
#include "SharedConfig.h"

#pragma comment(lib, "advapi32.lib")

SERVICE_STATUS g_ServiceStatus = { 0 };
SERVICE_STATUS_HANDLE g_StatusHandle = NULL;
HANDLE g_ServiceStopEvent = INVALID_HANDLE_VALUE;

std::vector<HotkeyEntry> g_hotkeys;

void LoadConfig() {
    g_hotkeys.clear();
    HKEY hKey = NULL;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, REG_KEY_PATH, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        HKEY hHotkeysKey = NULL;
        if (RegCreateKeyExW(hKey, L"Entries", 0, NULL, REG_OPTION_NON_VOLATILE, KEY_READ, NULL, &hHotkeysKey, NULL) == ERROR_SUCCESS) {
            DWORD index = 0;
            wchar_t subKeyName[256];
            DWORD subKeyNameLen = 256;
            while (RegEnumKeyExW(hHotkeysKey, index, subKeyName, &subKeyNameLen, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
                HKEY hEntryKey = NULL;
                if (RegOpenKeyExW(hHotkeysKey, subKeyName, 0, KEY_READ, &hEntryKey) == ERROR_SUCCESS) {
                    HotkeyEntry entry = { 0 };
                    DWORD type, cbData;
                    
                    cbData = sizeof(entry.name);
                    RegQueryValueExW(hEntryKey, L"Name", NULL, &type, (LPBYTE)entry.name, &cbData);
                    
                    cbData = sizeof(entry.command);
                    RegQueryValueExW(hEntryKey, L"Command", NULL, &type, (LPBYTE)entry.command, &cbData);
                    
                    DWORD val = 0;
                    cbData = sizeof(val);
                    if (RegQueryValueExW(hEntryKey, L"Modifiers", NULL, &type, (LPBYTE)&val, &cbData) == ERROR_SUCCESS) entry.modifiers = val;
                    if (RegQueryValueExW(hEntryKey, L"VK", NULL, &type, (LPBYTE)&val, &cbData) == ERROR_SUCCESS) entry.vk = val;
                    if (RegQueryValueExW(hEntryKey, L"RunAsSystem", NULL, &type, (LPBYTE)&val, &cbData) == ERROR_SUCCESS) entry.runAsSystem = (val != 0);
                    if (RegQueryValueExW(hEntryKey, L"RunAsCommand", NULL, &type, (LPBYTE)&val, &cbData) == ERROR_SUCCESS) entry.runAsCommand = (val != 0);
                    if (RegQueryValueExW(hEntryKey, L"BlockFullscreen", NULL, &type, (LPBYTE)&val, &cbData) == ERROR_SUCCESS) entry.blockFullscreen = (val != 0);
                    if (RegQueryValueExW(hEntryKey, L"OverrideNative", NULL, &type, (LPBYTE)&val, &cbData) == ERROR_SUCCESS) entry.overrideNative = (val != 0);
                    
                    // Default to enabled if the key doesn't exist (for backward compatibility)
                    entry.enabled = true;
                    if (RegQueryValueExW(hEntryKey, L"Enabled", NULL, &type, (LPBYTE)&val, &cbData) == ERROR_SUCCESS) entry.enabled = (val != 0);
                    
                    g_hotkeys.push_back(entry);
                    RegCloseKey(hEntryKey);
                }
                index++;
                subKeyNameLen = 256;
            }
            RegCloseKey(hHotkeysKey);
        }
        RegCloseKey(hKey);
    }
}

void SaveConfig() {
    HKEY hKey = NULL;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, REG_KEY_PATH, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE | KEY_READ, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        RegDeleteTreeW(hKey, L"Entries");
        HKEY hHotkeysKey = NULL;
        if (RegCreateKeyExW(hKey, L"Entries", 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hHotkeysKey, NULL) == ERROR_SUCCESS) {
            for (size_t i = 0; i < g_hotkeys.size(); i++) {
                std::wstring subKeyNameStr = std::to_wstring(i);
                HKEY hEntryKey = NULL;
                if (RegCreateKeyExW(hHotkeysKey, subKeyNameStr.c_str(), 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hEntryKey, NULL) == ERROR_SUCCESS) {
                    RegSetValueExW(hEntryKey, L"Name", 0, REG_SZ, (const BYTE*)g_hotkeys[i].name, (DWORD)(wcslen(g_hotkeys[i].name) + 1) * sizeof(wchar_t));
                    RegSetValueExW(hEntryKey, L"Command", 0, REG_SZ, (const BYTE*)g_hotkeys[i].command, (DWORD)(wcslen(g_hotkeys[i].command) + 1) * sizeof(wchar_t));
                    
                    DWORD val = g_hotkeys[i].modifiers;
                    RegSetValueExW(hEntryKey, L"Modifiers", 0, REG_DWORD, (const BYTE*)&val, sizeof(val));
                    val = g_hotkeys[i].vk;
                    RegSetValueExW(hEntryKey, L"VK", 0, REG_DWORD, (const BYTE*)&val, sizeof(val));
                    val = g_hotkeys[i].runAsSystem ? 1 : 0;
                    RegSetValueExW(hEntryKey, L"RunAsSystem", 0, REG_DWORD, (const BYTE*)&val, sizeof(val));
                    val = g_hotkeys[i].runAsCommand ? 1 : 0;
                    RegSetValueExW(hEntryKey, L"RunAsCommand", 0, REG_DWORD, (const BYTE*)&val, sizeof(val));
                    val = g_hotkeys[i].blockFullscreen ? 1 : 0;
                    RegSetValueExW(hEntryKey, L"BlockFullscreen", 0, REG_DWORD, (const BYTE*)&val, sizeof(val));
                    val = g_hotkeys[i].overrideNative ? 1 : 0;
                    RegSetValueExW(hEntryKey, L"OverrideNative", 0, REG_DWORD, (const BYTE*)&val, sizeof(val));
                    val = g_hotkeys[i].enabled ? 1 : 0;
                    RegSetValueExW(hEntryKey, L"Enabled", 0, REG_DWORD, (const BYTE*)&val, sizeof(val));
                    
                    RegCloseKey(hEntryKey);
                }
            }
            RegCloseKey(hHotkeysKey);
        }
        RegCloseKey(hKey);
    }
}

DWORD WINAPI PipeServerThread(LPVOID lpParam) {
    LoadConfig();
    while (WaitForSingleObject(g_ServiceStopEvent, 0) != WAIT_OBJECT_0) {
        SECURITY_ATTRIBUTES sa;
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = FALSE;
        ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;WD)", SDDL_REVISION_1, &sa.lpSecurityDescriptor, NULL);
        
        HANDLE hPipe = CreateNamedPipeW(PIPE_NAME,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            1,
            sizeof(IpcMessage) + 100 * sizeof(HotkeyEntry),
            sizeof(IpcMessage) + 100 * sizeof(HotkeyEntry),
            0,
            &sa);
            
        if (sa.lpSecurityDescriptor) {
            LocalFree(sa.lpSecurityDescriptor);
        }
            
        if (hPipe != INVALID_HANDLE_VALUE) {
            if (ConnectNamedPipe(hPipe, NULL) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED)) {
                IpcMessage msg;
                DWORD bytesRead, bytesWritten;
                if (ReadFile(hPipe, &msg, sizeof(msg), &bytesRead, NULL)) {
                    if (msg.type == 1) { // GetConfig
                        msg.count = (int)g_hotkeys.size();
                        WriteFile(hPipe, &msg, sizeof(msg), &bytesWritten, NULL);
                        for (const auto& hk : g_hotkeys) {
                            WriteFile(hPipe, &hk, sizeof(hk), &bytesWritten, NULL);
                        }
                    } else if (msg.type == 2) { // SetConfig
                        g_hotkeys.clear();
                        for (int i = 0; i < msg.count; i++) {
                            HotkeyEntry hk;
                            if (ReadFile(hPipe, &hk, sizeof(hk), &bytesRead, NULL)) {
                                g_hotkeys.push_back(hk);
                            }
                        }
                        SaveConfig();
                    } else if (msg.type == 3) { // ExecuteCommand
                        wchar_t cmdStr[256];
                        if (ReadFile(hPipe, cmdStr, sizeof(cmdStr), &bytesRead, NULL)) {
                            wchar_t expandedCmd[512];
                            ExpandEnvironmentStringsW(cmdStr, expandedCmd, 512);

                            STARTUPINFOW si = { sizeof(si) };
                            PROCESS_INFORMATION pi = { 0 };
                            
                            std::wstring finalCmd;
                            if (msg.count == 1) {
                                finalCmd = L"cmd.exe /c \"";
                                finalCmd += expandedCmd;
                                finalCmd += L"\"";
                            } else {
                                finalCmd = expandedCmd;
                            }
                            
                            wchar_t mutableCmd[512];
                            wcscpy_s(mutableCmd, finalCmd.c_str());
                            
                            CreateProcessW(NULL, mutableCmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
                            if (pi.hProcess) {
                                CloseHandle(pi.hProcess);
                                CloseHandle(pi.hThread);
                            }
                        }
                    }
                }
            }
            FlushFileBuffers(hPipe);
            DisconnectNamedPipe(hPipe);
            CloseHandle(hPipe);
        }
    }
    return 0;
}

VOID WINAPI ServiceCtrlHandler(DWORD CtrlCode) {
    switch (CtrlCode) {
    case SERVICE_CONTROL_STOP:
        if (g_ServiceStatus.dwCurrentState != SERVICE_RUNNING) break;
        g_ServiceStatus.dwControlsAccepted = 0;
        g_ServiceStatus.dwCurrentState = SERVICE_STOP_PENDING;
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
        SetEvent(g_ServiceStopEvent);
        break;
    default:
        break;
    }
}

VOID WINAPI ServiceMain(DWORD argc, LPTSTR* argv) {
    g_StatusHandle = RegisterServiceCtrlHandlerW(L"EliteHotkeyService", ServiceCtrlHandler);
    if (!g_StatusHandle) return;
    
    g_ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_ServiceStatus.dwCurrentState = SERVICE_START_PENDING;
    g_ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP;
    g_ServiceStatus.dwWin32ExitCode = 0;
    g_ServiceStatus.dwWaitHint = 3000;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
    
    g_ServiceStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    
    g_ServiceStatus.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
    
    HANDLE hThread = CreateThread(NULL, 0, PipeServerThread, NULL, 0, NULL);
    
    WaitForSingleObject(g_ServiceStopEvent, INFINITE);
    
    g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
}

int wmain(int argc, wchar_t* argv[]) {
    if (argc > 1) {
        if (_wcsicmp(argv[1], L"install") == 0) {
            SC_HANDLE hSCM = OpenSCManager(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
            wchar_t path[MAX_PATH];
            GetModuleFileNameW(NULL, path, MAX_PATH);
            SC_HANDLE hService = CreateServiceW(hSCM, L"EliteHotkeyService", L"Elite Hotkey Service",
                SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                path, NULL, NULL, NULL, NULL, NULL);
            if (hService) {
                CloseServiceHandle(hService);
                CloseServiceHandle(hSCM);
                return 0;
            }
            return 1;
        }
    }
    
    SERVICE_TABLE_ENTRYW ServiceTable[] = {
        { (LPWSTR)L"EliteHotkeyService", (LPSERVICE_MAIN_FUNCTIONW)ServiceMain },
        { NULL, NULL }
    };
    
    StartServiceCtrlDispatcherW(ServiceTable);
    return 0;
}
