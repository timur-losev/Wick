@echo off
powershell -ExecutionPolicy Bypass -File "%~dp0check_olivavanilla_blueprints_resume.ps1" -CheckpointNamespace olivavanilla_blueprints %*
