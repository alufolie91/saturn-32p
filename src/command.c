// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// Copyright (C) 1998-2000 by DooM Legacy Team.
// Copyright (C) 1999-2018 by Sonic Team Junior.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  command.c
/// \brief Parse and execute commands from console input/scripts and remote server
///
///        Handles console variables, which is a simplified version
///        of commands, each consvar can have a function called when
///        it is modified.. thus it acts nearly as commands.
///
///        code shamelessly inspired by the QuakeC sources, thanks Id :)

#include "doomdef.h"
#include "doomstat.h"
#include "command.h"
#include "console.h"
#include "z_zone.h"
#include "m_menu.h"
#include "m_misc.h"
#include "m_fixed.h"
#include "m_argv.h"
#include "byteptr.h"
#include "p_saveg.h"
#include "g_game.h" // for player_names
#include "m_cond.h" // for encore mode
#include "d_netcmd.h"
#include "hu_stuff.h"
#include "p_setup.h"
#include "lua_script.h"
#include "d_netfil.h" // findfile

//========
// protos.
//========
static boolean COM_Exists(const char *com_name);
static void COM_ExecuteString(char *com_text);

static void COM_Alias_f(void);
static void COM_Echo_f(void);
static void COM_CEcho_f(void);
static void COM_CEchoFlags_f(void);
static void COM_CEchoDuration_f(void);
static void COM_Exec_f(void);
static void COM_Wait_f(void);
static void COM_Help_f(void);
static void COM_Find_f(void);
static void COM_Toggle_f(void);
static void COM_Add_f(void);

static void CV_EnforceExecVersion(void);
static boolean CV_FilterVarByVersion(consvar_t *v, const char *valstr);

static boolean CV_Command(void);
consvar_t *CV_FindVar(const char *name);
static const char *CV_StringValue(const char *var_name);

static consvar_t *consvar_vars; // list of registered console variables
static UINT16     consvar_number_of_netids = 0;


static char com_token[1024];
static char *COM_Parse(char *data);

CV_PossibleValue_t CV_OnOff[] = {{0, "Off"}, {1, "On"}, {0, NULL}};
CV_PossibleValue_t CV_YesNo[] = {{0, "No"}, {1, "Yes"}, {0, NULL}};
CV_PossibleValue_t CV_Unsigned[] = {{0, "MIN"}, {999999999, "MAX"}, {0, NULL}};
CV_PossibleValue_t CV_Natural[] = {{1, "MIN"}, {999999999, "MAX"}, {0, NULL}};

//SRB2kart
//Expert Speed
CV_PossibleValue_t kartspeed_cons_t[] = {
	{0, "Easy"}, {1, "Normal"}, {2, "Hard"}, {3, "Expert"},
	{0, NULL}};

// Filter consvars by EXECVERSION
// First implementation is 2 (1.0.2), so earlier configs default at 1 (1.0.0)
// Also set CV_HIDEN during runtime, after config is loaded

static boolean execversion_enabled = false;
consvar_t cv_execversion = {"execversion","1",CV_CALL,CV_Unsigned, CV_EnforceExecVersion, 0, NULL, NULL, 0, 0, NULL};

// for default joyaxis detection
#if 0
static boolean joyaxis_default[4] = {false,false,false,false};
static INT32 joyaxis_count[4] = {0,0,0,0};
#endif

#define COM_BUF_SIZE 0x4000 // command buffer size, 0x4000 = 16384
#define MAX_ALIAS_RECURSION 100 // max recursion allowed for aliases

static INT32 com_wait; // one command per frame (for cmd sequences)

// command aliases
//
typedef struct cmdalias_s
{
	struct cmdalias_s *next;
	char *name;
	char *value; // the command string to replace the alias
} cmdalias_t;

static cmdalias_t *com_alias; // aliases list

// =========================================================================
//                            COMMAND BUFFER
// =========================================================================

static vsbuf_t com_text; // variable sized buffer

/** Adds text into the command buffer for later execution.
  *
  * \param ptext The text to add.
  * \sa COM_BufInsertText
  */
void COM_BufAddText(const char *ptext)
{
	size_t l;

	l = strlen(ptext);

	if (com_text.cursize + l >= com_text.maxsize)
	{
		CONS_Alert(CONS_WARNING, M_GetText("Command buffer full!\n"));
		return;
	}
	VS_Write(&com_text, ptext, l);
}

/** Adds command text and executes it immediately.
  *
  * \param ptext The text to execute. A newline is automatically added.
  * \sa COM_BufAddText
  */
void COM_BufInsertText(const char *ptext)
{
	char *temp = NULL;
	size_t templen;

	// copy off any commands still remaining in the exec buffer
	templen = com_text.cursize;
	if (templen)
	{
		temp = M_Memcpy(ZZ_Alloc(templen), com_text.data, templen);
		VS_Clear(&com_text);
	}

	// add the entire text of the file (or alias)
	COM_BufAddText(ptext);
	COM_BufExecute(); // do it right away

	// add the copied off data
	if (templen)
	{
		VS_Write(&com_text, temp, templen);
		Z_Free(temp);
	}
}

/** Progress the wait timer and flush waiting console commands when ready.
  */
void
COM_BufTicker(void)
{
	if (com_wait)
	{
		com_wait--;
		return;
	}

	COM_BufExecute();
}

/** Flushes (executes) console commands in the buffer.
  */
void COM_BufExecute(void)
{
	size_t i;
	char *ptext;
	char line[1024] = "";
	INT32 quotes;

	while (com_text.cursize)
	{
		// find a '\n' or; line break
		ptext = (char *)com_text.data;

		quotes = 0;
		for (i = 0; i < com_text.cursize; i++)
		{
			if (ptext[i] == '\"' && !quotes && i > 0 && ptext[i-1] != ' ') // Malformed command
				break;
			if (ptext[i] == '\"')
				quotes++;
			if (!(quotes & 1) && ptext[i] == ';')
				break; // don't break if inside a quoted string
			if (ptext[i] == '\n' || ptext[i] == '\r')
				break;
		}

		M_Memcpy(line, ptext, i);
		line[i] = 0;

		// flush the command text from the command buffer, _BEFORE_
		// executing, to avoid that 'recursive' aliases overflow the
		// command text buffer, in that case, new commands are inserted
		// at the beginning, in place of the actual, so it doesn't
		// overflow
		if (i == com_text.cursize)
			// the last command was just flushed
			com_text.cursize = 0;
		else
		{
			i++;
			com_text.cursize -= i;
			//memcpy(ptext, ptext+i, com_text.cursize); // Use memmove if the memory areas do overlap.
			memmove(ptext, ptext+i, com_text.cursize);
		}

		// execute the command line
		COM_ExecuteString(line);

		// delay following commands if a wait was encountered
		if (com_wait)
		{
			com_wait--;
			break;
		}
	}
}

/** Executes a string immediately.  Used for skirting around WAIT commands.
  */
void COM_ImmedExecute(const char *ptext)
{
	size_t i = 0, j = 0;
	char line[1024] = "";
	INT32 quotes;

	while (i < strlen(ptext))
	{

		quotes = 0;
		for (j = 0; i < strlen(ptext); i++,j++)
		{
			if (ptext[i] == '\"' && !quotes && i > 0 && ptext[i-1] != ' ') // Malformed command
				return;
			if (ptext[i] == '\"')
				quotes++;
			// don't break if inside a quoted string
			if ((!(quotes & 1) && ptext[i] == ';') || ptext[i] == '\n' || ptext[i] == '\r')
				break;
		}

		memcpy(line, ptext+(i-j), j);
		line[j] = 0;

		// execute the command line
		COM_ExecuteString(line);

		i++; // move to next character
	}
}

// =========================================================================
//                            COMMAND EXECUTION
// =========================================================================

typedef struct xcommand_s
{
	const char *name;
	struct xcommand_s *next;
	com_func_t function;
} xcommand_t;

static xcommand_t *com_commands = NULL; // current commands

