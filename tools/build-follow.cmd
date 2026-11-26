@echo off
REM Bouwt MuseScore 3.7 (RelWithDebInfo) met Ninja + MSVC (vcvars) + Qt 5.15.2.
REM Omzeilt de kapotte vswhere-registratie door NIET de VS-generator te gebruiken.
setlocal ENABLEEXTENSIONS

call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 ( echo VCVARS MISLUKT & exit /b 1 )

set "QTDIR=C:\Qt\5.15.2\msvc2019_64"
set "VSCMAKE=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
set "PATH=%QTDIR%\bin;%VSCMAKE%;%PATH%"
set "CMAKE_PREFIX_PATH=%QTDIR%"

cd /d C:\ms3\follow
if not exist bld mkdir bld
cd bld

echo ============================================================
echo  CONFIGUREREN (CMake / Ninja)
echo ============================================================
cmake -G Ninja ^
  -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
  -DCMAKE_INSTALL_PREFIX=C:/ms3/install-follow ^
  -DMUSESCORE_BUILD_CONFIG=dev ^
  -DBUILD_64=ON ^
  -DBUILD_JACK=OFF ^
  -DDOWNLOAD_SOUNDFONT=OFF ^
  -DBUILD_CRASH_REPORTER=OFF ^
  ..
if errorlevel 1 ( echo CONFIGURE MISLUKT & exit /b 1 )

echo ============================================================
echo  COMPILEREN (target mscore)
echo ============================================================
cmake --build . --target mscore
if errorlevel 1 ( echo BUILD MISLUKT & exit /b 1 )

echo ============================================================
echo  INSTALLEREN naar C:\ms3\install-follow
echo ============================================================
cmake --build . --target install
if errorlevel 1 ( echo INSTALL MISLUKT & exit /b 1 )

echo KLAAR-OK
exit /b 0
