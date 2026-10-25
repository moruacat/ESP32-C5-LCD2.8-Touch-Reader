@echo off
setlocal
cd /d "%~dp0"

python -c "import PySide6" >nul 2>&1
if errorlevel 1 (
    echo [EBF Studio] PySide6 is missing for "python".
    echo Install dependencies first:
    echo     python -m pip install -r requirements.txt
    pause
    exit /b 1
)

rem 用和 python 同一个解释器的 pythonw，避免 PATH 里混着别的 Python
for /f "delims=" %%I in ('python -c "import sys,os;print(os.path.join(os.path.dirname(sys.executable),'pythonw.exe'))"') do set "PYW=%%I"
if not exist "%PYW%" set "PYW=pythonw"

start "" "%PYW%" "%~dp0main.py"
