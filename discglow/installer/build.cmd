@echo off
rem Builds DiscGlowSetup.exe with the DiscGlow DLL embedded (uses the C# compiler that ships with Windows).
rem Build the DLL first with ..\build.cmd.
cd /d "%~dp0"
"%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe" /nologo /target:winexe /platform:x64 /optimize ^
  /out:DiscGlowSetup.exe /win32icon:icon.ico ^
  /resource:..\dinput8.dll,DiscGlow.dll ^
  /r:System.Windows.Forms.dll /r:System.Drawing.dll /r:System.Core.dll ^
  DiscGlowSetup.cs Ui.cs
