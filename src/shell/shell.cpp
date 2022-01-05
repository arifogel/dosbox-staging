/*
 *  Copyright (C) 2002-2021  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "shell.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <memory>

#include "callback.h"
#include "control.h"
#include "fs_utils.h"
#include "mapper.h"
#include "regs.h"
#include "string_utils.h"
#include "timer.h"

Bitu call_shellstop;
/* Larger scope so shell_del autoexec can use it to
 * remove things from the environment */
DOS_Shell *first_shell = nullptr;

static Bitu shellstop_handler()
{
	return CBRET_STOP;
}

static void SHELL_ProgramStart(Program * * make) {
	*make = new DOS_Shell;
}
//Repeat it with the correct type, could do it in the function below, but this way it should be 
//clear that if the above function is changed, this function might need a change as well.
static void SHELL_ProgramStart_First_shell(DOS_Shell * * make) {
	*make = new DOS_Shell;
}

#define AUTOEXEC_SIZE 4096
static char autoexec_data[AUTOEXEC_SIZE] = { 0 };
static std::list<std::string> autoexec_strings;
typedef std::list<std::string>::iterator auto_it;

void VFILE_Remove(const char *name);

void AutoexecObject::Install(const std::string &in) {
	if (GCC_UNLIKELY(installed))
		E_Exit("autoexec: already created %s", buf.c_str());
	installed = true;
	buf = in;
	autoexec_strings.push_back(buf);
	this->CreateAutoexec();

	//autoexec.bat is normally created AUTOEXEC_Init.
	//But if we are already running (first_shell)
	//we have to update the envirionment to display changes

	if(first_shell)	{
		//create a copy as the string will be modified
		std::string::size_type n = buf.size();
		char* buf2 = new char[n + 1];
		safe_strncpy(buf2, buf.c_str(), n + 1);
		if((strncasecmp(buf2,"set ",4) == 0) && (strlen(buf2) > 4)){
			char* after_set = buf2 + 4;//move to variable that is being set
			char* test = strpbrk(after_set,"=");
			if(!test) {first_shell->SetEnv(after_set,"");return;}
			*test++ = 0;
			//If the shell is running/exists update the environment
			first_shell->SetEnv(after_set,test);
		}
		delete [] buf2;
	}
}

void AutoexecObject::InstallBefore(const std::string &in) {
	if(GCC_UNLIKELY(installed)) E_Exit("autoexec: already created %s",buf.c_str());
	installed = true;
	buf = in;
	autoexec_strings.push_front(buf);
	this->CreateAutoexec();
}

void AutoexecObject::CreateAutoexec()
{
	/* Remove old autoexec.bat if the shell exists */
	if(first_shell)	VFILE_Remove("AUTOEXEC.BAT");

	//Create a new autoexec.bat
	autoexec_data[0] = 0;
	size_t auto_len;
	for (std::string linecopy : autoexec_strings) {
		std::string::size_type offset = 0;
		// Lets have \r\n as line ends in autoexec.bat.
		while(offset < linecopy.length()) {
			const auto n = linecopy.find('\n', offset);
			if (n == std::string::npos)
				break;
			const auto rn = linecopy.find("\r\n", offset);
			if (rn != std::string::npos && rn + 1 == n) {
				offset = n + 1;
				continue;
			}
			// \n found without matching \r
			linecopy.replace(n,1,"\r\n");
			offset = n + 2;
		}

		auto_len = safe_strlen(autoexec_data);
		if ((auto_len+linecopy.length() + 3) > AUTOEXEC_SIZE) {
			E_Exit("SYSTEM:Autoexec.bat file overflow");
		}
		sprintf((autoexec_data + auto_len),"%s\r\n",linecopy.c_str());
	}
	if (first_shell) VFILE_Register("AUTOEXEC.BAT",(Bit8u *)autoexec_data,(Bit32u)strlen(autoexec_data));
}

AutoexecObject::~AutoexecObject(){
	if(!installed) return;

	// Remove the line from the autoexecbuffer and update environment
	for(auto_it it = autoexec_strings.begin(); it != autoexec_strings.end(); ) {
		if ((*it) == buf) {
			std::string::size_type n = buf.size();
			char* buf2 = new char[n + 1];
			safe_strncpy(buf2, buf.c_str(), n + 1);
			bool stringset = false;
			// If it's a environment variable remove it from there as well
			if ((strncasecmp(buf2,"set ",4) == 0) && (strlen(buf2) > 4)){
				char* after_set = buf2 + 4;//move to variable that is being set
				char* test = strpbrk(after_set,"=");
				if (!test) {
					delete [] buf2;
					continue;
				}
				*test = 0;
				stringset = true;
				//If the shell is running/exists update the environment
				if (first_shell) first_shell->SetEnv(after_set,"");
			}
			delete [] buf2;
			if (stringset && first_shell && first_shell->bf && first_shell->bf->filename.find("AUTOEXEC.BAT") != std::string::npos) {
				//Replace entry with spaces if it is a set and from autoexec.bat, as else the location counter will be off.
				*it = buf.assign(buf.size(),' ');
				it++;
			} else {
				it = autoexec_strings.erase(it);
			}
		} else it++;
	}
	this->CreateAutoexec();
}

DOS_Shell::DOS_Shell()
        : Program(),
          l_history{},
          l_completion{},
          completion_start(nullptr),
          completion_index(0),
          input_handle(STDIN),
          bf(nullptr),
          echo(true),
          call(false)
{}

// TODO: this function should be refactored to make to it easier to understand.
// It's currently riddled with pointer and array adjustments each loop plus
// branches and sub-loops.
Bitu DOS_Shell::GetRedirection(char *s, char **ifn, char **ofn, bool *append)
{
	char * lr=s;
	char * lw=s;
	char ch;
	Bitu num=0;
	bool quote = false;
	char *temp = nullptr;
	size_t temp_len = 0;

	while ( (ch=*lr++) ) {
		if(quote && ch != '"') { /* don't parse redirection within quotes. Not perfect yet. Escaped quotes will mess the count up */
			*lw++ = ch;
			continue;
		}

		switch (ch) {
		case '"':
			quote = !quote;
			break;
		case '>':
			*append=((*lr)=='>');
			if (*append) lr++;
			lr=ltrim(lr);
			if (*ofn) {
				delete[] * ofn;
				*ofn = nullptr;
			}
			*ofn = lr;
			while (*lr && *lr!=' ' && *lr!='<' && *lr!='|') lr++;
			//if it ends on a : => remove it.
			if((*ofn != lr) && (lr[-1] == ':')) lr[-1] = 0;
			temp_len = static_cast<size_t>(lr - *ofn + 1u);
			temp = new char[temp_len];
			safe_strncpy(temp, *ofn, temp_len);
			*ofn = temp;
			continue;

		case '<':
			if (*ifn) {
				delete[] * ifn;
				*ifn = nullptr;
			}
			lr = ltrim(lr);
			*ifn = lr;

			while (*lr && *lr != ' ' && *lr != '>' && *lr != '|')
				lr++;

			if ((*ifn != lr) && (lr[-1] == ':'))
				lr[-1] = 0;

			assert(lr >= *ifn);
			temp_len = static_cast<size_t>(lr - *ifn + 1u);
			temp = new char[temp_len];
			safe_strncpy(temp, *ifn, temp_len);
			*ifn = temp;
			continue;

		case '|': ch = 0; num++;
		}
		*lw++=ch;
	}
	*lw=0;
	return num;
}

