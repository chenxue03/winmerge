cd /d "%~dp0"

del /s BuildTmp\*.res

setlocal
for /f "usebackq tokens=*" %%i in (`"%programfiles(x86)%\microsoft visual studio\installer\vswhere.exe" -version [16.0^,17.0^) -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
  set InstallDir=%%i
)
if exist "%InstallDir%\Common7\Tools\vsdevcmd.bat" (
  call "%InstallDir%\Common7\Tools\vsdevcmd.bat
)

if "%1" == "" (
  call :BuildBin x86 || goto :eof
  call :BuildBin x64 || goto :eof
  call :BuildBin ARM || goto :eof
  call :BuildBin ARM64 || goto :eof
) else (
  call :BuildBin %1 || goto :eof
)

endlocal

goto :eof

:BuildBin

del /s Build\%1\Release\FRHED\*.exe
MSBuild FRHED.vs2019.sln /t:rebuild /p:Configuration=Release /p:Platform="%1" || pause

if exist "%SIGNBAT_PATH%" (
  call "%SIGNBAT_PATH%" Build\%1\Release\FRHED\frhed.exe
  call "%SIGNBAT_PATH%" Build\%1\Release\FRHED\hekseditU.dll
  call "%SIGNBAT_PATH%" Build\%1\Release\FRHED\Languages\heksedit.lng
)

