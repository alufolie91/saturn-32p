# SRB2Kart: Saturn - Uranus fork

[SRB2Kart](https://srb2.org/mods/) is a kart racing mod based on the 3D Sonic the Hedgehog fangame [Sonic Robo Blast 2](https://srb2.org/), based on a modified version of [Doom Legacy](http://doomlegacy.sourceforge.net/).

## Uranus Fork
-This fork focuses on things that are not compatible with Vanilla Clients, things like netcode fixes, more players etc.

## Features (Normal Branch)
- Support for up to 32 Players
- Extra Skin colors
- Double the mobjfreeslots (2048 compared to 1024 in vanilla)
- Up to 3 Rows for Map voting
- Adds a 12 player cap to the distance calculation, to make item rng in big lobbies with more than 16 to be less bad (With this change, the distance multiplier is capped at 0.857 instead of 0.714 )
- Everything Saturn client also has See: https://github.com/Indev450/SRB2Kart-Saturn

- ## Features (Experimental Branch)
- Everything from above
- The Player collision system has been redone to be Cylindrical instead of being a Box. This will most likely fix magnet walls, and alot of other issues, esp on maps with complicated geometry
- Interpoints, means Points can be saved between Sessions, also your points can go negative aswell
- More Blua Hooks, For Stealing Bumpers in Battle mode, Hyudoro, Sneakers and Mobj Scales
- Aswell as more Lua things for Intermission
- Double the skin limit (512 instead of 255)
- General Fixes in the netcode. This makes players with high ping less likely to lag out the whole server.
- Synchfixes, maps which do alot of things at Level Load or maps with Wavy floors are less likely to desynch Players
- Also resynching has a higher chance to be succesful in doing so
- Optimized netarchiving for Lua things
- Optimized Polygon sorting with OpenGl batching 

## Dependencies
- NASM (x86 builds only)
- SDL2 (Linux/OS X only)
- SDL2-Mixer (Linux/OS X only)
- libupnp (Linux/OS X only)
- libgme (Linux/OS X only)
- libopenmpt (Linux/OS X only)

## Compiling

See [SRB2 Wiki/Source code compiling](http://wiki.srb2.org/wiki/Source_code_compiling). The compiling process for SRB2Kart is largely identical to SRB2.

## Disclaimer
Kart Krew is in no way affiliated with SEGA or Sonic Team. We do not claim ownership of any of SEGA's intellectual property used in SRB2.
