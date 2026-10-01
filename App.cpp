#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <shlwapi.h>
#include <string>
#include <vector>
#include <set>
#include "resource.h"
#include "SharedConfig.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shlwapi.lib")

HINSTANCE g_hInstance = NULL;
HWND g_hMainWnd = NULL;
HWND g_hConfigDlg = NULL;
std::vector<HotkeyEntry> g_hotkeys;
const int HARDCODED_HOTKEY_ID = 1;
UINT g_uTaskbarRestart = 0;

void LoadConfigFromServer();
void SaveConfigToServer();
void ApplyHotkeys();
void ExecuteCommandAsSystem(const wchar_t* cmd, bool runAsCommand);

void SaveColumnWidths(HWND hList) {
    HKEY hKey = NULL;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"SOFTWARE\\EliteSoftware\\Hotkeys\\AppV5", 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        for (int i = 0; i < 4; i++) {
            int width = ListView_GetColumnWidth(hList, i);
            std::wstring valName = L"ColWidth" + std::to_wstring(i);
            RegSetValueExW(hKey, valName.c_str(), 0, REG_DWORD, (const BYTE*)&width, sizeof(width));
        }
        RegCloseKey(hKey);
    }
}

void LoadColumnWidths(HWND hList) {
    HKEY hKey = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"SOFTWARE\\EliteSoftware\\Hotkeys\\AppV5", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        for (int i = 0; i < 4; i++) {
            DWORD width = 0;
            DWORD cbData = sizeof(width);
            DWORD type;
            std::wstring valName = L"ColWidth" + std::to_wstring(i);
            if (RegQueryValueExW(hKey, valName.c_str(), NULL, &type, (LPBYTE)&width, &cbData) == ERROR_SUCCESS) {
                ListView_SetColumnWidth(hList, i, width);
            }
        }
        RegCloseKey(hKey);
    }
}

bool IsFullscreenAppActive() {
    HWND hWnd = GetForegroundWindow();
    if (!hWnd) return false;
    
    HWND hDesktop = GetDesktopWindow();
    HWND hShell = GetShellWindow();
    if (hWnd == hDesktop || hWnd == hShell) return false;

    RECT rcApp;
    GetWindowRect(hWnd, &rcApp);

    HMONITOR hMonitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    if (GetMonitorInfo(hMonitor, &mi)) {
        if (rcApp.left <= mi.rcMonitor.left &&
            rcApp.top <= mi.rcMonitor.top &&
            rcApp.right >= mi.rcMonitor.right &&
            rcApp.bottom >= mi.rcMonitor.bottom) {
            return true;
        }
    }
    return false;
}

std::wstring GetHotkeyOnlyString(UINT mods, UINT vk) {
    std::wstring s;
    if (mods & MOD_CONTROL) s += L"CTRL+";
    if (mods & MOD_ALT) s += L"ALT+";
    if (mods & MOD_SHIFT) s += L"SHIFT+";
    if (mods & MOD_WIN) s += L"WIN+";
    if (vk) {
        wchar_t keyStr[32] = {0};
        UINT scanCode = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        
        switch (vk) {
            case VK_LEFT: case VK_UP: case VK_RIGHT: case VK_DOWN:
            case VK_PRIOR: case VK_NEXT: case VK_END: case VK_HOME:
            case VK_INSERT: case VK_DELETE: case VK_DIVIDE: case VK_NUMLOCK:
                scanCode |= 0x100;
                break;
        }
        
        GetKeyNameTextW(scanCode << 16, keyStr, 32);
        s += keyStr;
    }
    return s;
}

void RefreshListView(HWND hList) {
    ListView_DeleteAllItems(hList);
    for (size_t i = 0; i < g_hotkeys.size(); i++) {
        const auto& hk = g_hotkeys[i];
        
        LVITEMW lvi = { 0 };
        lvi.mask = LVIF_TEXT | LVIF_PARAM;
        lvi.iItem = (int)i;
        lvi.iSubItem = 0;
        lvi.pszText = (LPWSTR)hk.name;
        lvi.lParam = (LPARAM)i;
        ListView_InsertItem(hList, &lvi);
        ListView_SetCheckState(hList, (int)i, hk.enabled ? TRUE : FALSE);
        
        wchar_t file[256] = {0};
        wchar_t dir[256] = {0};
        
        if (hk.runAsCommand) {
            wcscpy_s(file, L"[Command Line]");
            wcscpy_s(dir, hk.command);
        } else {
            wcscpy_s(dir, hk.command);
            LPWSTR pFile = PathFindFileNameW(dir);
            if (pFile && pFile != dir) {
                wcscpy_s(file, pFile);
                PathRemoveFileSpecW(dir);
            } else {
                wcscpy_s(file, dir);
                wcscpy_s(dir, L"");
            }
        }
        
        ListView_SetItemText(hList, (int)i, 1, file);
        ListView_SetItemText(hList, (int)i, 2, dir);
        
        std::wstring hotkeyStr = GetHotkeyOnlyString(hk.modifiers, hk.vk);
        ListView_SetItemText(hList, (int)i, 3, (LPWSTR)hotkeyStr.c_str());
    }
}

