@echo off
"%~dp0out\build\local-win-release\BullyRecomp.exe" --game_data_root "%~dp0assets\game" --mnk_mode --allow_game_relative_writes --log_level warning --log_file "%~dp0logs\play.log"