#define MAX_ARGS 80
static size_t com_argc;
char *com_argv[MAX_ARGS];
static const char *com_null_string = "";
static char *com_args = NULL; // current command args or NULL

static void Got_NetVar(UINT8 **p, INT32 playernum);

/** Initializes command buffer and adds basic commands.
  */
void COM_Init(void)
{
	// allocate command buffer
	VS_Alloc(&com_text, COM_BUF_SIZE);

	// add standard commands
	COM_AddCommand("alias", COM_Alias_f);
	COM_AddCommand("echo", COM_Echo_f);
	COM_AddCommand("cecho", COM_CEcho_f);
	COM_AddCommand("cechoflags", COM_CEchoFlags_f);
	COM_AddCommand("cechoduration", COM_CEchoDuration_f);
	COM_AddCommand("exec", COM_Exec_f);
	COM_AddCommand("wait", COM_Wait_f);
	COM_AddCommand("help", COM_Help_f);
    COM_AddCommand("find", COM_Find_f);
	COM_AddCommand("toggle", COM_Toggle_f);
	COM_AddCommand("add", COM_Add_f);
	RegisterNetXCmd(XD_NETVAR, Got_NetVar);
}

/** Gets a console command argument count.
  *
  * \return Number of arguments for the last command.
  * \sa COM_Argv
  */
size_t COM_Argc(void)
{
	return com_argc;
}

/** Gets a console command argument.
  *
  * \param arg Index of the argument (0 to COM_Argc() - 1).
  * \return String pointer to the indicated argument.
  * \sa COM_Argc, COM_Args
  */
const char *COM_Argv(size_t arg)
{
	if (arg >= com_argc || (signed)arg < 0)
		return com_null_string;
	return com_argv[arg];
}

/** Returns all of the console command arguments
  *
  * \return String pointer to the argv table.
  * \sa COM_ArgvList
  */
char **COM_ArgvList(void)
{
	return com_argv;
}

/** Gets all console command arguments.
  *
  * \return String pointer to all arguments for the last command.
  * \sa COM_Argv
  */
char *COM_Args(void)
{
	return com_args;
}

/** Checks if a parameter was passed to a console command.
  *
  * \param check The parameter to look for, e.g. "-noerror".
  * \return The index of the argument containing the parameter,
  *         or 0 if the parameter was not found.
  */
size_t COM_CheckParm(const char *check)
{
	size_t i;

	for (i = 1; i < com_argc; i++)
		if (!strcasecmp(check, com_argv[i]))
			return i;
	return 0;
}

/** \brief COM_CheckParm, but checks only the start of each argument.
  *        E.g. checking for "-no" would match "-noerror" too.
  */
size_t COM_CheckPartialParm(const char *check)
{
	int  len;
	size_t i;

	len = strlen(check);

	for (i = 1; i < com_argc; i++)
	{
		if (strncasecmp(check, com_argv[i], len) == 0)
			return i;
	}
	return 0;
}

/** Find the first argument that starts with a hyphen (-).
  * \return The index of the argument, or 0
  *         if there are no such arguments.
  */
size_t COM_FirstOption(void)
{
	size_t i;

	for (i = 1; i < com_argc; i++)
	{
		if (com_argv[i][0] == '-')/* options start with a hyphen */
			return i;
	}
	return 0;
}

/** Parses a string into command-line tokens.
  *
  * \param ptext A null-terminated string. Does not need to be
  *             newline-terminated.
  */
static void COM_TokenizeString(char *ptext)
{
	size_t i;

	// Clear the args from the last string.
	for (i = 0; i < com_argc; i++)
		Z_Free(com_argv[i]);

	com_argc = 0;
	com_args = NULL;

	while (com_argc < MAX_ARGS)
	{
		// Skip whitespace up to a newline.
		while (*ptext != '\0' && *ptext <= ' ' && *ptext != '\n')
			ptext++;

		// A newline means end of command in buffer,
		// thus end of this command's args too.
		if (*ptext == '\n' || *ptext == '\0')
			break;

		if (com_argc == 1)
			com_args = ptext;

		ptext = COM_Parse(ptext);
		if (ptext == NULL)
			break;

		com_argv[com_argc] = Z_StrDup(com_token);
		com_argc++;
	}
}

/** Adds a console command.
  *
  * \param name Name of the command.
  * \param func Function called when the command is run.
  */
void COM_AddCommand(const char *name, com_func_t func)
{
	xcommand_t *cmd;

	// fail if the command is a variable name
	if (CV_StringValue(name)[0] != '\0')
	{
		I_Error("%s is a variable name\n", name);
		return;
	}

	// fail if the command already exists
	for (cmd = com_commands; cmd; cmd = cmd->next)
	{
		if (!stricmp(name, cmd->name)) //case insensitive now that we have lower and uppercase!
		{
			// don't I_Error for Lua commands
			// Lua commands can replace game commands, and they have priority.
			// BUT, if for some reason we screwed up and made two console commands with the same name,
			// it's good to have this here so we find out.
			if (cmd->function != COM_Lua_f)
				I_Error("Command %s already exists\n", name);

			return;
		}
	}

	cmd = ZZ_Alloc(sizeof *cmd);
	cmd->name = name;
	cmd->function = func;
	cmd->next = com_commands;
	com_commands = cmd;
}

/** Adds a console command for Lua.
  * No I_Errors allowed; return a negative code instead.
  *
  * \param name Name of the command.
  */
int COM_AddLuaCommand(const char *name)
{
	xcommand_t *cmd;

	// fail if the command is a variable name
	if (CV_StringValue(name)[0] != '\0')
		return -1;

	// command already exists
	for (cmd = com_commands; cmd; cmd = cmd->next)
	{
		if (!stricmp(name, cmd->name)) //case insensitive now that we have lower and uppercase!
		{
			// replace the built in command.
			cmd->function = COM_Lua_f;
			return 1;
		}
	}

	// Add a new command.
	cmd = ZZ_Alloc(sizeof *cmd);
	cmd->name = name;
	cmd->function = COM_Lua_f;
	cmd->next = com_commands;
	com_commands = cmd;
	return 0;
}

/** Tests if a command exists.
  *
  * \param com_name Name to test for.
  * \return True if a command by the given name exists.
  */
static boolean COM_Exists(const char *com_name)
{
	xcommand_t *cmd;

	for (cmd = com_commands; cmd; cmd = cmd->next)
		if (!stricmp(com_name, cmd->name))
			return true;

	return false;
}

/** Does command completion for the console.
  *
  * \param partial The partial name of the command (potentially).
  * \param skips   Number of commands to skip.
  * \return The complete command name, or NULL.
  * \sa CV_CompleteVar
  */
const char *COM_CompleteCommand(const char *partial, INT32 skips)
{
	xcommand_t *cmd;
	size_t len;

	len = strlen(partial);

	if (!len)
		return NULL;

	// check functions
	for (cmd = com_commands; cmd; cmd = cmd->next)
		if (!strncmp(partial, cmd->name, len))
			if (!skips--)
				return cmd->name;

	return NULL;
}

/** Parses a single line of text into arguments and tries to execute it.
  * The text can come from the command buffer, a remote client, or stdin.
  *
  * \param ptext A single line of text.
  */