HotkeyEntry* g_pCurrentEdit = nullptr;
UINT g_editMods = 0;
UINT g_editVk = 0;
HHOOK g_hKeyboardHook = NULL;
HWND g_hHotkeyEdit = NULL;

bool g_bSwallowNextWinUp = false;
UINT g_swallowedMods = 0;

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT* p = (KBDLLHOOKSTRUCT*)lParam;
        UINT vk = p->vkCode;
        
        if (g_hHotkeyEdit && GetFocus() == g_hHotkeyEdit) {
            if (vk == VK_TAB || vk == VK_RETURN || vk == VK_ESCAPE) {
                return CallNextHookEx(g_hKeyboardHook, nCode, wParam, lParam);
            }
            
            if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
                if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL) g_swallowedMods |= MOD_CONTROL;
                if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) g_swallowedMods |= MOD_ALT;
                if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) g_swallowedMods |= MOD_SHIFT;
                if (vk == VK_LWIN || vk == VK_RWIN) g_swallowedMods |= MOD_WIN;
                
                UINT activeMods = g_swallowedMods;
                if (GetAsyncKeyState(VK_CONTROL) & 0x8000) activeMods |= MOD_CONTROL;
                if (GetAsyncKeyState(VK_MENU) & 0x8000) activeMods |= MOD_ALT;
                if (GetAsyncKeyState(VK_SHIFT) & 0x8000) activeMods |= MOD_SHIFT;
                if ((GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000)) activeMods |= MOD_WIN;
                
                bool isMod = (vk == VK_LWIN || vk == VK_RWIN || vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU);
                
                g_editMods = activeMods;
                if (!isMod) {
                    g_editVk = vk;
                } else {
                    g_editVk = 0;
                }
                
                std::wstring s = GetHotkeyOnlyString(g_editMods, g_editVk);
                SetWindowTextW(g_hHotkeyEdit, s.c_str());
            } else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
                if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL) g_swallowedMods &= ~MOD_CONTROL;
                if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) g_swallowedMods &= ~MOD_ALT;
                if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) g_swallowedMods &= ~MOD_SHIFT;
                if (vk == VK_LWIN || vk == VK_RWIN) g_swallowedMods &= ~MOD_WIN;
            }
            return 1;
        }
        
        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
            UINT mods = 0;
            if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
            if (GetAsyncKeyState(VK_MENU) & 0x8000) mods |= MOD_ALT;
            if (GetAsyncKeyState(VK_SHIFT) & 0x8000) mods |= MOD_SHIFT;
            if ((GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000)) mods |= MOD_WIN;
            
            bool isMod = (vk == VK_LWIN || vk == VK_RWIN || vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU);
            
            if (!isMod) {
                for (size_t i = 0; i < g_hotkeys.size(); i++) {
                    if (g_hotkeys[i].enabled && g_hotkeys[i].overrideNative && g_hotkeys[i].vk == vk && g_hotkeys[i].modifiers == mods) {
                        if (mods & MOD_WIN) {
                            INPUT input[2] = {};
                            input[0].type = INPUT_KEYBOARD;
                            input[0].ki.wVk = VK_CONTROL;
                            input[1].type = INPUT_KEYBOARD;
                            input[1].ki.wVk = VK_CONTROL;
                            input[1].ki.dwFlags = KEYEVENTF_KEYUP;
                            SendInput(2, input, sizeof(INPUT));
                        }
                        PostMessageW(g_hMainWnd, WM_HOTKEY, 100 + i, 0);
                        return 1;
                    }
                }
            }
        }
    }
    return CallNextHookEx(g_hKeyboardHook, nCode, wParam, lParam);
}

bool CheckConflict(HWND hwndParent, UINT mods, UINT vk, HotkeyEntry* ignoreEntry, bool overrideNative) {
    for (auto& hk : g_hotkeys) {
        if (&hk == ignoreEntry) continue;
        if (hk.modifiers == mods && hk.vk == vk) {
            MessageBoxW(hwndParent, L"This hotkey conflicts with another hotkey already configured in this app.\nIt will still be saved.", L"Warning", MB_ICONWARNING | MB_OK | MB_TOPMOST);
            return true;
        }
    }
    
    if (!overrideNative) {
        if (!RegisterHotKey(NULL, 9999, mods | MOD_NOREPEAT, vk)) {
            MessageBoxW(hwndParent, L"This hotkey is already registered by the system or another application.\nYou may need to check 'Override Native OS Hotkey' for it to work.", L"Warning", MB_ICONWARNING | MB_OK | MB_TOPMOST);
            return true;
        }
        UnregisterHotKey(NULL, 9999);
    }
    
    return false;
}