void DOS_Shell::ParseLine(char * line) {
	LOG(LOG_EXEC,LOG_ERROR)("Parsing command line: %s",line);
	/* Check for a leading @ */
 	if (line[0] == '@') line[0] = ' ';
	line = trim(line);

	/* Do redirection and pipe checks */

	char *in = nullptr;
	char *out = nullptr;

	Bit16u dummy,dummy2;
	Bit32u bigdummy = 0;
	Bitu num = 0;		/* Number of commands in this line */
	bool append;
	bool normalstdin  = false;	/* wether stdin/out are open on start. */
	bool normalstdout = false;	/* Bug: Assumed is they are "con"      */

	num = GetRedirection(line,&in, &out,&append);
	if (num>1) LOG_MSG("SHELL: Multiple command on 1 line not supported");
	if (in || out) {
		normalstdin  = (psp->GetFileHandle(0) != 0xff);
		normalstdout = (psp->GetFileHandle(1) != 0xff);
	}
	if (in) {
		if(DOS_OpenFile(in,OPEN_READ,&dummy)) {	//Test if file exists
			DOS_CloseFile(dummy);
			LOG_MSG("SHELL: Redirect input from %s",in);
			if(normalstdin) DOS_CloseFile(0);	//Close stdin
			DOS_OpenFile(in,OPEN_READ,&dummy);	//Open new stdin
		}
	}
	if (out){
		LOG_MSG("SHELL: Redirect output to %s",out);
		if(normalstdout) DOS_CloseFile(1);
		if(!normalstdin && !in) DOS_OpenFile("con",OPEN_READWRITE,&dummy);
		bool status = true;
		/* Create if not exist. Open if exist. Both in read/write mode */
		if(append) {
			if( (status = DOS_OpenFile(out,OPEN_READWRITE,&dummy)) ) {
				 DOS_SeekFile(1,&bigdummy,DOS_SEEK_END);
			} else {
				status = DOS_CreateFile(out,DOS_ATTR_ARCHIVE,&dummy);	//Create if not exists.
			}
		} else {
			status = DOS_OpenFileExtended(out,OPEN_READWRITE,DOS_ATTR_ARCHIVE,0x12,&dummy,&dummy2);
		}

		if(!status && normalstdout) DOS_OpenFile("con",OPEN_READWRITE,&dummy); //Read only file, open con again
		if(!normalstdin && !in) DOS_CloseFile(0);
	}
	/* Run the actual command */
	DoCommand(line);
	/* Restore handles */
	if(in) {
		DOS_CloseFile(0);
		if(normalstdin) DOS_OpenFile("con",OPEN_READWRITE,&dummy);
		delete[] in;
	}
	if(out) {
		DOS_CloseFile(1);
		if(!normalstdin) DOS_OpenFile("con",OPEN_READWRITE,&dummy);
		if(normalstdout) DOS_OpenFile("con",OPEN_READWRITE,&dummy);
		if(!normalstdin) DOS_CloseFile(0);
		delete[] out;
	}
}

void DOS_Shell::RunInternal()
{
	char input_line[CMD_MAXLINE] = {0};
	while (bf && !shutdown_requested) {
		if (bf->ReadLine(input_line)) {
			if (echo) {
				if (input_line[0] != '@') {
					ShowPrompt();
					WriteOut_NoParsing(input_line);
					WriteOut_NoParsing("\n");
				}
			}
			ParseLine(input_line);
			if (echo) WriteOut_NoParsing("\n");
		} else {
			bf.reset();
		}
	}
}

extern int64_t ticks_at_program_launch; // from shell_cmd
void DOS_Shell::Run()
{
	// Initialize the tick-count only when the first shell has launched.
	// This ensures that slow-performing configurable tasks (like loading MIDI SF2 files) have already
	// been performed and won't affect this time.
	ticks_at_program_launch = GetTicks();

	char input_line[CMD_MAXLINE] = {0};
	std::string line;
	if (cmd->FindExist("/?", false) || cmd->FindExist("-?", false)) {
		WriteOut(MSG_Get("SHELL_CMD_COMMAND_HELP_LONG"));
		return;
	}
	if (cmd->FindStringRemainBegin("/C",line)) {
		safe_strcpy(input_line, line.c_str());
		char* sep = strpbrk(input_line,"\r\n"); //GTA installer
		if (sep) *sep = 0;
		DOS_Shell temp;
		temp.echo = echo;
		temp.ParseLine(input_line);		//for *.exe *.com  |*.bat creates the bf needed by runinternal;
		temp.RunInternal();				// exits when no bf is found.
		return;
	}
	/* Start a normal shell and check for a first command init */
	if (cmd->FindString("/INIT",line,true)) {
		const bool wants_welcome_banner = control->GetStartupVerbosity() >=
		                                  Verbosity::Medium;
		if (wants_welcome_banner) {
			WriteOut(MSG_Get("SHELL_STARTUP_BEGIN"),
			         DOSBOX_GetDetailedVersion(), PRIMARY_MOD_NAME,
			         PRIMARY_MOD_NAME, PRIMARY_MOD_PAD, PRIMARY_MOD_PAD,
			         PRIMARY_MOD_NAME, PRIMARY_MOD_PAD);
#if C_DEBUG
			WriteOut(MSG_Get("SHELL_STARTUP_DEBUG"), MMOD2_NAME);
#endif
			if (machine == MCH_CGA) {
				if (mono_cga)
					WriteOut(MSG_Get("SHELL_STARTUP_CGA_MONO"),
					         MMOD2_NAME);
				else
					WriteOut(MSG_Get("SHELL_STARTUP_CGA"),
					         MMOD2_NAME, MMOD1_NAME,
					         MMOD2_NAME, PRIMARY_MOD_PAD);
			}
			if (machine == MCH_HERC)
				WriteOut(MSG_Get("SHELL_STARTUP_HERC"));
			WriteOut(MSG_Get("SHELL_STARTUP_END"));
		}
		safe_strcpy(input_line, line.c_str());
		line.erase();
		ParseLine(input_line);
	} else {
		WriteOut(MSG_Get("SHELL_STARTUP_SUB"), DOSBOX_GetDetailedVersion());
	}
	do {
		if (bf){
			if(bf->ReadLine(input_line)) {
				if (echo) {
					if (input_line[0]!='@') {
						ShowPrompt();
						WriteOut_NoParsing(input_line);
						WriteOut_NoParsing("\n");
					}
				}
				ParseLine(input_line);
			} else {
				bf.reset();
			}
		} else {
			if (echo) ShowPrompt();
			InputCommand(input_line);
			ParseLine(input_line);
		}
	} while (!exit_cmd_called && !shutdown_requested);
}

void DOS_Shell::SyntaxError()
{
	WriteOut(MSG_Get("SHELL_SYNTAXERROR"));
}

extern int64_t ticks_at_program_launch;

