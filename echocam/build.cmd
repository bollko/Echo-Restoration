@echo off
rem Builds EchoCam.dll (needs Visual Studio's C++ tools). Uses DiscGlow's copy of MinHook.
setlocal
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist obj mkdir obj
cl /nologo /O2 /MT /EHsc /std:c++17 /W3 /Fo"obj\\" /c echocam.cpp panel.cpp || exit /b 1
cl /nologo /O2 /MT /W3 /Fo"obj\\" /c ..\discglow\minhook\buffer.c ..\discglow\minhook\hook.c ..\discglow\minhook\trampoline.c ..\discglow\minhook\hde\hde64.c || exit /b 1
link /nologo /DLL /OUT:EchoCam.dll obj\*.obj user32.lib || exit /b 1
echo Built EchoCam.dll
