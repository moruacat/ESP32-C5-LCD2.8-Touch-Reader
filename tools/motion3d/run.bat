@echo off
rem 启动姿态 3D 显示（需要 PyQt5 + numpy）
cd /d "%~dp0"
python motion_view.py
if errorlevel 1 pause