static void COM_ExecuteString(char *ptext)
{
	xcommand_t *cmd;
	cmdalias_t *a;
	static INT32 recursion = 0; // detects recursion and stops it if it goes too far

	COM_TokenizeString(ptext);

	// execute the command line
	if (COM_Argc() == 0)
		return; // no tokens

	// check functions
	for (cmd = com_commands; cmd; cmd = cmd->next)
	{
		if (!stricmp(com_argv[0], cmd->name)) //case insensitive now that we have lower and uppercase!
		{
			cmd->function();
			return;
		}
	}

	// check aliases
	for (a = com_alias; a; a = a->next)
	{
		if (!stricmp(com_argv[0], a->name))
		{
			if (recursion > MAX_ALIAS_RECURSION)
				CONS_Alert(CONS_WARNING, M_GetText("Alias recursion cycle detected!\n"));
			else
			{
				char buf[1024];
				char *write = buf, *read = a->value, *seek = read;

				while ((seek = strchr(seek, '$')) != NULL)
				{
					memcpy(write, read, seek-read);
					write += seek-read;

					seek++;

					if (*seek >= '1' && *seek <= '9')
					{
						if (com_argc > (size_t)(*seek - '0'))
						{
							memcpy(write, com_argv[*seek - '0'], strlen(com_argv[*seek - '0']));
							write += strlen(com_argv[*seek - '0']);
						}
						seek++;
					}
					else
					{
						*write = '$';
						write++;
					}

					read = seek;
				}
				WRITESTRING(write, read);

				// Monster Iestyn: keep track of how many levels of recursion we're in
				recursion++;
				COM_BufInsertText(buf);
				recursion--;
			}
			return;
		}
	}

	// check cvars
	// Hurdler: added at Ebola's request ;)
	// (don't flood the console in software mode with bad gr_xxx command)
	if (!CV_Command() && con_destlines)
		CONS_Printf(M_GetText("Unknown command '%s'\n"), COM_Argv(0));
}

// =========================================================================
//                            SCRIPT COMMANDS
// =========================================================================

/** Creates a command name that replaces another command.
  */
static void COM_Alias_f(void)
{
	cmdalias_t *a;
	const char *alias_format = "\x87%-8s:\x80 %s\n";
	int argc = COM_Argc();

	if (argc == 2)
	{
		const char *begin = COM_Argv(1);
		size_t szBegin = strlen(begin);

		/* Display all aliases that start with `begin`. */
		CONS_Printf(M_GetText("All aliases that start with \x87'%s'\x80 are:\n"), begin);

		int count = 0;
		for (cmdalias_t *head = com_alias; head->next != NULL; head = head->next)
		{
			if (strncmp(begin, head->name, szBegin) == 0)
			{
				CONS_Printf(alias_format, head->name, head->value);
				count++;
			}
		}

		if (count == 0)
		{
			CONS_Printf(M_GetText("There are none.\n"));
		}

		return;
	}
	else if (argc < 3)
	{
		/* Display alias subtext, show all aliases. */
		CONS_Printf(M_GetText("alias <name> <command>: create a shortcut command that executes other command(s)\n"));

		for (cmdalias_t *head = com_alias; head->next != NULL; head = head->next)
		{
			CONS_Printf(alias_format, head->name, head->value);
		}

		return;
	}

	a = ZZ_Alloc(sizeof *a);
	a->next = com_alias;
	com_alias = a;

	a->name = Z_StrDup(COM_Argv(1));
	// Just use arg 2 if it's the only other argument, in case the alias is wrapped in quotes (backward compat, or multiple commands in one string).
	// Otherwise pull the whole string and seek to the end of the alias name. The strctr is in case the alias is quoted.
	a->value = Z_StrDup(COM_Argc() == 3 ? COM_Argv(2) : (strchr(COM_Args() + strlen(a->name), ' ') + 1));
}

/** Prints a line of text to the console.
  */
static void COM_Echo_f(void)
{
	size_t i;

	for (i = 1; i < COM_Argc(); i++)
		CONS_Printf("%s ", COM_Argv(i));
	CONS_Printf("\n");
}

/** Displays text on the center of the screen for a short time.
  */
static void COM_CEcho_f(void)
{
	size_t i;
	char cechotext[1024] = "";

	for (i = 1; i < COM_Argc(); i++)
	{
		strncat(cechotext, COM_Argv(i), sizeof(cechotext)-1);
		strncat(cechotext, " ", sizeof(cechotext)-1);
	}

	cechotext[sizeof(cechotext) - 1] = '\0';

	HU_DoCEcho(cechotext);
}

/** Sets drawing flags for the CECHO command.
  */
static void COM_CEchoFlags_f(void)
{
	if (COM_Argc() > 1)
	{
		const char *arg = COM_Argv(1);

		if (arg[0] && arg[0] == '0' &&
			arg[1] && arg[1] == 'x') // Use hexadecimal!
			HU_SetCEchoFlags(axtoi(arg+2));
		else
			HU_SetCEchoFlags(atoi(arg));
	}
	else
		CONS_Printf(M_GetText("cechoflags <flags>: set CEcho flags, prepend with 0x to use hexadecimal\n"));
}

/** Sets the duration for CECHO commands to stay on the screen
  */
static void COM_CEchoDuration_f(void)
{
	if (COM_Argc() > 1)
		HU_SetCEchoDuration(atoi(COM_Argv(1)));
}

/** Executes a script file.
  */
static void COM_Exec_f(void)
{
	UINT8 *buf = NULL;
	char filename[256];

	if (COM_Argc() < 2 || COM_Argc() > 3)
	{
		CONS_Printf(M_GetText("exec <filename>: run a script file\n"));
		return;
	}

	// load file
	// Try with Argv passed verbatim first, for back compat
	FIL_ReadFile(COM_Argv(1), &buf);

	if (!buf)
	{
		// Now try by searching the file path
		// filename is modified with the full found path
		strcpy(filename, COM_Argv(1));
		if (findfile(filename, NULL, true) != FS_NOTFOUND)
			FIL_ReadFile(filename, &buf);

		if (!buf)
		{
			if (!COM_CheckParm("-noerror"))
				CONS_Printf(M_GetText("couldn't execute file %s\n"), COM_Argv(1));
			return;
		}
	}

	if (!COM_CheckParm("-silent"))
		CONS_Printf(M_GetText("executing %s\n"), COM_Argv(1));

	// insert text file into the command buffer
	COM_BufAddText((char *)buf);
	COM_BufAddText("\n");

	// free buffer
	Z_Free(buf);
}

/** Delays execution of the rest of the commands until the next frame.
  * Allows sequences of commands like "jump; fire; backward".
  */
static void COM_Wait_f(void)
{
	if (COM_Argc() > 1)
		com_wait = atoi(COM_Argv(1));
	else
		com_wait = 1; // 1 frame
}

/** Prints help on variables and commands.
  */
static void COM_Help_f(void)
{
	xcommand_t *cmd;
	consvar_t *cvar;
	INT32 i = 0;

	if (COM_Argc() > 1)
	{
		const char *help = COM_Argv(1);
		cvar = CV_FindVar(help);
		if (cvar)
		{
			CONS_Printf("\x82""Variable %s:\n", cvar->name);
			CONS_Printf(M_GetText("  flags: "));
			if (cvar->flags & CV_SAVE)
				CONS_Printf("AUTOSAVE ");
			if (cvar->flags & CV_FLOAT)
				CONS_Printf("FLOAT ");
			if (cvar->flags & CV_NETVAR)
				CONS_Printf("NETVAR ");
			if (cvar->flags & CV_CALL)
				CONS_Printf("ACTION ");
			if (cvar->flags & CV_CHEAT)
				CONS_Printf("CHEAT ");
			CONS_Printf("\n");
			if (cvar->PossibleValue)
			{
				if (!stricmp(cvar->PossibleValue[0].strvalue, "MIN") && !stricmp(cvar->PossibleValue[1].strvalue, "MAX"))
				{
					CONS_Printf("  range from %d to %d\n", cvar->PossibleValue[0].value,
						cvar->PossibleValue[1].value);
					i = 2;
				}

				{
					const char *cvalue = NULL;
					//CONS_Printf(M_GetText("  possible value : %s\n"), cvar->name);
					while (cvar->PossibleValue[i].strvalue)
					{
						CONS_Printf("  %-2d : %s\n", cvar->PossibleValue[i].value,
							cvar->PossibleValue[i].strvalue);
						if (cvar->PossibleValue[i].value == cvar->value)
							cvalue = cvar->PossibleValue[i].strvalue;
						i++;
					}
					if (cvalue)
						CONS_Printf(" Current value: %s\n", cvalue);
					else
						CONS_Printf(" Current value: %d\n", cvar->value);
				}
			}
			else
				CONS_Printf(" Current value: %d\n", cvar->value);
		}
		else
		{
			for (cmd = com_commands; cmd; cmd = cmd->next)
			{
				if (strcmp(cmd->name, help))
					continue;

				CONS_Printf("\x82""Command %s:\n", cmd->name);
				CONS_Printf("  help is not available for commands");
				CONS_Printf("\x82""\nCheck wiki.srb2.org for more or try typing <name> without arguments\n");
				return;
			}

			CONS_Printf("No variable or command named %s", help);
			CONS_Printf("\x82""\nCheck wiki.srb2.org for more or try typing help without arguments\n");

		}
		return;
	}

	{
		// commands
		CONS_Printf("\x82""Commands:\n");
		for (cmd = com_commands; cmd; cmd = cmd->next)
		{
			CONS_Printf("%s ",cmd->name);
			i++;
		}

		// variables
		CONS_Printf("\x82""\nVariables:\n");
		for (cvar = consvar_vars; cvar; cvar = cvar->next)
		{
			if (cvar->flags & CV_NOSHOWHELP)
				continue;
			CONS_Printf("%s ", cvar->name);
			i++;
		}

		CONS_Printf("\x82""\nCheck wiki.srb2.org for more or type help <command or variable>\n");

		CONS_Debug(DBG_GAMELOGIC, "\x82Total : %d\n", i);
	}
}


