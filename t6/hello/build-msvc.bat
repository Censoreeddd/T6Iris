@echo off
rem Run from "x86 Native Tools Command Prompt for VS 2022" (32-bit: the game is 32-bit)
cl /nologo /LD /O2 /MT IrisT6Hello.cpp user32.lib psapi.lib /link /DEF:IrisT6Hello.def /OUT:IrisT6Hello.Red32n
