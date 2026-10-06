@echo off
rem Windows launcher (scripts/run_windows.py): builds the port through MSYS2 when
rem needed and starts the game. Usage: run.bat [--game-dir DIR] [bb-probe options...]
rem MSYS2 is expected in C:\msys64 (set BB_MSYS2 otherwise); see README "Windows".
setlocal
call "%~dp0scripts\windows_python.bat" "%~dp0scripts\run_windows.py" %*
exit /b %ERRORLEVEL%
