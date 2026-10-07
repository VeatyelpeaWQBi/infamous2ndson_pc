@echo off
setlocal
call "%~dp0scripts\windows_python.bat" "%~dp0scripts\performance_sim.py" %*
exit /b %ERRORLEVEL%
