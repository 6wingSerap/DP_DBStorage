@echo off
rem Сборка: окружение MSVC (линкер, SDK) + clang-cl из LLVM.
rem   build.cmd [release|debug|tests]
rem Переменные (необязательные):
rem   VCVARS            путь к vcvars64.bat (иначе ищется vswhere)
rem   GRAFT_SOURCE_DIR  локальный клон KR_GRAFTED (иначе качается тег из CMakeLists)
rem   GTEST_SOURCE_DIR  готовый исходник googletest (иначе качается)
rem   SQLITE_SOURCE_DIR каталог с sqlite3.c и sqlite3.h (иначе качается с sqlite.org)
setlocal
set PRESET=%1
if "%PRESET%"=="" set PRESET=release
if not defined VCVARS (
    for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find VC\Auxiliary\Build\vcvars64.bat`) do set "VCVARS=%%i"
)
if not defined VCVARS echo MSVC не найден: нужен VS Build Tools с C++ workload & exit /b 1
call "%VCVARS%" >nul || exit /b 1
set "PATH=C:\Program Files\LLVM\bin;%PATH%"
cd /d "%~dp0"
set EXTRA=
if defined GRAFT_SOURCE_DIR set EXTRA=%EXTRA% -DGRAFT_SOURCE_DIR=%GRAFT_SOURCE_DIR% -DGRAFT_BOOT_FILE=%GRAFT_SOURCE_DIR%/cmake/graft.boot.cmake
if defined GTEST_SOURCE_DIR set EXTRA=%EXTRA% -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=%GTEST_SOURCE_DIR%
if defined SQLITE_SOURCE_DIR set EXTRA=%EXTRA% -DFETCHCONTENT_SOURCE_DIR_SQLITE=%SQLITE_SOURCE_DIR%
if not exist "build\%PRESET%\CMakeCache.txt" (
    cmake --preset %PRESET% %EXTRA% || exit /b 1
)
cmake --build --preset %PRESET% || exit /b 1
if "%PRESET%"=="tests" ctest --preset tests || exit /b 1
