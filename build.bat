@echo off
gcc src/blink.c src/engine.c src/wren.c -o blink.exe -std=c99 -lgdi32 -luser32 -lwinmm -ldwmapi -ldsound -Os -s -mwindows
