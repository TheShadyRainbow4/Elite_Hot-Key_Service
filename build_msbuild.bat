@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64

echo Building App...
msbuild App.vcxproj /p:Configuration=Release /p:Platform=x64

echo Building Service...
msbuild Service.vcxproj /p:Configuration=Release /p:Platform=x64

echo Done!
