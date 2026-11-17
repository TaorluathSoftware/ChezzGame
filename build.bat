@echo off
rem Builds Chezz.exe: one self-contained file. Everything under assets\ (piece sets, sounds, the
rem icon) is compiled into the exe, so the exe alone is all anyone needs to download and run.
rem Needs MinGW-w64 g++ and windres on PATH.
setlocal
cd /d "%~dp0"
echo Building Chezz...

powershell -NoProfile -ExecutionPolicy Bypass -File tools\make_rc.ps1
if %errorlevel% neq 0 (
    echo BUILD FAILED: could not list the assets
    exit /b 1
)
windres src\assets_embed.rc -O coff -o src\assets_embed.o
if %errorlevel% neq 0 (
    echo BUILD FAILED: could not compile the assets into the exe
    exit /b 1
)
windres src\icon.rc -O coff -o src\icon.o
if %errorlevel% neq 0 (
    echo BUILD FAILED: could not compile the assets into the exe
    exit /b 1
)

g++ -std=c++17 -O2 -DNDEBUG -Wall -Wno-unused-function -Isrc src\main.cpp src\chess.cpp src\eval.cpp src\nn.cpp src\search.cpp src\levels.cpp src\brain.cpp src\sounds.cpp src\audio.cpp src\json.cpp src\firebase.cpp src\patterns.cpp src\assets_embed.o src\icon.o -o Chezz.exe -mwindows -static -lgdiplus -lgdi32 -luser32 -lwinmm -lshell32 -lole32 -lwinhttp -lcomctl32 -lcomdlg32 -lmfplat -lmfreadwrite -lmfuuid -luuid
if %errorlevel% neq 0 (
    echo BUILD FAILED
    exit /b 1
)
for %%I in (Chezz.exe) do echo Built Chezz.exe (%%~zI bytes, self-contained)
