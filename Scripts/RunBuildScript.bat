@echo off
setlocal

rem Resolve every path from this batch file so it works from any directory.
set "SCRIPTS_DIRECTORY=%~dp0"
set "PREBUILD_SCRIPT=%SCRIPTS_DIRECTORY%PreBuild.ps1"
set "POSTBUILD_SCRIPT=%SCRIPTS_DIRECTORY%PostBuild.ps1"

rem Use the x64 Debug output by default. Pass another directory as the first
rem argument when updating a different platform or configuration.
if "%~1"=="" (
    set "TARGET_DIRECTORY=%SCRIPTS_DIRECTORY%..\Binaries\x64\Debug"
) else (
    set "TARGET_DIRECTORY=%~1"
)

echo [BuildContent] Running PreBuild scripts...
powershell -NoProfile -ExecutionPolicy Bypass -File "%PREBUILD_SCRIPT%"
if errorlevel 1 (
    echo [BuildContent] PreBuild failed.
    exit /b 1
)

echo [BuildContent] Running PostBuild scripts...
powershell -NoProfile -ExecutionPolicy Bypass -File "%POSTBUILD_SCRIPT%" -TargetDirectory "%TARGET_DIRECTORY%"
if errorlevel 1 (
    echo [BuildContent] PostBuild failed.
    exit /b 1
)

echo [BuildContent] Content update completed: "%TARGET_DIRECTORY%\Content"
exit /b 0
