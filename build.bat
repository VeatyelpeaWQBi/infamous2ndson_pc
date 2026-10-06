@echo off
setlocal
call "%~dp0scripts\windows_python.bat" "%~dp0scripts\windows_tools.py" build %*
exit /b %ERRORLEVEL%
