@echo off
rem Builds DiscGlow as dinput8.dll (needs Visual Studio's C++ tools).
setlocal
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist obj mkdir obj
cl /nologo /O2 /MT /EHsc /std:c++17 /W3 /Fo"obj\\" /c discglow.cpp || exit /b 1
cl /nologo /O2 /MT /W3 /Fo"obj\\" /c minhook\buffer.c minhook\hook.c minhook\trampoline.c minhook\hde\hde64.c || exit /b 1
link /nologo /DLL /DEF:discglow.def /OUT:dinput8.dll obj\*.obj user32.lib || exit /b 1
echo Built dinput8.dll
