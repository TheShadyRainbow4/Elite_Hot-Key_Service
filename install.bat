@echo off
NET SESSION >nul 2>&1
if %errorLevel% == 0 (
    echo Administrator rights confirmed.
) else (
    echo Please run this script as Administrator.
    pause
    exit /b 1
)

echo Stopping old service if running...
sc stop EliteHotkeyService >nul 2>&1
taskkill /F /IM Elite-HotKey.exe >nul 2>&1
timeout /t 2 >nul

echo Copying files to System32...
copy /Y "BuildOutput\Elite-HotKeyService.exe" "%windir%\System32\Elite-HotKeyService.exe"
copy /Y "BuildOutput\Elite-HotKey.exe" "%windir%\System32\Elite-HotKey.exe"

echo Registering Service...
sc delete EliteHotkeyService >nul 2>&1
timeout /t 1 >nul
sc create EliteHotkeyService binPath= "%windir%\System32\Elite-HotKeyService.exe" start= auto DisplayName= "Elite Hot-Key Service"
sc description EliteHotkeyService "Elite Hot-Key IPC backend service. Intercepts elevated execution requests from the userland tray app. Default Configuration: Startup Type = Automatic, Log On As = Local System account (Highest Permissions). Do not modify these defaults or SYSTEM-level hotkeys will fail to execute."
sc config EliteHotkeyService start= auto

echo Registering App to Startup...
reg add "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Run" /v "EliteHotKey" /t REG_SZ /d "\"%windir%\System32\Elite-HotKey.exe\" -startup" /f

echo Starting Service...
sc start EliteHotkeyService

echo Starting App...
start "" "%windir%\System32\Elite-HotKey.exe" -startup

echo Install Complete!
pause
