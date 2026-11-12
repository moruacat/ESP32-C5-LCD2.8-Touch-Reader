@echo off
setlocal
cd /d "%~dp0"

set "PYTHONUTF8=1"
python -c "import PySide6" >nul 2>&1
if errorlevel 1 (
    echo [CI Firmware Helper] PySide6 is missing for "python".
    echo Install dependencies first:
    echo     python -m pip install -r requirements.txt
    pause
    exit /b 1
)

for /f "delims=" %%I in ('python -c "import sys,os;print(os.path.join(os.path.dirname(sys.executable),'pythonw.exe'))"') do set "PYW=%%I"
if not exist "%PYW%" set "PYW=pythonw"

start "" "%PYW%" "%~dp0main.py"