static void COM_Find_f(void)
{
	static char prefix[80];
	xcommand_t *cmd;
	consvar_t *cvar;
	cmdalias_t *alias;
	const char *match;
	const char *help;
	size_t helplen;
	boolean matchesany;

	if (COM_Argc() != 2)
	{
		CONS_Printf(M_GetText("find <text>: Search for variables, commands and aliases containing <text>\n"));
		return;
	}

	help = COM_Argv(1);
	helplen = strlen(help);
	CONS_Printf("\x82""Variables:\n");
	matchesany = false;
	for (cvar = consvar_vars; cvar; cvar = cvar->next)
	{
		if (cvar->flags & CV_NOSHOWHELP)
			continue;
		match = strstr(cvar->name, help);
		if (match != NULL)
		{
			memcpy(prefix, cvar->name, match - cvar->name);
			prefix[match - cvar->name] = '\0';
			CONS_Printf("  %s\x83%s\x80%s\n", prefix, help, &match[helplen]);
			matchesany = true;
		}
	}
	if (!matchesany)
		CONS_Printf("  (none)\n");

	CONS_Printf("\x82""Commands:\n");
	matchesany = false;
	for (cmd = com_commands; cmd; cmd = cmd->next)
	{
		match = strstr(cmd->name, help);
		if (match != NULL)
		{
			memcpy(prefix, cmd->name, match - cmd->name);
			prefix[match - cmd->name] = '\0';
			CONS_Printf("  %s\x83%s\x80%s\n", prefix, help, &match[helplen]);
			matchesany = true;
		}
	}
	if (!matchesany)
		CONS_Printf("  (none)\n");

	CONS_Printf("\x82""Aliases:\n");
	matchesany = false;
	for (alias = com_alias; alias; alias = alias->next)
	{
		match = strstr(alias->name, help);
		if (match != NULL)
		{
			memcpy(prefix, alias->name, match - alias->name);
			prefix[match - alias->name] = '\0';
			CONS_Printf("  %s\x83%s\x80%s\n", prefix, help, &match[helplen]);
			matchesany = true;
		}
	}
	if (!matchesany)
		CONS_Printf("  (none)\n");
}


/** Toggles a console variable. Useful for on/off values.
  *
  * This works on on/off, yes/no values only
  */
static void COM_Toggle_f(void)
{
	consvar_t *cvar;

	if (COM_Argc() != 2)
	{
		CONS_Printf(M_GetText("Toggle <cvar_name>: Toggle the value of a cvar\n"));
		return;
	}
	cvar = CV_FindVar(COM_Argv(1));
	if (!cvar)
	{
		CONS_Alert(CONS_NOTICE, M_GetText("%s is not a cvar\n"), COM_Argv(1));
		return;
	}

	if (!(cvar->PossibleValue == CV_YesNo || cvar->PossibleValue == CV_OnOff))
	{
		CONS_Alert(CONS_NOTICE, M_GetText("%s is not a boolean value\n"), COM_Argv(1));
		return;
	}

	// netcvar don't change imediately
	cvar->flags |= CV_SHOWMODIFONETIME;
	CV_AddValue(cvar, +1);
}

/** Command variant of CV_AddValue
  */
static void COM_Add_f(void)
{
	consvar_t *cvar;

	if (COM_Argc() != 3)
	{
		CONS_Printf(M_GetText("Add <cvar_name> <value>: Add to the value of a cvar. Negative values work too!\n"));
		return;
	}
	cvar = CV_FindVar(COM_Argv(1));
	if (!cvar)
	{
		CONS_Alert(CONS_NOTICE, M_GetText("%s is not a cvar\n"), COM_Argv(1));
		return;
	}

	if (( cvar->flags & CV_FLOAT ))
		CV_Set(cvar, va("%f", FIXED_TO_FLOAT (cvar->value) + atof(COM_Argv(2))));
	else
		CV_AddValue(cvar, atoi(COM_Argv(2)));
}

// =========================================================================
//                      VARIABLE SIZE BUFFERS
// =========================================================================

/** Initializes a variable size buffer.
  *
  * \param buf      Buffer to initialize.
  * \param initsize Initial size for the buffer.
  */
void VS_Alloc(vsbuf_t *buf, size_t initsize)
{
#define VSBUFMINSIZE 256
	if (initsize < VSBUFMINSIZE)
		initsize = VSBUFMINSIZE;
	buf->data = Z_Malloc(initsize, PU_STATIC, NULL);
	buf->maxsize = initsize;
	buf->cursize = 0;
#undef VSBUFMINSIZE
}

/** Frees a variable size buffer.
  *
  * \param buf Buffer to free.
  */
void VS_Free(vsbuf_t *buf)
{
	buf->cursize = 0;
}

/** Clears a variable size buffer.
  *
  * \param buf Buffer to clear.
  */
void VS_Clear(vsbuf_t *buf)
{
	buf->cursize = 0;
}

/** Makes sure a variable size buffer has enough space for data of a
  * certain length.
  *
  * \param buf    The buffer. It is enlarged if necessary.
  * \param length The length of data we need to add.
  * \return Pointer to where the new data can go.
  */
void *VS_GetSpace(vsbuf_t *buf, size_t length)
{
	void *data;

	if (buf->cursize + length > buf->maxsize)
	{
		if (!buf->allowoverflow)
			I_Error("overflow 111");

		if (length > buf->maxsize)
			I_Error("overflow l%s 112", sizeu1(length));

		buf->overflowed = true;
		CONS_Printf("VS buffer overflow");
		VS_Clear(buf);
	}

	data = buf->data + buf->cursize;
	buf->cursize += length;

	return data;
}

/** Copies data to the end of a variable size buffer.
  *
  * \param buf    The buffer.
  * \param data   The data to copy.
  * \param length The length of the data.
  * \sa VS_Print
  */
void VS_Write(vsbuf_t *buf, const void *data, size_t length)
{
	M_Memcpy(VS_GetSpace(buf, length), data, length);
}

/** Prints text in a variable buffer. Like VS_Write() plus a
  * trailing NUL.
  *
  * \param buf  The buffer.
  * \param data The NUL-terminated string.
  * \sa VS_Write
  */
void VS_Print(vsbuf_t *buf, const char *data)
{
	size_t len;

	len = strlen(data) + 1;

	if (buf->data[buf->cursize-1])
		M_Memcpy((UINT8 *)VS_GetSpace(buf, len), data, len); // no trailing 0
	else
		M_Memcpy((UINT8 *)VS_GetSpace(buf, len-1) - 1, data, len); // write over trailing 0
}

