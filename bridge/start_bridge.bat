@echo off
rem Start the bridge without a console window. For a console with live log: python bridge.py
cd /d "%~dp0"
start "" pythonw bridge.py
