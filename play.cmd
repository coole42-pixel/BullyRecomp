@echo off
rem Settings live in BullyRecomp.ini next to this file (created with defaults on first run).
"%~dp0out\build\local-win-release\BullyRecomp.exe" --game_data_root "%~dp0assets\game" --allow_game_relative_writes --bully_ini "%~dp0BullyRecomp.ini" --log_file "%~dp0logs\play.log"