// =========================================================================
//
//                           CONSOLE VARIABLES
//
//   console variables are a simple way of changing variables of the game
//   through the console or code, at run time.
//
//   console vars acts like simplified commands, because a function can be
//   attached to them, and called whenever a console var is modified
//
// =========================================================================

static const char *cv_null_string = "";

/** Searches if a variable has been registered.
  *
  * \param name Variable to search for.
  * \return Pointer to the variable if found, or NULL.
  * \sa CV_FindNetVar
  */
consvar_t *CV_FindVar(const char *name)
{
	consvar_t *cvar;

	for (cvar = consvar_vars; cvar; cvar = cvar->next)
		if (!stricmp(name,cvar->name))
			return cvar;

	return NULL;
}


/** Finds a net variable based on its identifier number.
  *
  * \param netid The variable's identifier number.
  * \return A pointer to the variable itself if found, or NULL.
*/
static consvar_t *CV_FindNetVar(UINT16 netid)
{
	consvar_t *cvar;
	
	if (netid > consvar_number_of_netids)
		return NULL;


	for (cvar = consvar_vars; cvar; cvar = cvar->next)
		if (cvar->netid == netid)
			return cvar;


	return NULL;
}

static void Setvalue(consvar_t *var, const char *valstr, boolean stealth);

/** Registers a variable for later use from the console.
  *
  * \param variable The variable to register.
  */
void CV_RegisterVar(consvar_t *variable)
{
	//static UINT16 id = 1;
	// first check to see if it has already been defined
	if (CV_FindVar(variable->name))
	{
		CONS_Printf(M_GetText("Variable %s is already defined\n"), variable->name);
		return;
	}

	// check for overlap with a command
	if (COM_Exists(variable->name))
	{
		CONS_Printf(M_GetText("%s is a command name\n"), variable->name);
		return;
	}

	// check net variables
	if (variable->flags & CV_NETVAR)
	{
		/* in case of overflow... */
		if (consvar_number_of_netids + 1 < consvar_number_of_netids)
			I_Error("Way too many netvars");

		variable->netid = ++consvar_number_of_netids;
	}

	// link the variable in
	if (!(variable->flags & CV_HIDEN))
	{
		variable->next = consvar_vars;
		consvar_vars = variable;
	}
	variable->string = variable->zstring = NULL;
	variable->changed = 0; // new variable has not been modified by the user

#ifdef PARANOIA
	if ((variable->flags & CV_NOINIT) && !(variable->flags & CV_CALL))
		I_Error("variable %s has CV_NOINIT without CV_CALL\n", variable->name);
	if ((variable->flags & CV_CALL) && !variable->func)
		I_Error("variable %s has CV_CALL without a function\n", variable->name);
#endif

	if (variable->flags & CV_NOINIT)
		variable->flags &= ~CV_CALL;

	Setvalue(variable, variable->defaultvalue, false);

	if (variable->flags & CV_NOINIT)
		variable->flags |= CV_CALL;

	// the SetValue will set this bit
	variable->flags &= ~CV_MODIFIED;
}

/** Finds the string value of a console variable.
  *
  * \param var_name The variable's name.
  * \return The string value or "" if the variable is not found.
  */
static const char *CV_StringValue(const char *var_name)
{
	consvar_t *var;

	var = CV_FindVar(var_name);
	if (!var)
		return cv_null_string;
	return var->string;
}

/** Completes the name of a console variable.
  *
  * \param partial The partial name of the variable (potentially).
  * \param skips   Number of variables to skip.
  * \return The complete variable name, or NULL.
  * \sa COM_CompleteCommand
  */
const char *CV_CompleteVar(char *partial, INT32 skips)
{
	consvar_t *cvar;
	size_t len;

	len = strlen(partial);

	if (!len)
		return NULL;

	// check variables
	for (cvar = consvar_vars; cvar; cvar = cvar->next)
	{
		if (cvar->flags & CV_NOSHOWHELP)
			continue;
		if (strncmp(partial, cvar->name, len))
			continue;
		if (skips--)
			continue;
		return cvar->name;
	}

	return NULL;
}

/** Sets a value to a variable with less checking. Only for internal use.
  *
  * \param var    Variable to set.
  * \param valstr String value for the variable.
  */
static void Setvalue(consvar_t *var, const char *valstr, boolean stealth)
{
	boolean override = false;
	INT32 overrideval = 0;

	// If we want messages informing us if cheats have been enabled or disabled,
	// we need to rework the consvars a little bit.  This call crashes the game
	// on load because not all variables will be registered at that time.
/*	boolean prevcheats = false;
	if (var->flags & CV_CHEAT)
		prevcheats = CV_CheatsEnabled(); */

	if (var->PossibleValue)
	{
		INT32 v;

		if (var->flags & CV_FLOAT)
		{
			double d = atof(valstr);
			if (fpclassify(d) == FP_ZERO && valstr[0] != '0')
				v = INT32_MIN;
			else
				v = (INT32)(d * FRACUNIT);
		}
		else
		{
			v = atoi(valstr);
			if (!v && valstr[0] != '0')
				v = INT32_MIN; // Invalid integer trigger
		}

		if (var->PossibleValue[0].strvalue && !stricmp(var->PossibleValue[0].strvalue, "MIN")) // bounded cvar
		{
#define MINVAL 0
#define MAXVAL 1
			INT32 i;

#ifdef PARANOIA
			if (!var->PossibleValue[MAXVAL].strvalue)
				I_Error("Bounded cvar \"%s\" without maximum!\n", var->name);
#endif

			// search for other
			for (i = MAXVAL+1; var->PossibleValue[i].strvalue; i++)
				if (v == var->PossibleValue[i].value || !stricmp(var->PossibleValue[i].strvalue, valstr))
				{
					var->value = var->PossibleValue[i].value;
					var->string = var->PossibleValue[i].strvalue;
					goto finish;
				}

			if ((v != INT32_MIN && v < var->PossibleValue[MINVAL].value) || !stricmp(valstr, "MIN"))
			{
				v = var->PossibleValue[MINVAL].value;
				valstr = var->PossibleValue[MINVAL].strvalue;
				override = true;
				overrideval = v;
			}
			else if ((v != INT32_MIN && v > var->PossibleValue[MAXVAL].value) || !stricmp(valstr, "MAX"))
			{
				v = var->PossibleValue[MAXVAL].value;
				valstr = var->PossibleValue[MAXVAL].strvalue;
				override = true;
				overrideval = v;
			}
			if (v == INT32_MIN)
				goto badinput;
#undef MINVAL
#undef MAXVAL
		}
		else
		{
			INT32 i;

			// check first strings
			for (i = 0; var->PossibleValue[i].strvalue; i++)
				if (!stricmp(var->PossibleValue[i].strvalue, valstr))
					goto found;
			if (v != INT32_MIN)
			{
				// check INT32 now
				for (i = 0; var->PossibleValue[i].strvalue; i++)
					if (v == var->PossibleValue[i].value)
						goto found;
			}
			// Not found ... but wait, there's hope!
			if (var->PossibleValue == CV_OnOff || var->PossibleValue == CV_YesNo)
			{
				overrideval = -1;
				if (!stricmp(valstr, "on") || !stricmp(valstr, "yes"))
					overrideval = 1;
				else if (!stricmp(valstr, "off") || !stricmp(valstr, "no"))
					overrideval = 0;

				if (overrideval != -1)
				{
					for (i = 0; var->PossibleValue[i].strvalue; i++)
						if (overrideval == var->PossibleValue[i].value)
							goto found;
				}
			}

			// ...or not.
			goto badinput;
found:
			var->value = var->PossibleValue[i].value;
			var->string = var->PossibleValue[i].strvalue;
			goto finish;
		}
	}

	// free the old value string
	Z_Free(var->zstring);

	var->string = var->zstring = Z_StrDup(valstr);

	if (override)
		var->value = overrideval;
	else if (var->flags & CV_FLOAT)
	{
		double d = atof(var->string);
		var->value = (INT32)(d * FRACUNIT);
	}
	else
		var->value = atoi(var->string);

finish:
	if (var->flags & CV_SHOWMODIFONETIME || var->flags & CV_SHOWMODIF)
	{
		CONS_Printf(M_GetText("%s set to %s\n"), var->name, var->string);
		var->flags &= ~CV_SHOWMODIFONETIME;
	}
	else // display message in debug file only
	{
		DEBFILE(va("%s set to %s\n", var->name, var->string));
	}
	var->flags |= CV_MODIFIED;

	// raise 'on change' code
	LUA_CVarChanged(var->name); // let consolelib know what cvar this is.

	if (var->flags & CV_CALL && !stealth)
		var->func();

	return;

// landing point for possiblevalue failures
badinput:

	if (var != &cv_nextmap) // Suppress errors for cv_nextmap
		CONS_Printf(M_GetText("\"%s\" is not a possible value for \"%s\"\n"), valstr, var->name);

	// default value not valid... ?!
	if (var->defaultvalue == valstr)
		I_Error("Variable %s default value \"%s\" is not a possible value\n", var->name, var->defaultvalue);
}

