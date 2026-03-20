@echo off
powershell -ExecutionPolicy Bypass -File "%~dp0check_olivavanilla_code_resume.ps1" -CheckpointNamespace olivavanilla_code %*
