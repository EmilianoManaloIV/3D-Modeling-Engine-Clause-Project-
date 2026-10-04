@echo off
rem Builds Modeler3D for Windows and copies the executable to dist\windows\.
rem Uses CLion's bundled MinGW + CMake + Ninja if CLion is installed, otherwise
rem whatever CMake / g++ (MinGW-w64) or Visual Studio is on the PATH.
setlocal
cd /d "%~dp0"

for /d %%D in ("%ProgramFiles%\JetBrains\CLion*") do (
    if exist "%%D\bin\cmake\win\x64\bin\cmake.exe" set "CLION=%%D"
)
if defined CLION (
    echo Using toolchain from "%CLION%"
    set "PATH=%CLION%\bin\cmake\win\x64\bin;%CLION%\bin\mingw\bin;%CLION%\bin\ninja\win\x64;%PATH%"
)

where cmake >nul 2>nul
if errorlevel 1 (
    echo CMake was not found. Install CLion, Visual Studio, or CMake + MinGW-w64.
    exit /b 1
)

set "GEN="
where ninja >nul 2>nul
if not errorlevel 1 (
    where g++ >nul 2>nul
    if not errorlevel 1 set "GEN=-G Ninja"
)

cmake -S . -B build-release %GEN% -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build build-release --config Release || exit /b 1

if not exist dist\windows mkdir dist\windows
if exist build-release\Modeler3D.exe (
    copy /y build-release\Modeler3D.exe dist\windows\Modeler3D.exe >nul
) else (
    copy /y build-release\Release\Modeler3D.exe dist\windows\Modeler3D.exe >nul
)
echo.
echo Done: dist\windows\Modeler3D.exe  (double-click it to run)
endlocal
