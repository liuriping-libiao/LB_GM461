@echo off
REM Percipio C++ Service - Compilation Verification Script
REM Purpose: Build and test the project on Windows x86_64
REM Usage: run_compile_test.bat

setlocal enabledelayedexpansion

echo ================================================
echo Percipio C++ Service - Compilation Test
echo ================================================
echo.

set PROJECT_ROOT=c:\Users\zzulr\Desktop\LBGM461
cd /d %PROJECT_ROOT%

echo [1/5] Checking CMake availability...
where cmake >nul 2>&1
if errorlevel 1 (
    echo ERROR: CMake not found. Please ensure CMake is installed and in PATH.
    exit /b 1
)
echo OK: CMake found
echo.

echo [2/5] Checking project structure...
if not exist "CMakeLists.txt" (
    echo ERROR: CMakeLists.txt not found in %PROJECT_ROOT%
    exit /b 1
)
if not exist "include\vcamera" (
    echo ERROR: include\vcamera directory not found. SDK headers not copied?
    exit /b 1
)
if not exist "src\camera\PercipioCamera.cpp" (
    echo ERROR: PercipioCamera.cpp not found
    exit /b 1
)
echo OK: Project structure verified
echo.

echo [3/5] Configuring CMake with windows-debug preset...
cmake --preset windows-debug
if errorlevel 1 (
    echo ERROR: CMake configuration failed
    exit /b 1
)
echo OK: CMake configuration complete
echo.

echo [4/5] Building camera_service executable...
cd /d %PROJECT_ROOT%\out\build\vs2022-x64
cmake --build . --config Debug --target camera_service
if errorlevel 1 (
    echo ERROR: Build failed
    exit /b 1
)
echo OK: Build successful
echo.

echo [5/5] Verifying executable exists...
if not exist "Debug\camera_service.exe" (
    echo ERROR: Executable not generated
    exit /b 1
)
for %%F in (Debug\camera_service.exe) do (
    set size=%%~zF
    echo OK: camera_service.exe created [!size! bytes]
)
echo.

echo ================================================
echo BUILD VERIFICATION COMPLETE
echo ================================================
echo.
echo Next steps:
echo 1. Test synthetic mode:
echo    %PROJECT_ROOT%\out\build\vs2022-x64\Debug\camera_service.exe --synthetic
echo.
echo 2. From C# client, call GetYoloResult on 127.0.0.1:5111
echo.
echo 3. Verify shared memory file at returned path
echo.

exit /b 0
