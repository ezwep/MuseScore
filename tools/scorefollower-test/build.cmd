@echo off
REM Bouwt de losse ScoreFollower-test (Meelezen). Uitvoer naar C:\ms3\sftest\.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if not exist C:\ms3\sftest mkdir C:\ms3\sftest
cd /d C:\ms3\follow\tools\scorefollower-test
cl /nologo @flags.txt test.cpp C:\ms3\follow\mscore\scorefollower.cpp /Fo:C:\ms3\sftest\ /Fe:C:\ms3\sftest\sftest.exe /link /FORCE:UNRESOLVED C:\Qt\5.15.2\msvc2019_64\lib\Qt5Core.lib C:\Qt\5.15.2\msvc2019_64\lib\Qt5Gui.lib > C:\ms3\sftest\build.log 2>&1
echo BUILD %errorlevel%
