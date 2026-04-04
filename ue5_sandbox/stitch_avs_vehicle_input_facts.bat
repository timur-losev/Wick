@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0stitch_avs_vehicle_input_facts.ps1" %*
