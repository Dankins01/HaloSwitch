@echo off
rem Packs Halo: Combat Evolved (OpenCE) into an installable title (NSP) on
rem your own PC, with your own console's keys. Nothing here leaves the PC.
rem
rem In this folder: hacbrewpack.exe, prod.keys (from Lockpick_RCM), and
rem this kit's exefs, control and romfs folders. The game's maps folder is
rem taken from MAPS below (the one OpenCE made on your SD card, or a copy).
setlocal
cd /d "%~dp0"
set MAPS=%~dp0..\maps
if not "%~1"=="" set MAPS=%~1
set TITLEID=0100484C4F430000

if not exist hacbrewpack.exe (
  echo hacbrewpack.exe is not in this folder. Download it from
  echo https://github.com/The-4n/hacBrewPack/releases and put it here.
  goto :fail
)
if not exist prod.keys if exist ..\prod.keys copy /y ..\prod.keys prod.keys >nul
if not exist prod.keys (
  echo prod.keys is not in this folder. Copy it here from your SD card
  echo ^(switch\prod.keys, made by Lockpick_RCM^).
  goto :fail
)
if not exist "%MAPS%\ui.map" (
  echo No maps folder at "%MAPS%" ^(ui.map not found^).
  echo Run:  pack-title.bat "C:\path\to\maps"
  goto :fail
)

echo Copying the maps into the title...
robocopy "%MAPS%" romfs\maps *.map /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 goto :fail

echo Packing (a few minutes: the maps are about 2 GB)...
if exist hacbrewpack_nsp rmdir /s /q hacbrewpack_nsp
hacbrewpack.exe -k prod.keys --titleid %TITLEID% --titlename "Halo: Combat Evolved" --titlepublisher "OpenCE" --nologo
if errorlevel 1 goto :fail

echo.
echo Done: hacbrewpack_nsp\%TITLEID%.nsp
echo Install it with DBI or Tinfoil. Keep the SD card's switch\opence folder:
echo the settings and saves stay there.
pause
exit /b 0

:fail
echo.
echo Packing stopped.
pause
exit /b 1
