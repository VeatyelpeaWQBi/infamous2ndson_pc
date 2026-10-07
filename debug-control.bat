@echo off
setlocal
if "%~1"=="" (
    call "%~dp0scripts\windows_python.bat" "%~dp0scripts\debug_session.py" snapshot
    pause
    exit /b
)
call "%~dp0scripts\windows_python.bat" "%~dp0scripts\debug_session.py" %*
exit /b %ERRORLEVEL%
