@echo off
setlocal

echo ========================================
echo Configuring project with CMake...
echo ========================================
cmake -S . -B build-jk2-x64 -G "Visual Studio 17 2022" -A x64 ^
-D CMAKE_INSTALL_PREFIX=install-jk2 ^
-DBuildJK2SPEngine=ON ^
-DBuildJK2SPGame=ON ^
-DBuildJK2SPRdVanilla=ON ^
-DBuildSPEngine=OFF ^
-DBuildSPGame=OFF ^
-DBuildSPRdVanilla=OFF

if %errorlevel% neq 0 (
echo CMake configuration failed!
exit /b %errorlevel%
)

echo ========================================
echo Building project...
echo ========================================
cmake --build build-jk2-x64 --config Release --target ALL_BUILD --parallel

if %errorlevel% neq 0 (
echo Build failed!
exit /b %errorlevel%
)

echo ========================================
echo Installing build...
echo ========================================
cmake --install build-jk2-x64 --config Release

if %errorlevel% neq 0 (
echo Install failed!
exit /b %errorlevel%
)

echo ========================================
echo Build and install completed successfully!
echo ========================================

endlocal
