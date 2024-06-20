# SRB2Kart Planeptune: Custom lua features


# New Stuff from Neptune

## OS library time and date functions

os.clock
os.time
os.date
os.difftime

# New stuff from Saturn:

## mobj.localskin and player.localskin fields

Allow reading and assigning localskins to players. Local skins for other objects aren't supported currently.

## mobj.rollsum field

**Not netsafe**. Returns total sprite rotation value, taking sliptideroll and sloperoll cvars value into account.
Mainly for use in visual-only addons like secondcolor.

## player.sliproll field

**Not netsafe**. Returns player's current sliptide roll angle, taking sliptideroll cvar value into account.

## hud.setVoteBackground(name)

Changes vote background texture prefix (game looks for PREFIX + "W" or "C" (depends on resolution) + FRAME\_NUMBER).

## hud.add(fn, "vote") and hud.add(fn, "intermission")

Hud hooks for vote screen and intermission screen. They only get drawer (`v`) as argument.
To check if those hooks are available, check if globals FEATURE\_VOTEHUD and FEATURE\_INERMISSIONHUD
exist.

## x, y = hud.getOffsets(item)

Returns the values of the given HUD item's `xoffset`/`yoffset` cvars.
Available for any HUD item with offset cvars.

## x, y, flags = v.getDrawInfo(item)

Returns the X, Y and flags where the given HUD item will be drawn for the current displayplayer.
Available for `item`, `gametypeinfo` and `minimap`.

## patch, colormap = v.getColorHudPatch(item)

Returns the patch and colormap to use for the given HUD item. Colorization is based on the user's settings.
Available for `item` and `itemmul`.
Extra arguments for some items:
### item
* `small`: true for small item box, false for big item box.
* `dark`: true to darken item box. Depends on `darkitembox`.
### itemmul
* `small`: true for small sticker, false for big sticker.

## hudcolor = v.getHudColor()

Returns the displayplayer's HUD color.

## colorize = v.useColorHud()

Returns true if colorization is enabled, false otherwise.


## mobj.spritexscale, mobj.spriteyscale, mobj.spritexoffset, mobj.spriteyoffset, mobj.rollangle, mobj.sloperoll, mobj.rollmodel fields

Same fields as in SRB2 2.2

## S\_StopSoundByNum

Allows stopping specific sound globally. Because this function starts with S\_, like state, checking
if it exists is a bit more complicated:

```lua
if rawget(_G, "S_StopSoundByNum") ~= nil then
    ... -- Can use it
end
```

## hud.disable("statdisplay") hud.enable("statdisplay")

New lua hooks to enable/disable new hud element.

## P_IsLocalPlayer

Checks if player is a local player and returns true if valid



## P_ResetCamera

Resets players current camera.


## M_MapNumber

Returns mapnumber.


## P_ButteredSlope

Makes mobj go down slope as if it was made of butter.


## R_PointInSubsector

Used to check if slope is in subsector.

## Lua Debug library

debug.gethook
debug.getinfo
debug.getlocal
debug.getupvalue
debug.sethook
debug.traceback

# Stuff from Uranus

## Interpoints (p.interpoints)

Allows you to specify custom amount of points to give player. Useful for custom scoring systems and the like.

## Mashstop (p.mashstop)

Allows you to disable/enable player item mashing to cause red roulette.

## PlayerItemUse

Allows you to check if a player has used an item, what kind of item and whether or not its out. (Banana or Orinaut)

## KartHyudoro
Allows you to check what player is stealing, what player is being stolen from and whether or not the person was forced rolled a sink.

## KartStealBumper
Allows you to check what player is stealing, what player is being stolen from and whether or not the steal should be forced.

## MobjScaleChange

Changes scale of specific mobj type.

## KartSneaker

Lets you do a sneaker and choose what type visually.

## Titlescreen thinker, HUD hook and hud.disable and hud.enable stuff

New hooks
New "TitleThinker" Thinker
New "title" HUD hook

New set of things that can be enabled/disabled on title screen
"titlecheckl"
"titlecheckr"
"titlelogo"
"titlebanner"
"titleflash"