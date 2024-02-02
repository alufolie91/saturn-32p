// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// Copyright (C) 2022 by Callmore.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  lua_hooklib.h
/// \brief hooks for Lua scripting

#include "doomdef.h"

#ifdef HAVE_BLUA
void lib_expandHookArrays(UINT32 howMuch);
void lib_initHookArrays(void);
#endif
