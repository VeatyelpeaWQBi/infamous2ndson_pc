@echo off
rem Reuse Windows Python first; MSYS2 Python is only a fallback.
setlocal
rem Redirected Windows logs must preserve Unicode game titles such as Second Son's TM.
set "PYTHONIOENCODING=utf-8"
rem Keep Python-launched native tests and tools on the existing CLANG64 DLL path.
rem This is process-local; it does not modify the user's system PATH.
if not defined BB_MSYS2 set "BB_MSYS2=C:\msys64"
if exist "%BB_MSYS2%\clang64\bin" set "PATH=%BB_MSYS2%\clang64\bin;%PATH%"
if defined BB_PYTHON goto configured
where python >nul 2>nul
if not errorlevel 1 goto path_python
rem A dedicated sandbox user cannot see USER's Python launcher registry.
rem Reuse the existing Windows installation if command discovery misses it.
if exist "%LOCALAPPDATA%\Programs\Python\Python312\python.exe" (
    set "BB_PYTHON=%LOCALAPPDATA%\Programs\Python\Python312\python.exe"
    goto configured
)
where py >nul 2>nul
if not errorlevel 1 goto launcher
if exist "%BB_MSYS2%\clang64\bin\python.exe" goto msys_python
echo No existing Python found. Configure BB_PYTHON or install Python after approval.
exit /b 1

:configured
if not exist "%BB_PYTHON%" (
    echo Configured BB_PYTHON does not exist: %BB_PYTHON%
    exit /b 1
)
"%BB_PYTHON%" %*
exit /b %ERRORLEVEL%

:launcher
py -3 %*
exit /b %ERRORLEVEL%

:path_python
python %*
exit /b %ERRORLEVEL%

:msys_python
"%BB_MSYS2%\clang64\bin\python.exe" %*
exit /b %ERRORLEVEL%