INT_PTR CALLBACK EditDlgProc(HWND hwndDlg, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_INITDIALOG: {
        SendMessage(hwndDlg, WM_SETICON, ICON_SMALL, (LPARAM)LoadIcon(g_hInstance, MAKEINTRESOURCE(IDI_APP_ICON)));
        SendMessage(hwndDlg, WM_SETICON, ICON_BIG, (LPARAM)LoadIcon(g_hInstance, MAKEINTRESOURCE(IDI_APP_ICON)));
        g_pCurrentEdit = (HotkeyEntry*)lParam;
        g_editMods = g_pCurrentEdit->modifiers;
        g_editVk = g_pCurrentEdit->vk;
        
        if (g_editVk == 0) {
            SetWindowTextW(hwndDlg, L"Add Hotkey");
        } else {
            SetWindowTextW(hwndDlg, L"Edit Hotkey");
        }
        
        g_hHotkeyEdit = GetDlgItem(hwndDlg, IDC_HOTKEY_CTRL);
        
        SetDlgItemTextW(hwndDlg, IDC_NAME, g_pCurrentEdit->name);
        SetWindowTextW(g_hHotkeyEdit, GetHotkeyOnlyString(g_editMods, g_editVk).c_str());
        SetDlgItemTextW(hwndDlg, IDC_COMMAND, g_pCurrentEdit->command);
        SHAutoComplete(GetDlgItem(hwndDlg, IDC_COMMAND), SHACF_FILESYSTEM);
        
        CheckDlgButton(hwndDlg, IDC_RUN_AS_COMMAND, g_pCurrentEdit->runAsCommand ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwndDlg, IDC_RUN_AS_SYSTEM, g_pCurrentEdit->runAsSystem ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwndDlg, IDC_BLOCK_FULLSCREEN, g_pCurrentEdit->blockFullscreen ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwndDlg, IDC_OVERRIDE_NATIVE, g_pCurrentEdit->overrideNative ? BST_CHECKED : BST_UNCHECKED);
        
        RECT rcOwner, rcDlg, rc;
        GetWindowRect(GetParent(hwndDlg) ? GetParent(hwndDlg) : GetDesktopWindow(), &rcOwner);
        GetWindowRect(hwndDlg, &rcDlg);
        CopyRect(&rc, &rcOwner);
        OffsetRect(&rcDlg, -rcDlg.left, -rcDlg.top);
        OffsetRect(&rc, -rc.left, -rc.top);
        OffsetRect(&rc, (rc.right - rcDlg.right) / 2, (rc.bottom - rcDlg.bottom) / 2);
        SetWindowPos(hwndDlg, HWND_TOP, rcOwner.left + rc.left, rcOwner.top + rc.top, 0, 0, SWP_NOSIZE);
        
        return (INT_PTR)TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            if (g_editVk == 0) {
                MessageBoxW(hwndDlg, L"Please press a hotkey combination.", L"Error", MB_ICONERROR | MB_TOPMOST);
                return (INT_PTR)TRUE;
            }
            
            wchar_t cmdStr[256];
            GetDlgItemTextW(hwndDlg, IDC_COMMAND, cmdStr, 256);
            if (wcslen(cmdStr) == 0) {
                MessageBoxW(hwndDlg, L"Please enter a command.", L"Error", MB_ICONERROR | MB_TOPMOST);
                return (INT_PTR)TRUE;
            }
            
            bool willOverride = (IsDlgButtonChecked(hwndDlg, IDC_OVERRIDE_NATIVE) == BST_CHECKED);
            CheckConflict(hwndDlg, g_editMods, g_editVk, g_pCurrentEdit, willOverride);
            
            GetDlgItemTextW(hwndDlg, IDC_NAME, g_pCurrentEdit->name, 64);
            g_pCurrentEdit->modifiers = g_editMods;
            g_pCurrentEdit->vk = g_editVk;
            wcsncpy_s(g_pCurrentEdit->command, cmdStr, _TRUNCATE);
            g_pCurrentEdit->runAsCommand = (IsDlgButtonChecked(hwndDlg, IDC_RUN_AS_COMMAND) == BST_CHECKED);
            g_pCurrentEdit->runAsSystem = (IsDlgButtonChecked(hwndDlg, IDC_RUN_AS_SYSTEM) == BST_CHECKED);
            g_pCurrentEdit->blockFullscreen = (IsDlgButtonChecked(hwndDlg, IDC_BLOCK_FULLSCREEN) == BST_CHECKED);
            g_pCurrentEdit->overrideNative = willOverride;
            
            EndDialog(hwndDlg, 1);
        }
        else if (LOWORD(wParam) == IDCANCEL) {
            EndDialog(hwndDlg, 0);
        }
        break;
    case WM_DESTROY:
        g_hHotkeyEdit = NULL;
        g_swallowedMods = 0;
        break;
    }
    return (INT_PTR)FALSE;
}

