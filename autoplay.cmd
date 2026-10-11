@echo off
rem Unattended gameplay test: plays the game by itself, then writes logs\autoplay\<time>\report.md
rem Usage: autoplay.cmd [minutes] [seed]      e.g.  autoplay.cmd 30 5
set MINUTES=%~1
if "%MINUTES%"=="" set MINUTES=10
set SEED=%~2
if "%SEED%"=="" set SEED=1
pwsh -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\autoplay.ps1" -Minutes %MINUTES% -Seed %SEED%
