@echo off
cd /d "%~dp0"
call C:\esp-idf\export.bat
idf.py -p COM12 -b 115200 monitor
pause