class AUTOEXEC final : public Module_base {
private:
	AutoexecObject autoexec[17];
	AutoexecObject autoexec_echo;
	void ProcessConfigFileAutoexec(const Section_line &section,
	                               const std::string &source_name);

public:
	AUTOEXEC(Section* configuration)
		: Module_base(configuration),
		  autoexec_echo()
	{
		// Initialize configurable states that control autoexec-related
		// behavior

		/* Check -securemode switch to disable mount/imgmount/boot after
		 * running autoexec.bat */
		const bool secure = control->cmdline->FindExist("-securemode", true);

		// Are autoexec sections permitted?
		const bool autoexec_is_allowed = !secure &&
		                              !control->cmdline->FindExist("-noautoexec", true);

		// Should autoexec sections be joined or overwritten?
		const auto ds = control->GetSection("dosbox");
		assert(ds);
		const bool should_join_autoexecs = ds->GetPropValue("autoexec_section") == "join";

		/* Check to see for extra command line options to be added
		 * (before the command specified on commandline) */
		/* Maximum of extra commands: 10 */
		uint8_t i = 1;
		std::string line;
		bool exit_call_exists = false;
		while (control->cmdline->FindString("-c", line, true) && (i <= 11)) {
#if defined(WIN32)
			// replace single with double quotes so that mount
			// commands can contain spaces
			for (Bitu temp = 0; temp < line.size(); ++temp)
				if (line[temp] == '\'')
					line[temp] = '\"';
#endif // Linux users can simply use \" in their shell

			// If the user's added an exit call, simply store that
			// fact but don't insert it because otherwise it can
			// precede follow on [autoexec] calls.
			if (line == "exit" || line == "\"exit\"") {
				exit_call_exists = true;
				continue;
			}
			autoexec[i++].Install(line);
		}

		// Check for the -exit switch, which indicates they want to quit
		const bool exit_arg_exists = control->cmdline->FindExist("-exit");

		// Check if instant-launch is active
		const bool using_instant_launch = control->GetStartupVerbosity() ==
		                                  Verbosity::InstantLaunch;

		// Should we add an 'exit' call to the end of autoexec.bat?
		const bool addexit = exit_call_exists
		                     || exit_arg_exists
		                     || using_instant_launch;

		/* Check for first command being a directory or file */
		char buffer[CROSS_LEN + 1];
		char orig[CROSS_LEN + 1];
		char cross_filesplit[2] = {CROSS_FILESPLIT, 0};

		unsigned int command_index = 1;
		bool found_dir_or_command = false;
		while (control->cmdline->FindCommand(command_index++, line) &&
		       !found_dir_or_command) {
			struct stat test;
			if (line.length() > CROSS_LEN)
				continue;
			safe_strcpy(buffer, line.c_str());
			if (stat(buffer, &test)) {
				if (getcwd(buffer, CROSS_LEN) == NULL)
					continue;
				if (safe_strlen(buffer) + line.length() + 1 > CROSS_LEN)
					continue;
				safe_strcat(buffer, cross_filesplit);
				safe_strcat(buffer, line.c_str());
				if (stat(buffer, &test))
					continue;
			}
			if (test.st_mode & S_IFDIR) {
				autoexec[12].Install(std::string("MOUNT C \"") + buffer + "\"");
				autoexec[13].Install("C:");
				if (secure)
					autoexec[14].Install("z:\\config.com -securemode");
			} else {
				char *name = strrchr(buffer, CROSS_FILESPLIT);
				if (!name) { // Only a filename
					line = buffer;
					if (getcwd(buffer, CROSS_LEN) == NULL)
						continue;
					if (safe_strlen(buffer) + line.length() + 1 > CROSS_LEN)
						continue;
					safe_strcat(buffer, cross_filesplit);
					safe_strcat(buffer, line.c_str());
					if (stat(buffer, &test))
						continue;
					name = strrchr(buffer, CROSS_FILESPLIT);
					if (!name)
						continue;
				}
				*name++ = 0;
				if (!path_exists(buffer))
					continue;
				autoexec[12].Install(std::string("MOUNT C \"") + buffer + "\"");
				autoexec[13].Install("C:");
				/* Save the non-modified filename (so boot and
				 * imgmount can use it (long filenames, case
				 * sensivitive)) */
				safe_strcpy(orig, name);
				upcase(name);
				if (strstr(name, ".BAT") != 0) {
					if (secure)
						autoexec[14].Install("z:\\config.com -securemode");
					/* BATch files are called else exit will not work */
					autoexec[15].Install(std::string("CALL ") + name);
				} else if ((strstr(name, ".IMG") != 0) || (strstr(name, ".IMA") != 0)) {
					// No secure mode here as boot is destructive and enabling securemode disables boot
					/* Boot image files */
					autoexec[15].Install(std::string("BOOT ") + orig);
				} else if ((strstr(name, ".ISO") != 0) || (strstr(name, ".CUE") != 0)) {
					/* imgmount CD image files */
					/* securemode gets a different number from the previous branches! */
					autoexec[14].Install(std::string("IMGMOUNT D \"") + orig + std::string("\" -t iso"));
					// autoexec[16].Install("D:");
					if (secure)
						autoexec[15].Install("z:\\config.com -securemode");
					/* Makes no sense to exit here */
				} else {
					if (secure)
						autoexec[14].Install("z:\\config.com -securemode");
					autoexec[15].Install(name);
				}
			}
			found_dir_or_command = true;
		}

		if (autoexec_is_allowed) {
			if (should_join_autoexecs) {
				ProcessConfigFileAutoexec(*static_cast<const Section_line *>(configuration),
				                          "one or more joined sections");
			} else if (found_dir_or_command) {
				LOG_MSG("AUTOEXEC: Using commands provided on the command line");
			} else {
				ProcessConfigFileAutoexec(
				        control->GetOverwrittenAutoexecSection(),
				        control->GetOverwrittenAutoexecConf());
			}
		} else if (secure && !found_dir_or_command) {
			// If we're in secure mode without command line executabls, then seal off the configuration
			autoexec[12].Install("z:\\config.com -securemode");
		}

		// The last slot is always reserved for the exit call,
		// regardless if we're in secure-mode or not.
		if (addexit)
			autoexec[16].Install("exit");

		// Print the entire autoexec content, if needed:
		// for (const auto &autoexec_line : autoexec)
		// 	LOG_INFO("AUTOEXEC-LINE: %s", autoexec_line.GetLine().c_str());

		VFILE_Register("AUTOEXEC.BAT",(Bit8u *)autoexec_data,(Bit32u)strlen(autoexec_data));
	}
};

void AUTOEXEC::ProcessConfigFileAutoexec(const Section_line &section,
                                         const std::string &source_name)
{
	if (section.data.empty())
		return;

	auto extra = &section.data[0];

	/* detect if "echo off" is the first line */
	size_t firstline_length = strcspn(extra, "\r\n");
	bool echo_off = !strncasecmp(extra, "echo off", 8);
	if (echo_off && firstline_length == 8)
		extra += 8;
	else {
		echo_off = !strncasecmp(extra, "@echo off", 9);
		if (echo_off && firstline_length == 9)
			extra += 9;
		else
			echo_off = false;
	}

	/* if "echo off" move it to the front of autoexec.bat */
	if (echo_off) {
		autoexec_echo.InstallBefore("@echo off");
		if (*extra == '\r')
			extra++; // It can point to \0
		if (*extra == '\n')
			extra++; // same
	}

	/* Install the stuff from the configfile if anything
		* left after moving echo off */
	if (*extra) {
		autoexec[0].Install(std::string(extra));
		LOG_MSG("AUTOEXEC: Using autoexec from %s", source_name.c_str());
	}
}

static std::unique_ptr<AUTOEXEC> autoexec_module{};

void AUTOEXEC_Init(Section *sec)
{
	autoexec_module = std::make_unique<AUTOEXEC>(sec);
}

static Bitu INT2E_Handler()
{
	/* Save return address and current process */
	RealPt save_ret=real_readd(SegValue(ss),reg_sp);
	Bit16u save_psp=dos.psp();

	/* Set first shell as process and copy command */
	dos.psp(DOS_FIRST_SHELL);
	DOS_PSP psp(DOS_FIRST_SHELL);
	psp.SetCommandTail(RealMakeSeg(ds,reg_si));
	SegSet16(ss,RealSeg(psp.GetStack()));
	reg_sp=2046;

	/* Read and fix up command string */
	CommandTail tail;
	MEM_BlockRead(PhysMake(dos.psp(),128),&tail,128);
	if (tail.count<127) tail.buffer[tail.count]=0;
	else tail.buffer[126]=0;
	char* crlf=strpbrk(tail.buffer,"\r\n");
	if (crlf) *crlf=0;

	/* Execute command */
	if (safe_strlen(tail.buffer)) {
		DOS_Shell temp;
		temp.ParseLine(tail.buffer);
		temp.RunInternal();
	}

	/* Restore process and "return" to caller */
	dos.psp(save_psp);
	SegSet16(cs,RealSeg(save_ret));
	reg_ip=RealOff(save_ret);
	reg_ax=0;
	return CBRET_NONE;
}

static char const * const path_string="PATH=Z:\\";
static char const * const comspec_string="COMSPEC=Z:\\COMMAND.COM";
static char const * const full_name="Z:\\COMMAND.COM";
static char const * const init_line="/INIT AUTOEXEC.BAT";

