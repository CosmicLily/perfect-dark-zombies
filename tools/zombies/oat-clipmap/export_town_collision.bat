@echo off
rem Exports Tranzit's collision (clipmap) with the OpenAssetTools built here,
rem into OpenAssetTools\clipdump\zm_transit\maps\mp\zm_transit.d3dbsp.clip.txt
cd /d "%~dp0"
set ZONES=D:\Games and Necessities\Launchers (Ex. Steam)\Steam\steamapps\common\Call of Duty Black Ops II\zone\all
build\bin\Release_x64\Unlinker.exe -l "%ZONES%\common_zm.ff" --include-assets clipmap -o "%~dp0clipdump\?zone?" "%ZONES%\zm_transit.ff"
echo.
echo Done. You can close this window.
pause
