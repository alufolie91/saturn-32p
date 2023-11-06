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

# Stuff from Uranus


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