void SHELL_Init() {
	/* Add messages */
	MSG_Add("SHELL_ILLEGAL_PATH","Illegal Path.\n");
	MSG_Add("SHELL_CMD_HELP","If you want a list of all supported commands type \033[33;1mhelp /all\033[0m .\nA short list of the most often used commands:\n");
	MSG_Add("SHELL_CMD_COMMAND_HELP_LONG",
	        "Starts the DOSBox Staging command shell.\n"
	        "Usage:\n"
	        "  \033[32;1mcommand\033[0m\n"
	        "  \033[32;1mcommand\033[0m /c (or /init) \033[36;1mCOMMAND\033[0m\n"
	        "\n"
	        "Where:\n"
	        "  \033[36;1mCOMMAND\033[0m is a DOS command, game, or program to run.\n"
	        "\n"
	        "Notes:\n"
	        "  DOSBox Staging automatically starts a DOS command shell by invoking this\n"
	        "  command with /init option when it starts, which shows the welcome banner.\n"
	        "  You can load a new instance of the command shell by running \033[32;1mcommand\033[0m.\n"
	        "  Adding a /c option along with \033[36;1mCOMMAND\033[0m allows this command to run the\n"
	        "  specified command (optionally with parameters) and then exit automatically.\n"
	        "\n"
	        "Examples:\n"
	        "  \033[32;1mcommand\033[0m\n"
	        "  \033[32;1mcommand\033[0m /c \033[36;1mecho\033[0m \033[37mHello world!\033[0m\n"
	        "  \033[32;1mcommand\033[0m /init \033[36;1mdir\033[0m\n");
	MSG_Add("SHELL_CMD_ECHO_ON","ECHO is on.\n");
	MSG_Add("SHELL_CMD_ECHO_OFF", "ECHO is off.\n");
	MSG_Add("SHELL_ILLEGAL_SWITCH","Illegal switch: %s.\n");
	MSG_Add("SHELL_MISSING_PARAMETER","Required parameter missing.\n");
	MSG_Add("SHELL_CMD_CHDIR_ERROR","Unable to change to: %s.\n");
	MSG_Add("SHELL_CMD_CHDIR_HINT","Hint: To change to different drive type \033[31m%c:\033[0m\n");
	MSG_Add("SHELL_CMD_CHDIR_HINT_2","directoryname is longer than 8 characters and/or contains spaces.\nTry \033[31mcd %s\033[0m\n");
	MSG_Add("SHELL_CMD_CHDIR_HINT_3","You are still on drive Z:, change to a mounted drive with \033[31mC:\033[0m.\n");
	MSG_Add("SHELL_CMD_DATE_HELP", "Displays or changes the internal date.\n");
	MSG_Add("SHELL_CMD_DATE_ERROR", "The specified date is not correct.\n");
	MSG_Add("SHELL_CMD_DATE_DAYS", "3SunMonTueWedThuFriSat"); // "2SoMoDiMiDoFrSa"
	MSG_Add("SHELL_CMD_DATE_NOW", "Current date: ");
	MSG_Add("SHELL_CMD_DATE_SETHLP", "Type 'date MM-DD-YYYY' to change.\n");
	MSG_Add("SHELL_CMD_DATE_FORMAT", "M/D/Y");
	MSG_Add("SHELL_CMD_DATE_HELP_LONG",
	        "Usage:\n"
	        "  \033[32;1mdate\033[0m [/t]\n"
	        "  \033[32;1mdate\033[0m /h\n"
	        "  \033[32;1mdate\033[0m \033[36;1mDATE\033[0m\n"
	        "\n"
	        "Where:\n"
	        "  \033[36;1mDATE\033[0m is the new date to set to, in the format of \033[36;1mMM-DD-YYYY\033[0m.\n"
	        "\n"
	        "Notes:\n"
	        "  Running \033[32;1mdate\033[0m without an argument shows the current date, or only a date\n"
	        "  with the /t option. You can force a date synchronization of with the host\n"
	        "  system with the /h option, or manually specify a new date to set to.\n"
	        "\n"
	        "Examples:\n"
	        "  \033[32;1mdate\033[0m\n"
	        "  \033[32;1mdate\033[0m /h\n"
	        "  \033[32;1mdate\033[0m \033[36;1m10-11-2012\033[0m\n");
	MSG_Add("SHELL_CMD_TIME_HELP", "Displays or changes the internal time.\n");
	MSG_Add("SHELL_CMD_TIME_ERROR", "The specified time is not correct.\n");
	MSG_Add("SHELL_CMD_TIME_NOW", "Current time: ");
	MSG_Add("SHELL_CMD_TIME_SETHLP", "Type 'time hh:mm:ss' to change.\n");
	MSG_Add("SHELL_CMD_TIME_HELP_LONG",
	        "Usage:\n"
	        "  \033[32;1mtime\033[0m [/t]\n"
	        "  \033[32;1mtime\033[0m /h\n"
	        "  \033[32;1mtime\033[0m \033[36;1mTIME\033[0m\n"
	        "\n"
	        "Where:\n"
	        "  \033[36;1mTIME\033[0m is the new time to set to, in the format of \033[36;1mhh:mm:ss\033[0m.\n"
	        "\n"
	        "Notes:\n"
	        "  Running \033[32;1mtime\033[0m without an argument shows the current time, or a simple time\n"
	        "  with the /t option. You can force a time synchronization of with the host\n"
	        "  system with the /h option, or manually specify a new time to set to.\n"
	        "\n"
	        "Examples:\n"
	        "  \033[32;1mtime\033[0m\n"
	        "  \033[32;1mtime\033[0m /h\n"
	        "  \033[32;1mtime\033[0m \033[36;1m13:14:15\033[0m\n");
	MSG_Add("SHELL_CMD_MKDIR_ERROR","Unable to make: %s.\n");
	MSG_Add("SHELL_CMD_RMDIR_ERROR","Unable to remove: %s.\n");
	MSG_Add("SHELL_CMD_DEL_ERROR","Unable to delete: %s.\n");
	MSG_Add("SHELL_SYNTAXERROR","The syntax of the command is incorrect.\n");
	MSG_Add("SHELL_CMD_SET_NOT_SET","Environment variable %s not defined.\n");
	MSG_Add("SHELL_CMD_SET_OUT_OF_SPACE","Not enough environment space left.\n");
	MSG_Add("SHELL_CMD_IF_EXIST_MISSING_FILENAME","IF EXIST: Missing filename.\n");
	MSG_Add("SHELL_CMD_IF_ERRORLEVEL_MISSING_NUMBER","IF ERRORLEVEL: Missing number.\n");
	MSG_Add("SHELL_CMD_IF_ERRORLEVEL_INVALID_NUMBER","IF ERRORLEVEL: Invalid number.\n");
	MSG_Add("SHELL_CMD_GOTO_MISSING_LABEL","No label supplied to GOTO command.\n");
	MSG_Add("SHELL_CMD_GOTO_LABEL_NOT_FOUND","GOTO: Label %s not found.\n");
	MSG_Add("SHELL_CMD_FILE_NOT_FOUND", "File not found: %s\n");
	MSG_Add("SHELL_CMD_FILE_EXISTS","File %s already exists.\n");
	MSG_Add("SHELL_CMD_DIR_VOLUME"," Volume in drive %c is %s\n");
	MSG_Add("SHELL_CMD_DIR_INTRO"," Directory of %s\n");
	MSG_Add("SHELL_CMD_DIR_BYTES_USED","%17d file(s) %21s bytes\n");
	MSG_Add("SHELL_CMD_DIR_BYTES_FREE","%17d dir(s)  %21s bytes free\n");
	MSG_Add("SHELL_EXECUTE_DRIVE_NOT_FOUND","Drive %c does not exist!\nYou must \033[31mmount\033[0m it first. Type \033[1;33mintro\033[0m or \033[1;33mintro mount\033[0m for more information.\n");
	MSG_Add("SHELL_EXECUTE_ILLEGAL_COMMAND","Illegal command: %s.\n");
	MSG_Add("SHELL_CMD_PAUSE", "Press a key to continue...");
	MSG_Add("SHELL_CMD_PAUSE_HELP", "Waits for a keystroke to continue.\n");
	MSG_Add("SHELL_CMD_PAUSE_HELP_LONG",
	        "Usage:\n"
	        "  \033[32;1mpause\033[0m\n"
	        "\n"
	        "Where:\n"
	        "  This command has no parameters.\n"
	        "\n"
	        "Notes:\n"
	        "  This command is especially useful in batch programs to allow a user to\n"
	        "  continue the batch program execution with a key press. The user can press\n"
	        "  any key on the keyboard (except for certain control keys) to continue.\n"
	        "\n"
	        "Examples:\n"
	        "  \033[32;1mpause\033[0m\n");
	MSG_Add("SHELL_CMD_COPY_FAILURE","Copy failure : %s.\n");
	MSG_Add("SHELL_CMD_COPY_SUCCESS","   %d File(s) copied.\n");
	MSG_Add("SHELL_CMD_SUBST_NO_REMOVE","Unable to remove, drive not in use.\n");
	MSG_Add("SHELL_CMD_SUBST_FAILURE","SUBST failed. You either made an error in your commandline or the target drive is already used.\nIt's only possible to use SUBST on Local drives");

	MSG_Add("SHELL_STARTUP_BEGIN",
	        "\033[44;1m\xC9\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD"
	        "\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD"
	        "\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xBB\n"
	        "\xBA \033[32mWelcome to DOSBox Staging %-40s\033[37m \xBA\n"
	        "\xBA                                                                    \xBA\n"
	        "\xBA For a short introduction for new users type: \033[33mINTRO\033[37m                 \xBA\n"
	        "\xBA For supported shell commands type: \033[33mHELP\033[37m                            \xBA\n"
	        "\xBA                                                                    \xBA\n"
	        "\xBA To adjust the emulated CPU speed, use \033[31m%s+F11\033[37m and \033[31m%s+F12\033[37m.%s%s       \xBA\n"
	        "\xBA To activate the keymapper \033[31m%s+F1\033[37m.%s                                 \xBA\n"
	        "\xBA For more information read the \033[36mREADME\033[37m file in the DOSBox directory. \xBA\n"
	        "\xBA                                                                    \xBA\n");
	MSG_Add("SHELL_STARTUP_CGA",
	        "\xBA DOSBox supports Composite CGA mode.                                \xBA\n"
	        "\xBA Use \033[31mF12\033[37m to set composite output ON, OFF, or AUTO (default).        \xBA\n"
	        "\xBA \033[31mF10\033[37m selects the CGA settings to change and \033[31m(%s+)F11\033[37m changes it.   \xBA\n"
	        "\xBA                                                                    \xBA\n");
	MSG_Add("SHELL_STARTUP_CGA_MONO",
	        "\xBA Use \033[31mF11\033[37m to cycle through green, amber, white and paper-white mode, \xBA\n"
	        "\xBA and \033[31m%s+F11\033[37m to change contrast/brightness settings.                \xBA\n");
	MSG_Add("SHELL_STARTUP_HERC",
	        "\xBA Use \033[31mF11\033[37m to cycle through white, amber, and green monochrome color. \xBA\n"
	        "\xBA                                                                    \xBA\n");
	MSG_Add("SHELL_STARTUP_DEBUG",
	        "\xBA Press \033[31m%s+Pause\033[37m to enter the debugger or start the exe with \033[33mDEBUG\033[37m. \xBA\n"
	        "\xBA                                                                    \xBA\n");
	MSG_Add("SHELL_STARTUP_END",
	        "\xBA \033[33mhttps://dosbox-staging.github.io\033[37m                                   \xBA\n"
	        "\xC8\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD"
	        "\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD"
	        "\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xBC\033[0m\n"
	        "\n");

	MSG_Add("SHELL_STARTUP_SUB","\033[32;1mdosbox-staging %s\033[0m\n");
	MSG_Add("SHELL_CMD_CHDIR_HELP","Displays or changes the current directory.\n");
	MSG_Add("SHELL_CMD_CHDIR_HELP_LONG",
	        "Usage:\n"
	        "  \033[32;1mcd\033[0m \033[36;1mDIRECTORY\033[0m\n"
	        "  \033[32;1mchdir\033[0m \033[36;1mDIRECTORY\033[0m\n"
	        "\n"
	        "Where:\n"
	        "  \033[36;1mDIRECTORY\033[0m is the name of the directory to change to.\n"
	        "\n"
	        "Notes:\n"
	        "  Running \033[32;1mcd\033[0m without an argument displays the current directory.\n"
	        "  With \033[36;1mDIRECTORY\033[0m the command only changes the directory, not the current drive.\n"
	        "\n"
	        "Examples:\n"
	        "  \033[32;1mcd\033[0m\n"
	        "  \033[32;1mcd\033[0m \033[36;1mmydir\033[0m\n");
	MSG_Add("SHELL_CMD_CLS_HELP", "Clears the DOS screen.\n");
	MSG_Add("SHELL_CMD_CLS_HELP_LONG",
	        "Usage:\n"
	        "  \033[32;1mcls\033[0m\n"
	        "\n"
	        "Where:\n"
	        "  This command has no parameters.\n"
	        "\n"
	        "Notes:\n"
	        "  Running \033[32;1mcls\033[0m clears all texts on the DOS screen, except for the command\n"
	        "  prompt (e.g. \033[37;1mZ:\\>\033[0m or \033[37;1mC:\\GAMES>\033[0m) on the top-left corner of the screen.\n"
	        "\n"
	        "Examples:\n"
	        "  \033[32;1mcls\033[0m\n");
	MSG_Add("SHELL_CMD_DIR_HELP",
	        "Displays a list of files and subdirectories in a directory.\n");
	MSG_Add("SHELL_CMD_DIR_HELP_LONG",
	        "Usage:\n"
	        "  \033[32;1mdir\033[0m \033[36;1m[PATTERN]\033[0m [/w] [/b] [/p] [ad] [a-d] [/o\033[37;1mORDER\033[0m]\n"
	        "\n"
	        "Where:\n"
	        "  \033[36;1mPATTERN\033[0m is either an exact filename or an inexact filename with wildcards,\n"
	        "          which are the asterisk (*) and the question mark (?). A path can be\n"
	        "          specified in the pattern to list contents in the specified directory.\n"
	        "  \033[37;1mORDER\033[0m   is a listing order, including \033[37;1mn\033[0m (by name, alphabetic), \033[37;1ms\033[0m (by size,\n"
	        "          smallest first), \033[37;1me\033[0m (by extension, alphabetic), \033[37;1md\033[0m (by date/time,\n"
	        "          oldest first), with an optional \033[37;1m-\033[0m prefix to reverse order.\n"
	        "  /w      lists 5 files/directories in a row; /b      lists the names only.\n"
	        "  /o\033[37;1mORDER\033[0m orders the list (see above)         /p      pauses after each screen.\n"
	        "  /ad     lists all directories;              /a-d    lists all files.\n"
	        "\n"
	        "Notes:\n"
	        "  Running \033[32;1mdir\033[0m without an argument lists all files and subdirectories in the\n"
	        "  current directory, which is the same as \033[32;1mdir\033[0m \033[36;1m*.*\033[0m.\n"
	        "\n"
	        "Examples:\n"
	        "  \033[32;1mdir\033[0m \033[36;1m\033[0m\n"
	        "  \033[32;1mdir\033[0m \033[36;1mgames.*\033[0m /p\n"
	        "  \033[32;1mdir\033[0m \033[36;1mc:\\games\\*.exe\033[0m /b /o\033[37;1m-d\033[0m\n");
	MSG_Add("SHELL_CMD_ECHO_HELP",
	        "Displays messages and enables/disables command echoing.\n");
	MSG_Add("SHELL_CMD_ECHO_HELP_LONG",
	        "Usage:\n"
	        "  \033[32;1mecho\033[0m \033[36;1m[on|off]\033[0m\n"
	        "  \033[32;1mecho\033[0m \033[36;1m[MESSAGE]\033[0m\n"
	        "\n"
	        "Where:\n"
	        "  \033[36;1mon|off\033[0m  Turns on/off command echoing.\n"
	        "  \033[36;1mMESSAGE\033[0m The message to display.\n"
	        "\n"
	        "Notes:\n"
	        "  - Running \033[32;1mecho\033[0m without an argument shows the current on or off status.\n"
	        "  - Echo is especially useful when writing or debugging batch files.\n"
	        "\n"
	        "Examples:\n"
	        "  \033[32;1mecho\033[0m \033[36;1moff\033[0m\n"
	        "  \033[32;1mecho\033[0m \033[36;1mHello world!\033[0m\n");
	MSG_Add("SHELL_CMD_EXIT_HELP", "Exits from the DOS shell.\n");
	MSG_Add("SHELL_CMD_EXIT_HELP_LONG",
	        "Usage:\n"
	        "  \033[32;1mexit\033[0m\n"
	        "\n"
	        "Where:\n"
	        "  This command has no parameters.\n"
	        "\n"
	        "Notes:\n"
	        "  If you start a DOS shell from a program, running \033[32;1mexit\033[0m returns to the program.\n"
	        "  If there is no DOS program running, the command quits from DOSBox Staging.\n"
	        "\n"
	        "Examples:\n"
	        "  \033[32;1mexit\033[0m\n");
	MSG_Add("SHELL_CMD_EXIT_TOO_SOON", "Preventing an early 'exit' call from terminating.\n");
	MSG_Add("SHELL_CMD_HELP_HELP",
	        "Displays help information for DOS commands.\n");
	MSG_Add("SHELL_CMD_HELP_HELP_LONG",
	        "Usage:\n"
	        "  \033[32;1mhelp\033[0m\n"
	        "  \033[32;1mhelp\033[0m /a[ll]\n"
	        "  \033[32;1mhelp\033[0m \033[36;1mCOMMAND\033[0m\n"
	        "\n"
	        "Where:\n"
	        "  \033[36;1mCOMMAND\033[0m is the name of an internal DOS command, such as \033[36;1mdir\033[0m.\n"
	        "\n"
	        "Notes:\n"
	        "  - Running \033[32;1mecho\033[0m without an argument displays a DOS command list.\n"
	        "  - You can view a full list of internal commands with the /a or /all option.\n"
	        "  - Instead of \033[32;1mhelp\033[0m \033[36;1mCOMMAND\033[0m, you can also get command help with \033[36;1mCOMMAND\033[0m /?.\n"
	        "\n"
	        "Examples:\n"
	        "  \033[32;1mhelp\033[0m \033[36;1mdir\033[0m\n"
	        "  \033[32;1mhelp\033[0m /all\n");
	MSG_Add("SHELL_CMD_INTRO_HELP",
	        "Displays a full-screen introduction to DOSBox Staging.\n");
	MSG_Add("SHELL_CMD_INTRO_HELP_LONG",
	        "Usage:\n"
	        "  \033[32;1mintro\033[0m\n"
	        "  \033[32;1mintro\033[0m \033[37;1mPAGE\033[0m\n"
	        "\n"
	        "Where:\n"
	        "  \033[37;1mPAGE\033[0m is the page name to display, including \033[37;1mcdrom\033[0m, \033[37;1mmount\033[0m, and \033[37;1mspecial\033[0m.\n"
	        "\n"
	        "Notes:\n"
	        "  Running \033[32;1mintro\033[0m without an argument displays one information page at a time;\n"
	        "  press any key to move to the next page. If a page name is provided, then the\n"
	        "  specified page will be displayed directly.\n"
	        "\n"
	        "Examples:\n"
	        "  \033[32;1mintro\033[0m\n"
	        "  \033[32;1mintro\033[0m \033[37;1mcdrom\033[0m\n");
	MSG_Add("SHELL_CMD_MKDIR_HELP", "Creates a directory.\n");
	MSG_Add("SHELL_CMD_MKDIR_HELP_LONG",
	        "Usage:\n"
	        "  \033[32;1mmd\033[0m \033[36;1mDIRECTORY\033[0m\n"
	        "  \033[32;1mmkdir\033[0m \033[36;1mDIRECTORY\033[0m\n"
	        "\n"
	        "Where:\n"
	        "  \033[36;1mDIRECTORY\033[0m is the name of the directory to create.\n"
	        "\n"
	        "Notes:\n"
	        "  - The directory must be an exact name and does not yet exist.\n"
	        "  - You can specify a path where the directory will be created.\n"
	        "\n"
	        "Examples:\n"
	        "  \033[32;1mmd\033[0m \033[36;1mnewdir\033[0m\n"
	        "  \033[32;1mmd\033[0m \033[36;1mc:\\games\\dir\033[0m\n");
	MSG_Add("SHELL_CMD_RMDIR_HELP", "Removes a directory.\n");
	MSG_Add("SHELL_CMD_RMDIR_HELP_LONG",
	        "Usage:\n"
	        "  \033[32;1mrd\033[0m \033[36;1mDIRECTORY\033[0m\n"
	        "  \033[32;1mrmdir\033[0m \033[36;1mDIRECTORY\033[0m\n"
	        "\n"
	        "Where:\n"
	        "  \033[36;1mDIRECTORY\033[0m is the name of the directory to remove.\n"
	        "\n"
	        "Notes:\n"
	        "  The directory must be empty with no files or subdirectories.\n"
	        "\n"
	        "Examples:\n"
	        "  \033[32;1mrd\033[0m \033[36;1memptydir\033[0m\n");
	MSG_Add("SHELL_CMD_SET_HELP", "Displays or changes environment variables.\n");
	MSG_Add("SHELL_CMD_SET_HELP_LONG",
	        "Usage:\n"
	        "  \033[32;1mset\033[0m\n"
	        "  \033[32;1mset\033[0m \033[37;1mVARIABLE\033[0m=\033[36;1m[STRING]\033[0m\n"
	        "\n"
	        "Where:\n"
	        "  \033[37;1mVARIABLE\033[0m The name of the environment variable.\n"
	        "  \033[36;1mSTRING\033[0m   A series of characters to assign to the variable.\n"
	        "\n"
	        "Notes:\n"
	        "  - Assigning an empty string to the variable removes the variable.\n"
	        "  - The command without a parameter displays current environment variables.\n"
	        "\n"
	        "Examples:\n"
	        "  \033[32;1mset\033[0m\n"
	        "  \033[32;1mset\033[0m \033[37;1mname\033[0m=\033[36;1mvalue\033[0m\n");
	MSG_Add("SHELL_CMD_IF_HELP",
	        "Performs conditional processing in batch programs.\n");
	MSG_Add("SHELL_CMD_IF_HELP_LONG",
	        "Usage:\n"
	        "  \033[32;1mif\033[0m \033[35;1m[not]\033[0m \033[36;1merrorlevel\033[0m \033[37;1mNUMBER\033[0m COMMAND\n"
	        "  \033[32;1mif\033[0m \033[35;1m[not]\033[0m \033[37;1mSTR1==STR2\033[0m COMMAND\n"
	        "  \033[32;1mif\033[0m \033[35;1m[not]\033[0m \033[36;1mexist\033[0m \033[37;1mFILE\033[0m COMMAND\n"
	        "\n"
	        "Where:\n"
	        "  \033[37;1mNUMBER\033[0m     is a positive integer less or equal to the desired value.\n"
	        "  \033[37;1mSTR1==STR2\033[0m compares two text strings (case-sensitive).\n"
	        "  \033[37;1mFILE\033[0m       is an exact file name to check for existence.\n"
	        "  COMMAND    is a DOS command or program to run, optionally with parameters.\n"
	        "\n"
	        "Notes:\n"
	        "  The COMMAND is run if any of the three conditions in the usage are met.\n"
	        "  If \033[38;1mnot\033[0m is specified, then the command runs only with the false condition.\n"
	        "  The \033[36;1merrorlevel\033[0m condition is useful for checking if a programs ran correctly.\n"
	        "  If either \033[37;1mSTR1\033[0m or \033[37;1mSTR2\033[0m may be empty, you can enclose them in quotes (\").\n"
	        "\n"
	        "Examples:\n"
	        "  \033[32;1mif\033[0m \033[36;1merrorlevel\033[0m \033[37;1m2\033[0m dir\n"
	        "  \033[32;1mif\033[0m \033[37;1m\"%%myvar%%\"==\"mystring\"\033[0m echo Hello world!\n"
	        "  \033[32;1mif\033[0m \033[35;1mnot\033[0m \033[36;1mexist\033[0m \033[37;1mfile.txt\033[0m exit\n");
	MSG_Add("SHELL_CMD_GOTO_HELP",
	        "Jumps to a labeled line in a batch program.\n");
	MSG_Add("SHELL_CMD_GOTO_HELP_LONG",
	        "Usage:\n"
	        "  \033[32;1mgoto\033[0m \033[36;1mLABEL\033[0m\n"
	        "\n"
	        "Where:\n"
	        "  \033[36;1mLABEL\033[0m is text string used in the batch program as a label.\n"
	        "\n"
	        "Notes:\n"
	        "  A label is on a line by itself, beginning with a colon (:).\n"
	        "  The label must be unique, and can be anywhere within the batch program.\n"
	        "\n"
	        "Examples:\n"
	        "  \033[32;1mgoto\033[0m \033[36;1mmylabel\033[0m\n");
	MSG_Add("SHELL_CMD_SHIFT_HELP","Left-shifts command-line parameters in a batch program.\n");
	MSG_Add("SHELL_CMD_SHIFT_HELP_LONG",
	        "Usage:\n"
	        "  \033[32;1mshift\033[0m\n"
	        "\n"
	        "Where:\n"
	        "  This command has no parameters.\n"
	        "\n"
	        "Notes:\n"
	        "  This command allows a DOS batch program to accept more than 9 parameters.\n"
	        "  Running \033[32;1mshift\033[0m left-shifts the batch program variable %%1 to %%0, %%2 to %%1, etc.\n"
	        "\n"
	        "Examples:\n"
	        "  \033[32;1mshift\033[0m\n");
	MSG_Add("SHELL_CMD_TYPE_HELP", "Display the contents of a text file.\n");
	MSG_Add("SHELL_CMD_TYPE_HELP_LONG",
	        "Usage:\n"
	        "  \033[32;1mtype\033[0m \033[36;1mFILE\033[0m\n"
	        "\n"
	        "Where:\n"
	        "  \033[36;1mFILE\033[0m is the name of the file to display.\n"
	        "\n"
	        "Notes:\n"
	        "  The file must be an exact file name, optionally with a path.\n"
	        "  This command is only for viewing text files, not binary files.\n"
	        "\n"
	        "Examples:\n"
	        "  \033[32;1mtype\033[0m \033[36;1mtext.txt\033[0m\n"
	        "  \033[32;1mtype\033[0m \033[36;1mc:\\dos\\readme.txt\033[0m\n");
	MSG_Add("SHELL_CMD_REM_HELP", "Adds comments in a batch program.\n");
	MSG_Add("SHELL_CMD_REM_HELP_LONG",
	        "Usage:\n"
	        "  \033[32;1mrem\033[0m \033[36;1mCOMMENT\033[0m\n"
	        "\n"
	        "Where:\n"
	        "  \033[36;1mCOMMENT\033[0m is any comment you want to add.\n"
	        "\n"
	        "Notes:\n"
	        "  Adding comments to a batch program can make it easier to understand.\n"
	        "  You can also temporarily comment out some commands with this command.\n"
	        "\n"
	        "Examples:\n"
	        "  \033[32;1mrem\033[0m \033[36;1mThis is my test batch program.\033[0m\n");
	MSG_Add("SHELL_CMD_NO_WILD","This is a simple version of the command, no wildcards allowed!\n");
	MSG_Add("SHELL_CMD_RENAME_HELP", "Renames one or more files.\n");
	MSG_Add("SHELL_CMD_RENAME_HELP_LONG",
	        "Usage:\n"
	        "  \033[32;1mren\033[0m \033[37;1mSOURCE\033[0m \033[36;1mDESTINATION\033[0m\n"
	        "  \033[32;1mrename\033[0m \033[37;1mSOURCE\033[0m \033[36;1mDESTINATION\033[0m\n"
	        "\n"
	        "Where:\n"
	        "  \033[37;1mSOURCE\033[0m      is the name of the file to rename.\n"
	        "  \033[36;1mDESTINATION\033[0m is the new name for the renamed file.\n"
	        "\n"
	        "Notes:\n"
	        "  - The source file must be an exact file name, optionally with a path.\n"
	        "  - The destination file must be an exact file name without a path.\n"
	        "\n"
	        "Examples:\n"
	        "  \033[32;1mren\033[0m \033[37;1moldname\033[0m \033[36;1mnewname\033[0m\n"
	        "  \033[32;1mren\033[0m \033[37;1mc:\\dos\\file.txt\033[0m \033[36;1mf.txt\033[0m\n");
	MSG_Add("SHELL_CMD_DELETE_HELP","Removes one or more files.\n");
	MSG_Add("SHELL_CMD_DELETE_HELP_LONG", "Usage:\n"
	        "  \033[32;1mdel\033[0m \033[36;1mPATTERN\033[0m\n"
	        "  \033[32;1merase\033[0m \033[36;1mPATTERN\033[0m\n"
	        "\n"
	        "Where:\n"
	        "  \033[36;1mPATTERN\033[0m can be either an exact filename (such as \033[36;1mfile.txt\033[0m) or an inexact\n"
	        "          filename using one or more wildcards, which are the asterisk (*)\n"
	        "          representing any sequence of one or more characters, and the question\n"
	        "          mark (?) representing any single character, such as \033[36;1m*.bat\033[0m and \033[36;1mc?.txt\033[0m.\n"
	        "\n"
	        "Warning:\n"
	        "  Be careful when using a pattern with wildcards, especially \033[36;1m*.*\033[0m, as all files\n"
	        "  matching the pattern will be deleted.\n"
	        "\n"
	        "Examples:\n"
	        "  \033[32;1mdel\033[0m \033[36;1mtest.bat\033[0m\n"
	        "  \033[32;1mdel\033[0m \033[36;1mc*.*\033[0m\n"
	        "  \033[32;1mdel\033[0m \033[36;1ma?b.c*\033[0m\n");
	MSG_Add("SHELL_CMD_COPY_HELP", "Copies one or more files.\n");
	MSG_Add("SHELL_CMD_COPY_HELP_LONG",
	        "Usage:\n"
	        "  \033[32;1mcopy\033[0m \033[37;1mSOURCE\033[0m \033[36;1m[DESTINATION]\033[0m\n"
	        "  \033[32;1mcopy\033[0m \033[37;1mSOURCE1+SOURCE2[+...]\033[0m \033[36;1m[DESTINATION]\033[0m\n"
	        "\n"
	        "Where:\n"
	        "  \033[37;1mSOURCE\033[0m      Can be either an exact filename or an inexact filename with\n"
	        "              wildcards, which are the asterisk (*) and the question mark (?).\n"
	        "  \033[36;1mDESTINATION\033[0m An exact filename or directory, not containing any wildcards.\n"
	        "\n"
	        "Notes:\n"
	        "  The \033[37;1m+\033[0m operator combines multiple source files provided to a single file.\n"
	        "  Destination is optional: if omitted, files are copied to the current path.\n"
	        "\n"
	        "Examples:\n"
	        "  \033[32;1mcopy\033[0m \033[37;1msource.bat\033[0m \033[36;1mnew.bat\033[0m\n"
	        "  \033[32;1mcopy\033[0m \033[37;1mfile1.txt+file2.txt\033[0m \033[36;1mfile3.txt\033[0m\n"
	        "  \033[32;1mcopy\033[0m \033[37;1m..\\c*.*\033[0m\n");
	MSG_Add("SHELL_CMD_CALL_HELP",
	        "Starts a batch program from within another batch program.\n");
	MSG_Add("SHELL_CMD_CALL_HELP_LONG",
	        "Usage:\n"
	        "  \033[32;1mcall\033[0m \033[37;1mBATCH\033[0m \033[36;1m[PARAMETERS]\033[0m\n"
	        "\n"
	        "Where:\n"
	        "  \033[37;1mBATCH\033[0m      is a batch program to launch.\n"
	        "  \033[36;1mPARAMETERS\033[0m are optional parameters for the batch program.\n"
	        "\n"
	        "Notes:\n"
	        "  After calling another batch program, the original batch program will\n"
	        "  resume running after the other batch program ends.\n"
	        "\n"
	        "Examples:\n"
	        "  \033[32;1mcall\033[0m \033[37;1mmybatch.bat\033[0m\n"
	        "  \033[32;1mcall\033[0m \033[37;1mfile.bat\033[0m \033[36;1mHello world!\033[0m\n");
	MSG_Add("SHELL_CMD_SUBST_HELP", "Assign an internal directory to a drive.\n");
	MSG_Add("SHELL_CMD_SUBST_HELP_LONG",
	        "Usage:\n"
	        "  \033[32;1msubst\033[0m \033[37;1mDRIVE\033[0m \033[36;1mPATH\033[0m\n"
	        "  \033[32;1msubst\033[0m \033[37;1mDRIVE\033[0m /d\n"
	        "\n"
	        "Where:\n"
	        "  \033[37;1mDRIVE\033[0m is a drive to which you want to assign a path.\n"
	        "  \033[36;1mPATH\033[0m  is a mounted DOS path you want to assign to.\n"
	        "\n"
	        "Notes:\n"
	        "  The path must be on a drive mounted by the \033[32;1mmount\033[0m command.\n"
	        "  You can remove an assigned drive with the /d option.\n"
	        "\n"
	        "Examples:\n"
	        "  \033[32;1msubst\033[0m \033[37;1md:\033[0m \033[36;1mc:\\games\033[0m\n"
	        "  \033[32;1msubst\033[0m \033[37;1me:\033[0m \033[36;1m/d\033[0m\n");
	MSG_Add("SHELL_CMD_LOADHIGH_HELP", "Loads a DOS program into upper memory.\n");
	MSG_Add("SHELL_CMD_LOADHIGH_HELP_LONG",
	        "Usage:\n"
	        "  \033[32;1mlh\033[0m \033[36;1mPROGRAM\033[0m \033[37;1m[PARAMETERS]\033[0m\n"
	        "  \033[32;1mloadhigh\033[0m \033[36;1mPROGRAM\033[0m \033[37;1m[PARAMETERS]\033[0m\n"
	        "\n"
	        "Where:\n"
	        "  \033[36;1mPROGRAM\033[0m is a DOS TSR program to be loaded, optionally with parameters.\n"
	        "\n"
	        "Notes:\n"
	        "  This command intends to save the conventional memory by loading specified DOS\n"
	        "  TSR programs into upper memory if possible. Such programs may be required for\n"
	        "  some DOS games; XMS and UMB memory must be enabled (xms=true and umb=true).\n"
	        "  Not all DOS TSR programs can be loaded into upper memory with this command.\n"
	        "\n"
	        "Examples:\n"
	        "  \033[32;1mlh\033[0m \033[36;1mtsrapp\033[0m \033[37;1margs\033[0m\n");
	MSG_Add("SHELL_CMD_LS_HELP",
	        "Displays directory contents in the wide list format.\n");
	MSG_Add("SHELL_CMD_LS_HELP_LONG",
	        "Usage:\n"
	        "  \033[32;1mls\033[0m \033[36;1mPATTERN\033[0m\n"
	        "  \033[32;1mls\033[0m \033[36;1mPATH\033[0m\n"
	        "\n"
	        "Where:\n"
	        "  \033[36;1mPATTERN\033[0m can be either an exact filename or an inexact filename with\n"
	        "          wildcards, which are the asterisk (*) and the question mark (?).\n"
	        "  \033[36;1mPATH\033[0m    is an exact path in a mounted DOS drive to list contents.\n"
	        "\n"
	        "Notes:\n"
	        "  The command will list directories in \033[34;1mblue\033[0m, executable DOS programs\n"
	        "   (*.com, *.exe, *.bat) in \033[32;1mgreen\033[0m, and other files in the normal color.\n"
	        "\n"
	        "Examples:\n"
	        "  \033[32;1mls\033[0m \033[36;1mfile.txt\033[0m\n"
	        "  \033[32;1mls\033[0m \033[36;1mc*.ba?\033[0m\n");
	MSG_Add("SHELL_CMD_LS_PATH_ERR",
	        "ls: cannot access '%s': No such file or directory\n");

	MSG_Add("SHELL_CMD_CHOICE_HELP",
	        "Waits for a keypress and sets an ERRORLEVEL value.\n");
	MSG_Add("SHELL_CMD_CHOICE_HELP_LONG",
	        "Usage:\n"
	        "  \033[32;1mchoice\033[0m \033[36;1m[TEXT]\033[0m\n"
	        "  \033[32;1mchoice\033[0m /c[:]\033[37;1mCHOICES\033[0m [/n] [/s] \033[36;1m[TEXT]\033[0m\n"
	        "\n"
	        "Where:\n"
	        "  \033[36;1mTEXT\033[0m         is the text to display as a prompt, or empty.\n"
	          "  /c[:]\033[37;1mCHOICES\033[0m Specifies allowable keys, which default to \033[37;1myn\033[0m.\n"
	          "  /n           Do not display the choices at end of prompt.\n"
	          "  /s           Enables case-sensitive choices to be selected.\n"
	        "\n"
	        "Notes:\n"
	        "  This command sets an ERRORLEVEL value starting from 1 according to the\n"
	        "  allowable keys specified in /c option, and the user input can then be checked\n"
	        "  with \033[32;1mif\033[0m command. With /n option only the specified text will be displayed,\n"
	        "  but not the actual choices (such as the default \033[37;1m[Y,N]?\033[0m) in the end.\n"
	        "\n"
	        "Examples:\n"
	        "  \033[32;1mchoice\033[0m \033[36;1mContinue?\033[0m\n"
	        "  \033[32;1mchoice\033[0m /c:\033[37;1mabc\033[0m /s \033[36;1mType the letter a, b, or c\033[0m\n");
	MSG_Add("SHELL_CMD_PATH_HELP",
	        "Displays or sets a search path for executable files.\n");
	MSG_Add("SHELL_CMD_PATH_HELP_LONG",
	        "Usage:\n"
	        "  \033[32;1mpath\033[0m\n"
	        "  \033[32;1mpath\033[0m \033[36;1m[[drive:]path[;...]\033[0m\n"
	        "\n"
	        "Where:\n"
	        "  \033[36;1m[[drive:]path[;...]\033[0m is a path containing a drive and directory.\n"
	        "  More than one path can be specified, separated by a semi-colon (;).\n"
	        "\n"
	        "Notes:\n"
	        "  Parameter with a semi-colon (;) only clears all search path settings.\n"
	        "  The path can also be set using \033[32;1mset\033[0m command, e.g. \033[32;1mset\033[0m \033[37;1mpath\033[0m=\033[36;1mZ:\\\033[0m\n"
	        "\n"
	        "Examples:\n"
	        "  \033[32;1mpath\033[0m\n"
	        "  \033[32;1mpath\033[0m \033[36;1mZ:\\;C:\\DOS\033[0m\n");
	MSG_Add("SHELL_CMD_VER_HELP", "View or set the reported DOS version.\n");
	MSG_Add("SHELL_CMD_VER_HELP_LONG", "Usage:\n"
	        "  \033[32;1mver\033[0m\n"
	        "  \033[32;1mver\033[0m \033[37;1mset\033[0m \033[36;1mVERSION\033[0m\n"
	        "\n"
	        "Where:\n"
	        "  \033[36;1mVERSION\033[0m can be a whole number, such as \033[36;1m5\033[0m, or include a two-digit decimal\n"
	        "          value, such as: \033[36;1m6.22\033[0m, \033[36;1m7.01\033[0m, or \033[36;1m7.10\033[0m. The decimal can alternatively be\n"
	        "          space-separated, such as: \033[36;1m6 22\033[0m, \033[36;1m7 01\033[0m, or \033[36;1m7 10\033[0m.\n"
	        "\n"
	        "Notes:\n"
	        "  The DOS version can also be set in the configuration file under the [dos]\n"
	        "  section using the \"ver = \033[36;1mVERSION\033[0m\" setting.\n"
	        "\n"
	        "Examples:\n"
	        "  \033[32;1mver\033[0m \033[37;1mset\033[0m \033[36;1m6.22\033[0m\n"
	        "  \033[32;1mver\033[0m \033[37;1mset\033[0m \033[36;1m7 10\033[0m\n");
	MSG_Add("SHELL_CMD_VER_VER", "DOSBox Staging version %s\n"
	                             "DOS version %d.%02d\n");
	MSG_Add("SHELL_CMD_VER_INVALID", "The specified DOS version is not correct.\n");

	/* Regular startup */
	call_shellstop=CALLBACK_Allocate();
	/* Setup the startup CS:IP to kill the last running machine when exitted */
	RealPt newcsip=CALLBACK_RealPointer(call_shellstop);
	SegSet16(cs,RealSeg(newcsip));
	reg_ip=RealOff(newcsip);

	CALLBACK_Setup(call_shellstop,shellstop_handler,CB_IRET,"shell stop");
	PROGRAMS_MakeFile("COMMAND.COM",SHELL_ProgramStart);

	/* Now call up the shell for the first time */
	Bit16u psp_seg=DOS_FIRST_SHELL;
	Bit16u env_seg=DOS_FIRST_SHELL+19; //DOS_GetMemory(1+(4096/16))+1;
	Bit16u stack_seg=DOS_GetMemory(2048/16);
	SegSet16(ss,stack_seg);
	reg_sp=2046;

	/* Set up int 24 and psp (Telarium games) */
	real_writeb(psp_seg+16+1,0,0xea);		/* far jmp */
	real_writed(psp_seg+16+1,1,real_readd(0,0x24*4));
	real_writed(0,0x24*4,((Bit32u)psp_seg<<16) | ((16+1)<<4));

	/* Set up int 23 to "int 20" in the psp. Fixes what.exe */
	real_writed(0,0x23*4,((Bit32u)psp_seg<<16));

	/* Set up int 2e handler */
	Bitu call_int2e=CALLBACK_Allocate();
	RealPt addr_int2e=RealMake(psp_seg+16+1,8);
	CALLBACK_Setup(call_int2e,&INT2E_Handler,CB_IRET_STI,Real2Phys(addr_int2e),"Shell Int 2e");
	RealSetVec(0x2e,addr_int2e);

	/* Setup MCBs */
	DOS_MCB pspmcb((Bit16u)(psp_seg-1));
	pspmcb.SetPSPSeg(psp_seg);	// MCB of the command shell psp
	pspmcb.SetSize(0x10+2);
	pspmcb.SetType(0x4d);
	DOS_MCB envmcb((Bit16u)(env_seg-1));
	envmcb.SetPSPSeg(psp_seg);	// MCB of the command shell environment
	envmcb.SetSize(DOS_MEM_START-env_seg);
	envmcb.SetType(0x4d);

	/* Setup environment */
	PhysPt env_write=PhysMake(env_seg,0);
	MEM_BlockWrite(env_write,path_string,(Bitu)(strlen(path_string)+1));
	env_write += (PhysPt)(strlen(path_string)+1);
	MEM_BlockWrite(env_write,comspec_string,(Bitu)(strlen(comspec_string)+1));
	env_write += (PhysPt)(strlen(comspec_string)+1);
	mem_writeb(env_write++,0);
	mem_writew(env_write,1);
	env_write+=2;
	MEM_BlockWrite(env_write,full_name,(Bitu)(strlen(full_name)+1));

	DOS_PSP psp(psp_seg);
	psp.MakeNew(0);
	dos.psp(psp_seg);

	/* The start of the filetable in the psp must look like this:
	 * 01 01 01 00 02
	 * In order to achieve this: First open 2 files. Close the first and
	 * duplicate the second (so the entries get 01) */
	Bit16u dummy=0;
	DOS_OpenFile("CON",OPEN_READWRITE,&dummy);	/* STDIN  */
	DOS_OpenFile("CON",OPEN_READWRITE,&dummy);	/* STDOUT */
	DOS_CloseFile(0);							/* Close STDIN */
	DOS_ForceDuplicateEntry(1,0);				/* "new" STDIN */
	DOS_ForceDuplicateEntry(1,2);				/* STDERR */
	DOS_OpenFile("CON",OPEN_READWRITE,&dummy);	/* STDAUX */
	DOS_OpenFile("PRN",OPEN_READWRITE,&dummy);	/* STDPRN */

	/* Create appearance of handle inheritance by first shell */
	for (Bit16u i=0;i<5;i++) {
		Bit8u handle=psp.GetFileHandle(i);
		if (Files[handle]) Files[handle]->AddRef();
	}

	psp.SetParent(psp_seg);
	/* Set the environment */
	psp.SetEnvironment(env_seg);
	/* Set the command line for the shell start up */
	CommandTail tail;
	tail.count=(Bit8u)strlen(init_line);
	memset(&tail.buffer,0,127);
	safe_strcpy(tail.buffer, init_line);
	MEM_BlockWrite(PhysMake(psp_seg,128),&tail,128);

	/* Setup internal DOS Variables */
	dos.dta(RealMake(psp_seg,0x80));
	dos.psp(psp_seg);


	SHELL_ProgramStart_First_shell(&first_shell);
	first_shell->Run();
	delete first_shell;
	first_shell = nullptr; // Make clear that it shouldn't be used anymore
}
