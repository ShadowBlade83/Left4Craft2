@echo off
setlocal
set HERE=%~dp0
set CXX=g++.exe
rem a w64devkit next to the repository, or the one from HL2Craft's folder; else g++ from PATH
if exist "%HERE%..\..\HalfLife2Minecraft\w64devkit\bin\g++.exe" set CXX="%HERE%..\..\HalfLife2Minecraft\w64devkit\bin\g++.exe"
if exist "%HERE%..\..\w64devkit\bin\g++.exe" set CXX="%HERE%..\..\w64devkit\bin\g++.exe"
if exist "%HERE%..\w64devkit\bin\g++.exe" set CXX="%HERE%..\w64devkit\bin\g++.exe"

echo [Left4Craft] Compiling HL2Minecraft.addon32 (Left 4 Dead 2 build)...
%CXX% -m32 -shared -O3 -std=c++20 -static -DWIN32_LEAN_AND_MEAN -DNOMINMAX ^
  -I "%HERE%third_party\reshade" -I "%HERE%src" ^
  "%HERE%src\hl2_bridge.cpp" "%HERE%src\compositor.cpp" "%HERE%src\d3d9_render.cpp" "%HERE%src\addon_entry.cpp" "%HERE%src\l4d2_game.cpp" ^
  -luser32 -lgdi32 -ld3d9 -o "%HERE%HL2Minecraft.addon32"

if %ERRORLEVEL% EQU 0 (
  echo [Left4Craft] Build SUCCESSFUL: %HERE%HL2Minecraft.addon32
  copy /y "%HERE%HL2Minecraft.addon32" "%HERE%..\fabric\src\main\resources\assets\hl2craft\HL2Minecraft.addon32" >nul
  echo [Left4Craft] Bundled addon updated in fabric resources.
) else (
  echo [Left4Craft] Build FAILED!
  exit /b 1
)
