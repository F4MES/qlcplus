@echo off
rem runde 439 (N9): double-click to update QLC+ and PSMAIN.qxw on the lys-pc
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0update-qlc.ps1" %*
pause