INT_PTR CALLBACK ConfigDlgProc(HWND hwndDlg, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_INITDIALOG: {
        SendMessage(hwndDlg, WM_SETICON, ICON_SMALL, (LPARAM)LoadIcon(g_hInstance, MAKEINTRESOURCE(IDI_APP_ICON)));
        SendMessage(hwndDlg, WM_SETICON, ICON_BIG, (LPARAM)LoadIcon(g_hInstance, MAKEINTRESOURCE(IDI_APP_ICON)));

        HMONITOR hMonitor = MonitorFromWindow(NULL, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoW(hMonitor, &mi);
        RECT rc;
        GetWindowRect(hwndDlg, &rc);
        int x = mi.rcMonitor.left + (mi.rcMonitor.right - mi.rcMonitor.left - (rc.right - rc.left)) / 2;
        int y = mi.rcMonitor.top + (mi.rcMonitor.bottom - mi.rcMonitor.top - (rc.bottom - rc.top)) / 2;
        SetWindowPos(hwndDlg, NULL, x, y, 0, 0, SWP_NOZORDER | SWP_NOSIZE);

        HWND hList = GetDlgItem(hwndDlg, IDC_HOTKEY_LIST);
        ListView_SetExtendedListViewStyle(hList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_CHECKBOXES);
        
        LVCOLUMNW lvc = { 0 };
        lvc.mask = LVCF_FMT | LVCF_WIDTH | LVCF_TEXT | LVCF_SUBITEM;
        lvc.fmt = LVCFMT_LEFT;
        
        lvc.iSubItem = 0; lvc.cx = 80; lvc.pszText = (LPWSTR)L"Name";
        ListView_InsertColumn(hList, 0, &lvc);
        
        lvc.iSubItem = 1; lvc.cx = 90; lvc.pszText = (LPWSTR)L"Target File";
        ListView_InsertColumn(hList, 1, &lvc);
        
        lvc.iSubItem = 2; lvc.cx = 120; lvc.pszText = (LPWSTR)L"Target Path";
        ListView_InsertColumn(hList, 2, &lvc);
        
        lvc.iSubItem = 3; lvc.cx = 80; lvc.pszText = (LPWSTR)L"Hot-Keys";
        ListView_InsertColumn(hList, 3, &lvc);

        LoadColumnWidths(hList);
        RefreshListView(hList);
        return (INT_PTR)TRUE;
    }
    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_CLOSE || (wParam & 0xFFF0) == SC_MINIMIZE) {
            if ((wParam & 0xFFF0) == SC_CLOSE) {
                LoadConfigFromServer();
                RefreshListView(GetDlgItem(hwndDlg, IDC_HOTKEY_LIST));
            }
            SaveColumnWidths(GetDlgItem(hwndDlg, IDC_HOTKEY_LIST));
            ShowWindow(hwndDlg, SW_HIDE);
            return (INT_PTR)TRUE;
        }
        break;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_ADD) {
            HotkeyEntry newHk = {0};
            newHk.enabled = true;
            if (DialogBoxParamW(g_hInstance, MAKEINTRESOURCEW(IDD_EDIT_DIALOG), hwndDlg, EditDlgProc, (LPARAM)&newHk) == 1) {
                g_hotkeys.push_back(newHk);
                HWND hList = GetDlgItem(hwndDlg, IDC_HOTKEY_LIST);
                RefreshListView(hList);
            }
        }
        else if (LOWORD(wParam) == IDC_REMOVE) {
            HWND hList = GetDlgItem(hwndDlg, IDC_HOTKEY_LIST);
            int sel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
            if (sel != -1) {
                g_hotkeys.erase(g_hotkeys.begin() + sel);
                RefreshListView(hList);
            }
        }
        else if (LOWORD(wParam) == IDC_EXPORT) {
            OPENFILENAMEW ofn = {0};
            wchar_t szFile[MAX_PATH] = L"HotkeysBackup.ehk";
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwndDlg;
            ofn.lpstrFile = szFile;
            ofn.nMaxFile = sizeof(szFile) / sizeof(wchar_t);
            ofn.lpstrFilter = L"Elite Hotkey Backup (*.ehk)\0*.ehk\0All Files (*.*)\0*.*\0";
            ofn.nFilterIndex = 1;
            ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
            if (GetSaveFileNameW(&ofn)) {
                HANDLE hFile = CreateFileW(szFile, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                if (hFile != INVALID_HANDLE_VALUE) {
                    DWORD written;
                    int count = (int)g_hotkeys.size();
                    WriteFile(hFile, &count, sizeof(count), &written, NULL);
                    for (int i = 0; i < count; i++) {
                        WriteFile(hFile, &g_hotkeys[i], sizeof(HotkeyEntry), &written, NULL);
                    }
                    CloseHandle(hFile);
                    MessageBoxW(hwndDlg, L"Backup exported successfully!", L"Export", MB_OK | MB_ICONINFORMATION);
                }
            }
        }
        else if (LOWORD(wParam) == IDC_IMPORT) {
            OPENFILENAMEW ofn = {0};
            wchar_t szFile[MAX_PATH] = {0};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwndDlg;
            ofn.lpstrFile = szFile;
            ofn.nMaxFile = sizeof(szFile) / sizeof(wchar_t);
            ofn.lpstrFilter = L"Elite Hotkey Backup (*.ehk)\0*.ehk\0All Files (*.*)\0*.*\0";
            ofn.nFilterIndex = 1;
            ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
            if (GetOpenFileNameW(&ofn)) {
                HANDLE hFile = CreateFileW(szFile, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
                if (hFile != INVALID_HANDLE_VALUE) {
                    DWORD read;
                    int count = 0;
                    if (ReadFile(hFile, &count, sizeof(count), &read, NULL) && count >= 0 && count <= 1000) {
                        g_hotkeys.clear();
                        for (int i = 0; i < count; i++) {
                            HotkeyEntry entry;
                            if (ReadFile(hFile, &entry, sizeof(HotkeyEntry), &read, NULL) && read == sizeof(HotkeyEntry)) {
                                g_hotkeys.push_back(entry);
                            }
                        }
                        RefreshListView(GetDlgItem(hwndDlg, IDC_HOTKEY_LIST));
                        MessageBoxW(hwndDlg, L"Backup imported! Click Apply or OK to save changes.", L"Import", MB_OK | MB_ICONINFORMATION);
                    } else {
                        MessageBoxW(hwndDlg, L"Invalid backup file.", L"Error", MB_OK | MB_ICONERROR);
                    }
                    CloseHandle(hFile);
                }
            }
        }
        else if (LOWORD(wParam) == IDC_APPLY) {
            SaveConfigToServer();
            ApplyHotkeys();
        }
        else if (LOWORD(wParam) == IDOK) {
            SaveConfigToServer();
            ApplyHotkeys();
            SaveColumnWidths(GetDlgItem(hwndDlg, IDC_HOTKEY_LIST));
            ShowWindow(hwndDlg, SW_HIDE);
        }
        else if (LOWORD(wParam) == IDCANCEL) {
            LoadConfigFromServer();
            HWND hList = GetDlgItem(hwndDlg, IDC_HOTKEY_LIST);
            RefreshListView(hList);
            SaveColumnWidths(hList);
            ShowWindow(hwndDlg, SW_HIDE);
        }
        break;
    case WM_NOTIFY: {
        LPNMHDR lpnm = (LPNMHDR)lParam;
        if (lpnm->idFrom == IDC_HOTKEY_LIST) {
            if (lpnm->code == LVN_ITEMCHANGED) {
                LPNMLISTVIEW pnmv = (LPNMLISTVIEW)lParam;
                if ((pnmv->uChanged & LVIF_STATE) && pnmv->iItem >= 0) {
                    UINT oldState = pnmv->uOldState & LVIS_STATEIMAGEMASK;
                    UINT newState = pnmv->uNewState & LVIS_STATEIMAGEMASK;
                    if (oldState != newState && oldState != 0) {
                        bool checked = (newState >> 12) == 2;
                        if (pnmv->iItem < (int)g_hotkeys.size() && g_hotkeys[pnmv->iItem].enabled != checked) {
                            g_hotkeys[pnmv->iItem].enabled = checked;
                        }
                    }
                }
            } else if (lpnm->code == NM_DBLCLK) {
                HWND hList = GetDlgItem(hwndDlg, IDC_HOTKEY_LIST);
                int sel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
                if (sel != -1 && sel < (int)g_hotkeys.size()) {
                    if (DialogBoxParamW(g_hInstance, MAKEINTRESOURCEW(IDD_EDIT_DIALOG), hwndDlg, EditDlgProc, (LPARAM)&g_hotkeys[sel]) == 1) {
                        RefreshListView(hList);
                    }
                }
            } else if (lpnm->code == NM_CUSTOMDRAW) {
                LPNMLVCUSTOMDRAW lplvcd = (LPNMLVCUSTOMDRAW)lParam;
                switch (lplvcd->nmcd.dwDrawStage) {
                    case CDDS_PREPAINT: {
                        HWND hList = lpnm->hwndFrom;
                        HDC hdc = lplvcd->nmcd.hdc;
                        RECT rc;
                        GetClientRect(hList, &rc);
                        
                        int itemHeight = 18;
                        int startY = 0;
                        int topIndex = 0;
                        
                        HWND hHeader = ListView_GetHeader(hList);
                        if (hHeader) {
                            RECT rcHeader;
                            GetWindowRect(hHeader, &rcHeader);
                            startY = rcHeader.bottom - rcHeader.top;
                        }
                        
                        if (ListView_GetItemCount(hList) > 0) {
                            RECT rcItem;
                            if (ListView_GetItemRect(hList, 0, &rcItem, LVIR_BOUNDS)) {
                                itemHeight = rcItem.bottom - rcItem.top;
                            }
                            topIndex = ListView_GetTopIndex(hList);
                            POINT pt = {0, 0};
                            if (ListView_GetItemPosition(hList, topIndex, &pt)) {
                                startY = pt.y;
                            }
                        }
                        
                        HBRUSH hEven = CreateSolidBrush(RGB(255, 255, 255));
                        HBRUSH hOdd = CreateSolidBrush(RGB(242, 242, 242));
                        for (int y = startY; y < rc.bottom; y += itemHeight) {
                            RECT r = { rc.left, y, rc.right, y + itemHeight };
                            int index = topIndex + ((y - startY) / itemHeight);
                            if (index < 0) continue;
                            FillRect(hdc, &r, (index % 2 == 0) ? hEven : hOdd);
                        }
                        DeleteObject(hEven);
                        DeleteObject(hOdd);
                        
                        SetWindowLongPtrW(hwndDlg, DWLP_MSGRESULT, CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT);
                        return (INT_PTR)TRUE;
                    }
                    case CDDS_POSTPAINT: {
                        HWND hList = lpnm->hwndFrom;
                        HDC hdc = lplvcd->nmcd.hdc;
                        RECT rc;
                        GetClientRect(hList, &rc);
                        
                        int startY = 0;
                        HWND hHeader = ListView_GetHeader(hList);
                        if (hHeader) {
                            RECT rcHeader;
                            GetWindowRect(hHeader, &rcHeader);
                            startY = rcHeader.bottom - rcHeader.top;
                        }

                        HPEN hGridPen = CreatePen(PS_SOLID, 1, RGB(180, 180, 180));
                        HPEN hOldPen = (HPEN)SelectObject(hdc, hGridPen);
                        
                        int cols = Header_GetItemCount(hHeader);
                        int currentX = rc.left;
                        for (int c = 0; c < cols; c++) {
                            RECT rcHeaderItem;
                            Header_GetItemRect(hHeader, c, &rcHeaderItem);
                            currentX += (rcHeaderItem.right - rcHeaderItem.left);
                            MoveToEx(hdc, currentX - 1, startY, NULL);
                            LineTo(hdc, currentX - 1, rc.bottom);
                        }
                        
                        // Also draw horizontal lines
                        int itemHeight = 18;
                        if (ListView_GetItemCount(hList) > 0) {
                            RECT rcItem;
                            if (ListView_GetItemRect(hList, 0, &rcItem, LVIR_BOUNDS)) {
                                itemHeight = rcItem.bottom - rcItem.top;
                            }
                            int topIndex = ListView_GetTopIndex(hList);
                            POINT pt = {0, 0};
                            if (ListView_GetItemPosition(hList, topIndex, &pt)) {
                                startY = pt.y;
                            }
                        }
                        for (int y = startY; y < rc.bottom; y += itemHeight) {
                            MoveToEx(hdc, rc.left, y + itemHeight - 1, NULL);
                            LineTo(hdc, rc.right, y + itemHeight - 1);
                        }
                        
                        SelectObject(hdc, hOldPen);
                        DeleteObject(hGridPen);
                        
                        SetWindowLongPtrW(hwndDlg, DWLP_MSGRESULT, CDRF_DODEFAULT);
                        return (INT_PTR)TRUE;
                    }
                    case CDDS_ITEMPREPAINT:
                        if ((lplvcd->nmcd.dwItemSpec % 2) == 0) {
                            lplvcd->clrTextBk = RGB(255, 255, 255);
                        } else {
                            lplvcd->clrTextBk = RGB(240, 240, 240);
                        }
                        SetWindowLongPtrW(hwndDlg, DWLP_MSGRESULT, CDRF_NEWFONT);
                        return (INT_PTR)TRUE;
                }
            }
        }
        break;
    }
    }
    return (INT_PTR)FALSE;
}

