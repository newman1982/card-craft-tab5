@echo off
rem ============================================================================
rem  Card Craft for M5Stack Tab5 - build + flash
rem
rem  Run from an ESP-IDF v6.x command prompt (export.bat already called).
rem
rem      build_flash.bat <PORT>          -> build, flash, monitor
rem      build_flash.bat <PORT> clean    -> set-target first (fresh sdkconfig)
rem      build_flash.bat <PORT> nobuild  -> flash and monitor only
rem ============================================================================
setlocal
set "PROJ=%~dp0"
set "PORT=%~1"
set "MODE=%~2"

if "%PORT%"=="" (
    echo Usage: build_flash.bat ^<PORT^> [clean^|nobuild]
    exit /b 1
)
where idf.py >nul 2>nul
if errorlevel 1 (
    echo [ERROR] idf.py not found - run ESP-IDF export.bat first.
    exit /b 1
)

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
echo [ERROR] A step failed - see the output above.
exit /b 1
