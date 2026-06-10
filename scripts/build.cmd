@echo off
rem Helper: configure + build the project. Run from anywhere — we cd to the
rem project root (the parent of the scripts/ folder) before invoking cmake.
setlocal
cd /d "%~dp0.." || exit /b 1
set "VS=C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools"
if not exist "%VS%\VC\Auxiliary\Build\vcvars64.bat" (
    rem Fall back to vswhere — covers VS 2022 and custom install paths.
    for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
set "CMAKE=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "NINJA=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
"%CMAKE%" -S . -B build -G Ninja ^
    -DCMAKE_MAKE_PROGRAM="%NINJA%" ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DMAGNIFIER_WARNINGS_AS_ERRORS=OFF ^
    -DMAGNIFIER_BUILD_TESTS=ON || exit /b 1
"%CMAKE%" --build build -j || exit /b 1
endlocal