//
// Use XD_NETVAR argument:
//      2 byte for variable identification
//      then the value of the variable followed with a 0 byte (like str)
//

static boolean serverloading = false;

static void Got_NetVar(UINT8 **p, INT32 playernum)
{
	consvar_t *cvar;
	UINT16 netid;
	char *svalue;
	UINT8 stealth = false;

	if (playernum != serverplayer && !IsPlayerAdmin(playernum) && !serverloading)
	{
		// not from server or remote admin, must be hacked/buggy client
		CONS_Alert(CONS_WARNING, M_GetText("Illegal netvar command received from %s\n"), player_names[playernum]);

		if (server)
		{
			UINT8 buf[2];

			buf[0] = (UINT8)playernum;
			buf[1] = KICK_MSG_CON_FAIL;
			SendNetXCmd(XD_KICK, &buf, 2);
		}
		return;
	}
	netid = READUINT16(*p);
	cvar = CV_FindNetVar(netid);
	svalue = (char *)*p;
	SKIPSTRING(*p);
	stealth = READUINT8(*p);

	if (!cvar)
	{
		CONS_Alert(CONS_WARNING, "Netvar not found with netid %hu\n", netid);
		return;
	}
	DEBFILE(va("Netvar received: %s [netid=%d] value %s\n", cvar->name, netid, svalue));

	Setvalue(cvar, svalue, stealth);
}

void CV_SaveNetVars(UINT8 **p, boolean isdemorecording)
{
	consvar_t *cvar;
	UINT8 *count_p = *p;
	UINT16 count = 0;

	// send only changed cvars ...
	// the client will reset all netvars to default before loading
	WRITEUINT16(*p, 0x0000);
	for (cvar = consvar_vars; cvar; cvar = cvar->next)
		if (((cvar->flags & CV_NETVAR) && !CV_IsSetToDefault(cvar)) || (isdemorecording && cvar->netid == cv_numlaps.netid))
		{
			WRITEUINT16(*p, cvar->netid);

			// UGLY HACK: Save proper lap count in net replays
			if (isdemorecording && cvar->netid == cv_numlaps.netid)
			{
				if (cv_basenumlaps.value &&
					(!(mapheaderinfo[gamemap - 1]->levelflags & LF_SECTIONRACE)
					|| (mapheaderinfo[gamemap - 1]->numlaps > cv_basenumlaps.value))
				)
				{
					WRITESTRING(*p, cv_basenumlaps.string);
				}
				else
				{
					char buf[9];
					sprintf(buf, "%d", mapheaderinfo[gamemap - 1]->numlaps);
					WRITESTRING(*p, buf);
				}
			}
			else
			{
				WRITESTRING(*p, cvar->string);
			}

			WRITEUINT8(*p, false);
			++count;
		}
	WRITEUINT16(count_p, count);
}

void CV_LoadNetVars(UINT8 **p)
{
	consvar_t *cvar;
	UINT16 count;

	// prevent "invalid command received"
	serverloading = true;

	for (cvar = consvar_vars; cvar; cvar = cvar->next)
		if (cvar->flags & CV_NETVAR)
			Setvalue(cvar, cvar->defaultvalue, true);

	count = READUINT16(*p);
	while (count--)
		Got_NetVar(p, 0);

	serverloading = false;
}

static void CV_SetCVar(consvar_t *var, const char *value, boolean stealth);

void CV_ResetCheatNetVars(void)
{
	consvar_t *cvar;

	// Stealthset everything back to default.
	for (cvar = consvar_vars; cvar; cvar = cvar->next)
		if (cvar->flags & CV_CHEAT)
			CV_SetCVar(cvar, cvar->defaultvalue, true);
}

// Returns true if the variable's current value is its default value
boolean CV_IsSetToDefault(consvar_t *v)
{
	return (!(strcmp(v->defaultvalue, v->string)));
}

// If any cheats CVars are not at their default settings, return true.
// Else return false.
// This returns a UINT8 because I'm too lazy to deal with the packet structure.
// Deal with it. =P
UINT8 CV_CheatsEnabled(void)
{
	consvar_t *cvar;

	for (cvar = consvar_vars; cvar; cvar = cvar->next)
		if ((cvar->flags & CV_CHEAT) && strcmp(cvar->defaultvalue, cvar->string))
			return 1;
	return 0;
}

/** Sets a value to a variable, performing some checks and calling the
  * callback function if there is one.
  * Does as if "<varname> <value>" is entered at the console.
  *
  * \param var   The variable.
  * \param value The string value.
  * \sa CV_StealthSet, CV_SetValue
  */
static void CV_SetCVar(consvar_t *var, const char *value, boolean stealth)
{
#ifdef PARANOIA
	if (!var)
		I_Error("CV_Set: no variable\n");
	if (!var->string)
		I_Error("CV_Set: %s no string set!\n", var->name);
#endif
	if (!var || !var->string || !value || !stricmp(var->string, value))
		return; // no changes

	if (var->flags & CV_NETVAR)
	{
		// send the value of the variable
		UINT8 buf[128];
		UINT8 *p = buf;
		if (!(server || (IsPlayerAdmin(consoleplayer))))
		{
			CONS_Printf(M_GetText("Only the server or admin can change: %s %s\n"), var->name, var->string);
			return;
		}

		if (var == &cv_kartencore && !M_SecretUnlocked(SECRET_ENCORE))
		{
			CONS_Printf(M_GetText("You haven't unlocked Encore Mode yet!\n"));
			return;
		}

		if (var == &cv_kartspeed && !M_SecretUnlocked(SECRET_HARDSPEED))
		{
			if (!stricmp(value, "Hard") || atoi(value) == 2)
			{
				CONS_Printf(M_GetText("You haven't unlocked this yet!\n"));
				return;
			}
		}

		// Only add to netcmd buffer if in a netgame, otherwise, just change it.
		if (netgame || multiplayer)
		{
			WRITEUINT16(p, var->netid);
			WRITESTRING(p, value);
			WRITEUINT8(p, stealth);

			SendNetXCmd(XD_NETVAR, buf, p-buf);
		}
		else
			Setvalue(var, value, stealth);
	}
	else
		if ((var->flags & CV_NOTINNET) && netgame)
		{
			CONS_Printf(M_GetText("This variable can't be changed while in netgame: %s %s\n"), var->name, var->string);
			return;
		}
		else
			Setvalue(var, value, stealth);
}

/** Sets a value to a variable without calling its callback function.
  *
  * \param var   The variable.
  * \param value The string value.
  * \sa CV_Set, CV_StealthSetValue
  */
void CV_StealthSet(consvar_t *var, const char *value)
{
	CV_SetCVar(var, value, true);
}

