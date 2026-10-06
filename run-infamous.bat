@echo off
setlocal
cd /d "%~dp0"
if not exist "out\CUSA00309" mkdir "out\CUSA00309"
set "BB_PREBUILT=1"
set "BB_DATA_DIR=%~dp0"
set "BB_PROBE=out\bb-probe.exe"
echo Starting Second Son. Log: %~dp0out\CUSA00309\game-test.log
call "%~dp0run.bat" --game-dir "%~dp0patches\CUSA00309" %* > "%~dp0out\CUSA00309\game-test.log" 2>&1
set "INFAMOUS_EXIT=%ERRORLEVEL%"
echo Game process finished with exit code %INFAMOUS_EXIT%.
echo Log: %~dp0out\CUSA00309\game-test.log
pause
exit /b %INFAMOUS_EXIT%
