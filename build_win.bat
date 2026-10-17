@echo off
rem Run build_win.sh from PowerShell or cmd. It takes the same arguments, e.g. build_win.bat package
rem MSYS2 is expected in C:\msys64; set MSYS2_ROOT to its install directory otherwise.
setlocal

set "MSYS2_DIR=%MSYS2_ROOT%"
if not defined MSYS2_DIR set "MSYS2_DIR=C:\msys64"
if not exist "%MSYS2_DIR%\usr\bin\bash.exe" (
    >&2 echo MSYS2 not found at %MSYS2_DIR%; set MSYS2_ROOT to its install directory.
    exit /b 1
)

rem bash's dirname splits paths only on forward slashes, so hand it the script path with them.
set "SCRIPT=%~dp0build_win.sh"
"%MSYS2_DIR%\usr\bin\bash.exe" "%SCRIPT:\=/%" %*
exit /b %ERRORLEVEL%
