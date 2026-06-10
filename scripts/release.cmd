@echo off
rem ---------------------------------------------------------------------------
rem  release.cmd  --  build Magnifier, run tests, then assemble a portable
rem  .zip and (if the WiX 4 toolset is available) an .msi installer under
rem  the artifacts\ folder. Used both locally and as the source of truth for
rem  the GitHub Release upload step.
rem ---------------------------------------------------------------------------
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0.." || exit /b 1

rem ---- 1. build ------------------------------------------------------------
call scripts\build.cmd || exit /b 1

rem ---- 2. test -------------------------------------------------------------
.\build\tests\magnifier_tests.exe --gtest_brief=1 1>nul
if errorlevel 1 (
    echo [release] tests FAILED, aborting.
    exit /b 1
)
echo [release] tests passed.

rem ---- 3. version ----------------------------------------------------------
for /f "tokens=*" %%v in ('powershell -NoProfile -Command "(Select-String -Path CMakeLists.txt -Pattern 'VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)').Matches[0].Groups[1].Value"') do set "VER=%%v"
if "%VER%"=="" set "VER=0.1.0"
echo [release] version = %VER%

rem ---- 4. clean artifacts dir ---------------------------------------------
if exist artifacts rmdir /s /q artifacts
mkdir artifacts || exit /b 1

rem ---- 5. portable zip -----------------------------------------------------
set "STAGE=artifacts\stage"
mkdir "%STAGE%" || exit /b 1
copy /Y build\Magnifier.exe                       "%STAGE%\" >nul
copy /Y packaging\portable\config.default.toml     "%STAGE%\config.toml" >nul
copy /Y README.md                                  "%STAGE%\" >nul
copy /Y LICENSE                                    "%STAGE%\" >nul
powershell -NoProfile -Command ^
    "Compress-Archive -Path 'artifacts\stage\*' -DestinationPath 'artifacts\Magnifier-%VER%-win64.zip' -Force" ^
    || exit /b 1
rmdir /s /q "%STAGE%"
echo [release] wrote artifacts\Magnifier-%VER%-win64.zip

rem ---- 6. MSI (optional; requires WiX 4 via dotnet tool) ------------------
where wix >nul 2>nul
if errorlevel 1 (
    where dotnet >nul 2>nul
    if errorlevel 1 (
        echo [release] dotnet not installed; skipping MSI build.
        goto :done
    )
    echo [release] installing WiX 4 ^(one-time^) ...
    dotnet tool install --global wix >nul 2>&1
    if errorlevel 1 (
        echo [release] dotnet tool install wix failed; skipping MSI build.
        goto :done
    )
    rem `dotnet tool install --global` adds to %USERPROFILE%\.dotnet\tools but
    rem cmd inherited PATH may not include it for THIS process. Add it.
    set "PATH=%PATH%;%USERPROFILE%\.dotnet\tools"
)
wix extension add WixToolset.UI.wixext >nul 2>&1
wix build packaging\wix\Magnifier.wxs ^
    -define BinariesDir=build ^
    -bindpath "Binaries=build" ^
    -define Version=%VER% ^
    -arch x64 ^
    -ext WixToolset.UI.wixext ^
    -out "artifacts\Magnifier-%VER%-x64.msi"
if errorlevel 1 (
    echo [release] MSI build FAILED.
    exit /b 1
)
echo [release] wrote artifacts\Magnifier-%VER%-x64.msi

:done
echo.
echo [release] === artifacts ===
dir /b artifacts
endlocal
