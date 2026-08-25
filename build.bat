@echo off
rem ============================================================
rem  SwitchProXInput - one-shot build script.
rem  Uses MSVC (cl.exe) if available, otherwise MinGW g++.
rem  For MSVC, run this from a "Developer Command Prompt for VS".
rem
rem  Optional code signing: if the environment variable
rem  SXPX_THUMBPRINT is set to a certificate thumbprint, the build
rem  is signed automatically after compiling (see tools\sign.ps1):
rem      set SXPX_THUMBPRINT=0123456789ABCDEF...
rem      build.bat
rem ============================================================
setlocal
cd /d "%~dp0"

where cl >nul 2>nul
if %errorlevel%==0 goto msvc

where g++ >nul 2>nul
if %errorlevel%==0 goto mingw

echo [ERROR] No compiler found.
echo         Install one of:
echo          - Visual Studio Build Tools ^(Desktop development with C++^)
echo          - MinGW-w64 ^(https://winlibs.com/)
exit /b 1

:msvc
echo Building with MSVC...
rc /nologo app.rc
cl /nologo /EHsc /O2 /W3 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 ^
   main.cpp config.cpp engine.cpp gui.cpp app.res ^
   /link setupapi.lib hid.lib shell32.lib advapi32.lib ^
   /SUBSYSTEM:WINDOWS /out:SwitchProXInput.exe
if %errorlevel%==0 (echo [OK] SwitchProXInput.exe built.) else (echo [ERROR] Build failed. & exit /b 1)
call :maybe_sign
exit /b 0

:mingw
echo Building with MinGW...
where windres >nul 2>nul
if %errorlevel%==0 (
    windres app.rc -O coff app_res.o
    g++ -std=c++17 -O2 -static -mwindows main.cpp config.cpp engine.cpp gui.cpp app_res.o ^
        -o SwitchProXInput.exe -lsetupapi -lhid -lshell32 -ladvapi32
) else (
    g++ -std=c++17 -O2 -static -mwindows main.cpp config.cpp engine.cpp gui.cpp ^
        -o SwitchProXInput.exe -lsetupapi -lhid -lshell32 -ladvapi32
)
if %errorlevel%==0 (echo [OK] SwitchProXInput.exe built.) else (echo [ERROR] Build failed. & exit /b 1)
call :maybe_sign
exit /b 0

:maybe_sign
if "%SXPX_THUMBPRINT%"=="" exit /b 0
echo Signing build with certificate %SXPX_THUMBPRINT% ...
powershell -ExecutionPolicy Bypass -File "%~dp0tools\sign.ps1" -Thumbprint %SXPX_THUMBPRINT%
exit /b
