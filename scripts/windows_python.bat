@echo off
rem Reuse Windows Python first; MSYS2 Python is only a fallback.
setlocal
rem Redirected Windows logs must preserve Unicode game titles such as Second Son's TM.
set "PYTHONIOENCODING=utf-8"
if not defined BB_MSYS2 set "BB_MSYS2=C:\msys64"
if defined BB_PYTHON goto configured
rem A running desktop may have a stale process PATH. Read the already-configured
rem system/user paths before adding CLANG64 for native DLLs, which also contains
rem another python.exe. Do not modify persistent environment variables.
for /f "delims=" %%P in ('powershell.exe -NoProfile -Command "$pythonSearchPaths=[Environment]::GetEnvironmentVariable('Path','Machine')+';'+[Environment]::GetEnvironmentVariable('Path','User'); foreach($pythonToolDir in $pythonSearchPaths.Split(';')) { if($pythonToolDir -and $pythonToolDir -notmatch '\\(clang64|mingw64|ucrt64)\\bin' -and $pythonToolDir -notmatch '\\Microsoft\\WindowsApps') { $pythonToolFile=Join-Path $pythonToolDir.Trim([char]34) 'python.exe'; if(Test-Path -LiteralPath $pythonToolFile -PathType Leaf) { $pythonToolFile; break } } }"') do set "BB_PYTHON=%%P"
if defined BB_PYTHON goto configured
rem A dedicated sandbox user cannot see USER's Python launcher registry.
rem Reuse the existing Windows installation if command discovery misses it.
if exist "%LOCALAPPDATA%\Programs\Python\Python312\python.exe" (
    set "BB_PYTHON=%LOCALAPPDATA%\Programs\Python\Python312\python.exe"
    goto configured
)
for /f "delims=" %%P in ('where python 2^>nul') do if not defined BB_PYTHON set "BB_PYTHON=%%P"
if defined BB_PYTHON goto configured
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
rem Process-local DLL search for the existing native executables.
if exist "%BB_MSYS2%\clang64\bin" set "PATH=%BB_MSYS2%\clang64\bin;%PATH%"
"%BB_PYTHON%" %*
exit /b %ERRORLEVEL%

:launcher
py -3 %*
exit /b %ERRORLEVEL%

:msys_python
"%BB_MSYS2%\clang64\bin\python.exe" %*
exit /b %ERRORLEVEL%