void ShowConfigDialog() {
    if (!g_hConfigDlg) {
        g_hConfigDlg = CreateDialogW(g_hInstance, MAKEINTRESOURCEW(IDD_CONFIG_DIALOG), g_hMainWnd, ConfigDlgProc);
    }
    ShowWindow(g_hConfigDlg, SW_SHOW);
    SetForegroundWindow(g_hConfigDlg);
}

void ExecuteCommandAsSystem(const wchar_t* cmd, bool runAsCommand) {
    HANDLE hPipe = CreateFileW(PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (hPipe != INVALID_HANDLE_VALUE) {
        IpcMessage msg;
        msg.type = 3;
        msg.count = runAsCommand ? 1 : 0;
        DWORD bytesWritten;
        WriteFile(hPipe, &msg, sizeof(msg), &bytesWritten, NULL);
        WriteFile(hPipe, cmd, 256 * sizeof(wchar_t), &bytesWritten, NULL);
        CloseHandle(hPipe);
    }
}

void LoadConfigFromServer() {
    HANDLE hPipe = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 10; i++) {
        if (WaitNamedPipeW(PIPE_NAME, 1000)) {
            hPipe = CreateFileW(PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
            if (hPipe != INVALID_HANDLE_VALUE) break;
        } else {
            hPipe = CreateFileW(PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
            if (hPipe != INVALID_HANDLE_VALUE) break;
            Sleep(200);
        }
    }
    if (hPipe != INVALID_HANDLE_VALUE) {
        IpcMessage msg;
        msg.type = 1;
        msg.count = 0;
        DWORD bytesWritten, bytesRead;
        WriteFile(hPipe, &msg, sizeof(msg), &bytesWritten, NULL);
        if (ReadFile(hPipe, &msg, sizeof(msg), &bytesRead, NULL)) {
            g_hotkeys.clear();
            for (int i = 0; i < msg.count; i++) {
                HotkeyEntry entry;
                if (ReadFile(hPipe, &entry, sizeof(entry), &bytesRead, NULL)) {
                    g_hotkeys.push_back(entry);
                }
            }
        }
        CloseHandle(hPipe);
    }
    ApplyHotkeys();
}

void SaveConfigToServer() {
    HANDLE hPipe = CreateFileW(PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (hPipe != INVALID_HANDLE_VALUE) {
        IpcMessage msg;
        msg.type = 2;
        msg.count = (int)g_hotkeys.size();
        DWORD bytesWritten;
        WriteFile(hPipe, &msg, sizeof(msg), &bytesWritten, NULL);
        for (const auto& hk : g_hotkeys) {
            WriteFile(hPipe, &hk, sizeof(hk), &bytesWritten, NULL);
        }
        CloseHandle(hPipe);
    }
}

void ApplyHotkeys() {
    for (int i = 0; i < 1000; i++) {
        UnregisterHotKey(g_hMainWnd, 100 + i);
    }
    
    for (size_t i = 0; i < g_hotkeys.size(); i++) {
        const auto& hk = g_hotkeys[i];
        if (hk.enabled && !hk.overrideNative) {
            RegisterHotKey(g_hMainWnd, 100 + (int)i, hk.modifiers | MOD_NOREPEAT, hk.vk);
        }
    }
}

void ShowContextMenu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);
    HMENU hMenu = CreatePopupMenu();
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING, ID_TRAY_ADD, L"Add New Hotkey");
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING, ID_TRAY_CONFIG, L"Configuration");
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING, ID_TRAY_EXIT, L"Exit");
    SetForegroundWindow(hwnd);
    TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(hMenu);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == g_uTaskbarRestart && g_uTaskbarRestart != 0) {
        NOTIFYICONDATAW nid = { sizeof(nid) };
        nid.hWnd = hwnd;
        nid.uID = ID_TRAY_APP_ICON;
        nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        nid.uCallbackMessage = WM_TRAYICON;
        nid.hIcon = LoadIcon(g_hInstance, MAKEINTRESOURCE(IDI_APP_ICON));
        wcscpy_s(nid.szTip, L"Elite Hotkey App");
        Shell_NotifyIconW(NIM_ADD, &nid);
        return 0;
    }

    switch (uMsg) {
    case WM_CREATE: {
        NOTIFYICONDATAW nid = { sizeof(nid) };
        nid.hWnd = hwnd;
        nid.uID = ID_TRAY_APP_ICON;
        nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        nid.uCallbackMessage = WM_TRAYICON;
        nid.hIcon = LoadIcon(g_hInstance, MAKEINTRESOURCE(IDI_APP_ICON));
        wcscpy_s(nid.szTip, L"Elite Hotkey App");
        Shell_NotifyIconW(NIM_ADD, &nid);
        
        RegisterHotKey(hwnd, HARDCODED_HOTKEY_ID, MOD_CONTROL | MOD_ALT, 'M');
        return 0;
    }
    case WM_HOTKEY: {
        int id = (int)wParam;
        if (id == HARDCODED_HOTKEY_ID) {
            ShowConfigDialog();
        } else {
            int index = id - 100;
            if (index >= 0 && index < (int)g_hotkeys.size()) {
                const auto& hk = g_hotkeys[index];
                if (!hk.enabled) return 0;
                if (hk.blockFullscreen && IsFullscreenAppActive()) {
                    break;
                }
                if (hk.runAsSystem) {
                    ExecuteCommandAsSystem(hk.command, hk.runAsCommand);
                } else {
                    wchar_t expandedCmd[1024];
                    ExpandEnvironmentStringsW(hk.command, expandedCmd, 1024);
                    
                    if (hk.runAsCommand) {
                        std::wstring cmd = L"/c \"";
                        cmd += expandedCmd;
                        cmd += L"\"";
                        ShellExecuteW(NULL, L"open", L"cmd.exe", cmd.c_str(), NULL, SW_SHOWNORMAL);
                    } else {
                        wchar_t* args = PathGetArgsW(expandedCmd);
                        if (args && *args) {
                            PathRemoveArgsW(expandedCmd);
                            PathUnquoteSpacesW(expandedCmd);
                            ShellExecuteW(NULL, L"open", expandedCmd, args, NULL, SW_SHOWNORMAL);
                        } else {
                            PathUnquoteSpacesW(expandedCmd);
                            ShellExecuteW(NULL, L"open", expandedCmd, NULL, NULL, SW_SHOWNORMAL);
                        }
                    }
                }
            }
        }
        return 0;
    }
    case WM_TRAYICON:
        if (lParam == WM_RBUTTONUP) {
            ShowContextMenu(hwnd);
        } else if (lParam == WM_LBUTTONDBLCLK) {
            ShowConfigDialog();
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wParam) == ID_TRAY_EXIT) {
            PostQuitMessage(0);
        } else if (LOWORD(wParam) == ID_TRAY_CONFIG) {
            ShowConfigDialog();
        } else if (LOWORD(wParam) == ID_TRAY_ADD) {
            HotkeyEntry newHk = {0};
            newHk.enabled = true;
            if (DialogBoxParamW(g_hInstance, MAKEINTRESOURCEW(IDD_EDIT_DIALOG), NULL, EditDlgProc, (LPARAM)&newHk) == 1) {
                g_hotkeys.push_back(newHk);
                SaveConfigToServer();
                ApplyHotkeys();
                if (g_hConfigDlg && IsWindowVisible(g_hConfigDlg)) {
                    HWND hList = GetDlgItem(g_hConfigDlg, IDC_HOTKEY_LIST);
                    RefreshListView(hList);
                }
            }
        }
        return 0;
    case WM_DESTROY: {
        NOTIFYICONDATAW nid = { sizeof(nid) };
        nid.hWnd = hwnd;
        nid.uID = ID_TRAY_APP_ICON;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
    g_hInstance = hInstance;
    
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    
    g_uTaskbarRestart = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilter(g_uTaskbarRestart, MSGFLT_ADD);
    
    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"Local\\EliteHotkeyApp_Instance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND hExisting = FindWindowW(L"EliteHotkeyAppClass", L"Elite Hotkey App");
        if (hExisting) {
            PostMessageW(hExisting, g_uTaskbarRestart, 0, 0);
            PostMessageW(hExisting, WM_COMMAND, ID_TRAY_CONFIG, 0);
            SetForegroundWindow(hExisting);
        }
        CloseHandle(hMutex);
        CoUninitialize();
        return 0;
    }
    
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icex);
    
    bool showOnStartup = true;
    if (wcsstr(lpCmdLine, L"-startup")) {
        showOnStartup = false;
    }
    
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"EliteHotkeyAppClass";
    RegisterClassW(&wc);
    
    g_hMainWnd = CreateWindowExW(0, wc.lpszClassName, L"Elite Hotkey App", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        NULL, NULL, hInstance, NULL);
        
    LoadConfigFromServer();
    
    if (showOnStartup) {
        ShowConfigDialog();
    }
    
    g_hKeyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, g_hInstance, 0);
    
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (!g_hConfigDlg || !IsDialogMessage(g_hConfigDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
    
    if (g_hKeyboardHook) {
        UnhookWindowsHookEx(g_hKeyboardHook);
    }
    
    CoUninitialize();
    return 0;
}
