@echo off
rem ============================================================================
rem  Card Craft pro M5Stack Tab5 - build + flash (ESP-IDF v6.1)
rem
rem      build_flash.bat              -> build, flash na COM13, monitor
rem      build_flash.bat COM5         -> totez na COM5
rem      build_flash.bat COM13 clean  -> set-target (novy sdkconfig z defaults)
rem      build_flash.bat COM13 nobuild-> jen flash
rem ============================================================================
setlocal
set "PROJ=%~dp0"
set "IDF=C:\esp\v6.1\esp-idf"
set "IDF_TOOLS_PATH=%USERPROFILE%\.espressif"

set "PORT=%~1"
if "%PORT%"=="" set "PORT=COM13"
set "MODE=%~2"

if /i not "%IDF_PATH%"=="%IDF%" goto :activate
where idf.py >nul 2>nul
if errorlevel 1 goto :activate
goto :activated
:activate
echo [*] Aktivuji ESP-IDF z %IDF% ...
set "IDF_PATH="
call "%IDF%\export.bat"
if errorlevel 1 exit /b 1
:activated

if /i "%MODE%"=="nobuild" goto :flash
if /i "%MODE%"=="clean" (
    call idf.py -C "%PROJ%." set-target esp32p4
    if errorlevel 1 goto :err
)
call idf.py -C "%PROJ%." build
if errorlevel 1 goto :err

:flash
call idf.py -C "%PROJ%." -p %PORT% flash monitor
if errorlevel 1 goto :err
exit /b 0

:err
echo [CHYBA] Nektery krok selhal - viz vystup vyse.
exit /b 1