/** Sets a numeric value to a variable without calling its callback
  * function.
  *
  * \param var   The variable.
  * \param value The numeric value, converted to a string before setting.
  * \sa CV_SetValue, CV_StealthSet
  */
void CV_StealthSetValue(consvar_t *var, INT32 value)
{
	char val[32];

	sprintf(val, "%d", value);
	CV_SetCVar(var, val, true);
}

// New wrapper for what used to be CV_Set()
void CV_Set(consvar_t *var, const char *value)
{
	CV_SetCVar(var, value, false);
}

/** Sets a numeric value to a variable, performing some checks and
  * calling the callback function if there is one.
  *
  * \param var   The variable.
  * \param value The numeric value, converted to a string before setting.
  * \sa CV_Set, CV_StealthSetValue
  */
void CV_SetValue(consvar_t *var, INT32 value)
{
	char val[32];

	sprintf(val, "%d", value);
	CV_SetCVar(var, val, false);
}

/** Adds a value to a console variable.
  * Used to increment and decrement variables from the menu.
  * Contains special cases to handle pointlimit in some multiplayer modes,
  * map number for game hosting, etc.
  *
  * \param var       The variable to add to.
  * \param increment The change in the variable; can be negative for a
  *                  decrement.
  * \sa CV_SetValue
  */
void CV_AddValue(consvar_t *var, INT32 increment)
{
	INT32 newvalue, max;

	if (!increment)
		return;

	// count pointlimit better
	/*if (var == &cv_pointlimit && (gametype == GT_MATCH))
		increment *= 50;*/
	newvalue = var->value + increment;

	if (var->PossibleValue)
	{
		if (var == &cv_nextmap)
		{
			// Special case for the nextmap variable, used only directly from the menu
			INT32 oldvalue = var->value - 1, gt;
			gt = cv_newgametype.value;
			{
				newvalue = var->value - 1;
				do
				{
					if(increment > 0) // Going up!
					{
						if (++newvalue == NUMMAPS)
							newvalue = -1;
					}
					else // Going down!
					{
						if (--newvalue == -2)
							newvalue = NUMMAPS-1;
					}

					if (newvalue == oldvalue)
						break; // don't loop forever if there's none of a certain gametype

					if(!mapheaderinfo[newvalue])
						continue; // Don't allocate the header.  That just makes memory usage skyrocket.

				} while (!M_CanShowLevelInList(newvalue, gt));

				var->value = newvalue + 1;
				var->func();
				return;
			}
		}
#define MINVAL 0
#define MAXVAL 1
		else if (var->PossibleValue[MINVAL].strvalue && !strcmp(var->PossibleValue[MINVAL].strvalue, "MIN"))
		{ // SRB2Kart
#ifdef PARANOIA
			if (!var->PossibleValue[MAXVAL].strvalue)
				I_Error("Bounded cvar \"%s\" without maximum!\n", var->name);
#endif

			if (newvalue < var->PossibleValue[MINVAL].value || newvalue > var->PossibleValue[MAXVAL].value)
			{
				INT32 currentindice = -1, newindice;
				for (max = MAXVAL+1; var->PossibleValue[max].strvalue; max++)
				{
					if (var->PossibleValue[max].value == newvalue)
					{
						increment = 0;
						currentindice = max;
						break; // The value we definitely want, stop here.
					}
					else if (var->PossibleValue[max].value == var->value)
						currentindice = max; // The value we maybe want.
				}

				if (increment)
				{
					increment = (increment > 0) ? 1 : -1;
					if (currentindice == -1 && max != MAXVAL+1)
						newindice = ((increment > 0) ? MAXVAL : max) + increment;
					else
						newindice = currentindice + increment;

					if (newindice >= max || newindice <= MAXVAL)
					{
						newvalue = var->PossibleValue[((increment > 0) ? MINVAL : MAXVAL)].value;
						CV_SetValue(var, newvalue);
					}
					else
						CV_Set(var, var->PossibleValue[newindice].strvalue);
				}
				else
					CV_Set(var, var->PossibleValue[currentindice].strvalue);
			}
			else
				CV_SetValue(var, newvalue);
		}
#undef MINVAL
#undef MAXVAL
		else
		{
			INT32 currentindice = -1, newindice;

			// this code do not support more than same value for differant PossibleValue
			for (max = 0; var->PossibleValue[max].strvalue; max++)
				if (var->PossibleValue[max].value == var->value)
					currentindice = max;

			// The following options will NOT handle netsyncing.
			if (var == &cv_chooseskin)
			{
				// Special case for the chooseskin variable, used only directly from the menu
				if (increment > 0) // Going up!
				{
					newvalue = var->value - 1;
					do
					{
						newvalue++;
						if (newvalue == MAXSKINS)
							newvalue = 0;
					} while (var->PossibleValue[newvalue].strvalue == NULL);
					var->value = newvalue + 1;
					var->string = var->PossibleValue[newvalue].strvalue;
					var->func();
					return;
				}
				else if (increment < 0) // Going down!
				{
					newvalue = var->value - 1;
					do
					{
						newvalue--;
						if (newvalue == -1)
							newvalue = MAXSKINS-1;
					} while (var->PossibleValue[newvalue].strvalue == NULL);
					var->value = newvalue + 1;
					var->string = var->PossibleValue[newvalue].strvalue;
					var->func();
					return;
				}
			}
			else if (var == &cv_playercolor)
			{
				// Special case for the playercolor variable, used only directly from the menu
				if (increment > 0) // Going up!
				{
					newvalue = var->value + 1;
					if (newvalue > MAXSKINCOLORS-1)
						newvalue = 1;
					var->value = newvalue;
					var->string = var->PossibleValue[var->value].strvalue;
					var->func();
					return;
				}
				else if (increment < 0) // Going down!
				{
					newvalue = var->value - 1;
					if (newvalue < 1)
						newvalue = MAXSKINCOLORS-1;
					var->value = newvalue;
					var->string = var->PossibleValue[var->value].strvalue;
					var->func();
					return;
				}
			}
			else if (var == &cv_kartspeed)
			{
				//expert toggle is shared with hard unlock
				max = (M_SecretUnlocked(SECRET_HARDSPEED) ? 4 : 2);
			}
#ifdef PARANOIA
			if (currentindice == -1)
				I_Error("CV_AddValue: current value %d not found in possible value\n",
					var->value);
#endif

			newindice = (currentindice + increment + max) % max;
			CV_Set(var, var->PossibleValue[newindice].strvalue);
		}
	}
	else
		CV_SetValue(var, newvalue);

	var->changed = 1; // user has changed it now
}

void CV_InitFilterVar(void)
{
#if 0
	UINT8 i;
	for (i = 0; i < 4; i++)
	{
		joyaxis_default[i] = true;
		joyaxis_count[i] = 0;
	}
#endif
}

void CV_ToggleExecVersion(boolean enable)
{
	execversion_enabled = enable;
}

static void CV_EnforceExecVersion(void)
{
	if (!execversion_enabled)
		CV_StealthSetValue(&cv_execversion, EXECVERSION);
}

static boolean CV_FilterJoyAxisVars(consvar_t *v, const char *valstr)
{
#if 1
	// We don't have changed axis defaults yet
	(void)v;
	(void)valstr;
#else
	UINT8 i;

	// If ALL axis settings are previous defaults, set them to the new defaults
	// EXECVERSION < 26 (2.1.21)

	for (i = 0; i < 4; i++)
	{
		if (joyaxis_default[i])
		{
			if (!stricmp(v->name, "joyaxis_fire"))
			{
				if (joyaxis_count[i] > 7) return false;
				else if (joyaxis_count[i] == 7) return true;

				if (!stricmp(valstr, "None")) joyaxis_count[i]++;
				else joyaxis_default[i] = false;
			}
			// reset all axis settings to defaults
			if (joyaxis_count[i] == 7)
			{
				switch (i)
				{
					default:
						COM_BufInsertText(va("%s \"%s\"\n", cv_turnaxis.name, cv_turnaxis.defaultvalue));
						COM_BufInsertText(va("%s \"%s\"\n", cv_moveaxis.name, cv_moveaxis.defaultvalue));
						COM_BufInsertText(va("%s \"%s\"\n", cv_brakeaxis.name, cv_brakeaxis.defaultvalue));
						COM_BufInsertText(va("%s \"%s\"\n", cv_aimaxis.name, cv_aimaxis.defaultvalue));
						COM_BufInsertText(va("%s \"%s\"\n", cv_lookaxis.name, cv_lookaxis.defaultvalue));
						COM_BufInsertText(va("%s \"%s\"\n", cv_fireaxis.name, cv_fireaxis.defaultvalue));
						COM_BufInsertText(va("%s \"%s\"\n", cv_driftaxis.name, cv_driftaxis.defaultvalue));
						break;
				}
				joyaxis_count[i]++;
				return false;
			}
		}
	}
#endif

	// we haven't reached our counts yet, or we're not default
	return true;
}

