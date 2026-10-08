@echo off
setlocal
call "%~dp0scripts\windows_python.bat" "%~dp0scripts\runtime_benchmark.py" %*
exit /b %ERRORLEVEL%