// Block the Xbox DInput default axes and reset to the current defaults 
static boolean CV_FilterJoyAxisVars2(consvar_t *v, const char *valstr)
{
	if (!stricmp(v->name, "joyaxis_turn") && !stricmp(valstr, "X-Axis"))
		return false;
	if (!stricmp(v->name, "joyaxis2_turn") && !stricmp(valstr, "X-Axis"))
		return false;
	if (!stricmp(v->name, "joyaxis3_turn") && !stricmp(valstr, "X-Axis"))
		return false;
	if (!stricmp(v->name, "joyaxis4_turn") && !stricmp(valstr, "X-Axis"))
		return false;
	if (!stricmp(v->name, "joyaxis_aim") && !stricmp(valstr, "Y-Axis"))
		return false;
	if (!stricmp(v->name, "joyaxis2_aim") && !stricmp(valstr, "Y-Axis"))
		return false;
	if (!stricmp(v->name, "joyaxis3_aim") && !stricmp(valstr, "Y-Axis"))
		return false;
	if (!stricmp(v->name, "joyaxis4_aim") && !stricmp(valstr, "Y-Axis"))
		return false;
	if (!stricmp(v->name, "joyaxis_fire") && !stricmp(valstr, "None"))
		return false;
	if (!stricmp(v->name, "joyaxis2_fire") && !stricmp(valstr, "None"))
		return false;
	if (!stricmp(v->name, "joyaxis3_fire") && !stricmp(valstr, "None"))
		return false;
	if (!stricmp(v->name, "joyaxis4_fire") && !stricmp(valstr, "None"))
		return false;
	if (!stricmp(v->name, "joyaxis_drift") && !stricmp(valstr, "None"))
		return false;
	if (!stricmp(v->name, "joyaxis2_drift") && !stricmp(valstr, "None"))
		return false;
	if (!stricmp(v->name, "joyaxis3_drift") && !stricmp(valstr, "None"))
		return false;
	if (!stricmp(v->name, "joyaxis4_drift") && !stricmp(valstr, "None"))
		return false;

	return true;
}

static boolean CV_FilterVarByVersion(consvar_t *v, const char *valstr)
{
	// True means allow the CV change, False means block it

	// We only care about CV_SAVE because this filters the user's config files
	// We do this same check in CV_Command
	if (!(v->flags & CV_SAVE))
		return true;

	if (GETMAJOREXECVERSION(cv_execversion.value) < 8) // 8 = 1.4
	{
		if (!stricmp(v->name, "masterserver") // Replaces a hack in MasterServer_OnChange for the original SRB2 MS.
			|| !stricmp(v->name, "gamma")) // Too easy to accidentially change in prior versions.
			return false;
	}

	if (GETMAJOREXECVERSION(cv_execversion.value) < 2) // 2 = 1.0.2
	{
#if 0
		// We don't have changed saved cvars yet
		if (!stricmp(v->name, "alwaysmlook")
			|| !stricmp(v->name, "alwaysmlook2")
			|| !stricmp(v->name, "mousemove")
			|| !stricmp(v->name, "mousemove2"))
			return false;
#endif

		// axis defaults were changed to be friendly to 360 controllers
		// if ALL axis settings are defaults, then change them to new values
		if (!CV_FilterJoyAxisVars(v, valstr))
			return false;
	}

	if (GETMAJOREXECVERSION(cv_execversion.value) < 10) // 10 = 1.6
	{
		// axis defaults changed again to SDL game controllers
		if (!CV_FilterJoyAxisVars2(v, valstr))
			return false;
	}

	return true;
}

/** Displays or changes a variable from the console.
  * Since the user is presumed to have been directly responsible
  * for this change, the variable is marked as changed this game.
  *
  * \return False if passed command was not recognized as a console
  *         variable, otherwise true.
  * \sa CV_ClearChangedFlags
  */
static boolean CV_Command(void)
{
	consvar_t *v;

	// check variables
	v = CV_FindVar(COM_Argv(0));
	if (!v)
		return false;

	// perform a variable print or set
	if (COM_Argc() == 1)
	{
		CONS_Printf(M_GetText("\"%s\" is \"%s\" default is \"%s\"\n"), v->name, v->string, v->defaultvalue);
		return true;
	}

	if (!(v->flags & CV_SAVE) || CV_FilterVarByVersion(v, COM_Argv(1)))
	{
		CV_Set(v, COM_Argv(1));
		v->changed = 1; // now it's been changed by (presumably) the user
	}
	return true;
}

/** Marks all variables as unchanged, indicating they've not been changed
  * by the user this game.
  *
  * \sa CV_Command
  * \author Graue <graue@oceanbase.org>
  */
void CV_ClearChangedFlags(void)
{
	consvar_t *cvar;

	for (cvar = consvar_vars; cvar; cvar = cvar->next)
		cvar->changed = 0;
}

/** Saves console variables to a file if they have the ::CV_SAVE
  * flag set.
  *
  * \param f File to save to.
  */
void CV_SaveVariables(FILE *f)
{
	consvar_t *cvar;

	for (cvar = consvar_vars; cvar; cvar = cvar->next)
		if (cvar->flags & CV_SAVE)
		{
			char stringtowrite[MAXTEXTCMD+1];

			// Silly hack for Min/Max vars
			if (!strcmp(cvar->string, "MAX") || !strcmp(cvar->string, "MIN"))
				sprintf(stringtowrite, "%d", cvar->value);
			else
				strcpy(stringtowrite, cvar->string);

			fprintf(f, "%s \"%s\"\n", cvar->name, stringtowrite);
		}
}

//============================================================================
//                            SCRIPT PARSE
//============================================================================

/** Parses a token out of a string. Handles script files too.
  *
  * \param data String to parse.
  * \return The data pointer after the token. NULL if no token found.
  */
static char *COM_Parse(char *data)
{
	char c;
	size_t len = 0;

	com_token[0] = 0;

	if (data == NULL)
		return NULL;

	// skip whitespace
skipwhite:
	while ((c = *data) <= ' ')
	{
		if (c == '\0')
			return NULL; // end of file;
		data++;
	}

	// skip // comments
	if (c == '/' && data[1] == '/')
	{
		while (*data && *data != '\n')
			data++;
		goto skipwhite;
	}

	// handle quoted strings specially
	if (c == '\"')
	{
		data++;
		for (;;)
		{
			c = *data++;
			if (c == '\"' || c == '\0')
			{
				com_token[len] = 0;
				return data;
			}
			com_token[len] = c;
			len++;
		}
	}

	// parse single characters
	if (c == '{' || c == '}' || c == ')' || c == '(' || c == '\'')
	{
		com_token[len] = c;
		len++;
		com_token[len] = 0;
		return data + 1;
	}

	// parse a regular word
	do
	{
		com_token[len] = c;
		data++;
		len++;
		c = *data;
		if (c == '{' || c == '}' || c == ')'|| c == '(' || c == '\'')
			break;
	} while (c > 32);

	com_token[len] = 0;
	return data;
}
