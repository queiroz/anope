/* Initalization and related routines.
 *
 * (C) 2003-2012 Anope Team
 * Contact us at team@anope.org
 *
 * Please read COPYING and README for further details.
 *
 * Based on the original code of Epona by Lara.
 * Based on the original code of Services by Andy Church.
 *
 */

#include "services.h"
#include "config.h"
#include "users.h"
#include "protocol.h"
#include "bots.h"
#include "xline.h"
#include "socketengine.h"
#include "servers.h"
#include "language.h"

#ifndef _WIN32
#include <sys/wait.h>
#include <sys/stat.h>
#endif

Anope::string Anope::ConfigDir = "conf", Anope::DataDir = "data", Anope::ModuleDir = "lib", Anope::LocaleDir = "locale", Anope::LogDir = "logs";

/* Vector of pairs of command line arguments and their params */
static std::vector<std::pair<Anope::string, Anope::string> > CommandLineArguments;

/** Called on startup to organize our starting arguments in a better way
 * and check for errors
 * @param ac number of args
 * @param av args
 */
static void ParseCommandLineArguments(int ac, char **av)
{
	for (int i = 1; i < ac; ++i)
	{
		Anope::string option = av[i];
		Anope::string param;
		while (!option.empty() && option[0] == '-')
			option.erase(option.begin());
		size_t t = option.find('=');
		if (t != Anope::string::npos)
		{
			param = option.substr(t + 1);
			option.erase(t);
		}

		if (option.empty())
			continue;

		CommandLineArguments.push_back(std::make_pair(option, param));
	}
}

/** Check if an argument was given on startup and its parameter
 * @param name The argument name
 * @param shortname A shorter name, eg --debug and -d
 * @param param A string to put the param, if any, of the argument
 * @return true if name/shortname was found, false if not
 */
static bool GetCommandLineArgument(const Anope::string &name, char shortname, Anope::string &param)
{
	param.clear();

	for (std::vector<std::pair<Anope::string, Anope::string> >::iterator it = CommandLineArguments.begin(), it_end = CommandLineArguments.end(); it != it_end; ++it)
	{
		if (it->first.equals_ci(name) || (it->first.length() == 1 && it->first[0] == shortname))
		{
			param = it->second;
			return true;
		}
	}

	return false;
}

/** Check if an argument was given on startup
 * @param name The argument name
 * @param shortname A shorter name, eg --debug and -d
 * @return true if name/shortname was found, false if not
 */
static bool GetCommandLineArgument(const Anope::string &name, char shortname = 0)
{
	Anope::string Unused;
	return GetCommandLineArgument(name, shortname, Unused);
}

bool Anope::AtTerm()
{
	return isatty(fileno(stdout)) && isatty(fileno(stdin)) && isatty(fileno(stderr));
}

void Anope::Fork()
{
#ifndef _WIN32
	kill(getppid(), SIGUSR2);
	
	freopen("/dev/null", "r", stdin);
	freopen("/dev/null", "w", stdout);
	freopen("/dev/null", "w", stderr);

	setpgid(0, 0);
#else
	FreeConsole();
#endif
}

void Anope::HandleSignal()
{
	switch (Signal)
	{
		case SIGHUP:
		{
			Anope::SaveDatabases();

			ServerConfig *old_config = Config;
			try
			{
				Config = new ServerConfig();
				FOREACH_MOD(I_OnReload, OnReload());
				delete old_config;
			}
			catch (const ConfigException &ex)
			{
				Config = old_config;
				Log() << "Error reloading configuration file: " << ex.GetReason();
			}
			break;
		}
		case SIGTERM:
		case SIGINT:
#ifndef _WIN32
			Log() << "Received " << strsignal(Signal) << " signal (" << Signal << "), exiting.";
			Anope::QuitReason = Anope::string("Services terminating via signal ") + strsignal(Signal) + " (" + stringify(Signal) + ")";
#else
			Log() << "Received signal " << Signal << ", exiting.";
			Anope::QuitReason = Anope::string("Services terminating via signal ") + stringify(Signal);
#endif
			Anope::SaveDatabases();
			Anope::Quitting = true;
			break;
	}

	Signal = 0;
}

#ifndef _WIN32
static void parent_signal_handler(int signal)
{
	if (signal == SIGUSR2)
	{
		Anope::Quitting = true;
	}
	else if (signal == SIGCHLD)
	{
		Anope::ReturnValue = -1;
		Anope::Quitting = true;
		int status = 0;
		wait(&status);
		if (WIFEXITED(status))
			Anope::ReturnValue = WEXITSTATUS(status);
	}
}
#endif

static void SignalHandler(int sig)
{
	Anope::Signal = sig;
}

static void InitSignals()
{
	struct sigaction sa;

	sa.sa_flags = 0;
	sigemptyset(&sa.sa_mask);

	sa.sa_handler = SignalHandler;

	sigaction(SIGHUP, &sa, NULL);

	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	sa.sa_handler = SIG_IGN;

	sigaction(SIGCHLD, &sa, NULL);
	sigaction(SIGPIPE, &sa, NULL);
}

/* Remove our PID file.  Done at exit. */

static void remove_pidfile()
{
	remove(Config->PIDFilename.c_str());
}

/* Create our PID file and write the PID to it. */

static void write_pidfile()
{
	FILE *pidfile = fopen(Config->PIDFilename.c_str(), "w");
	if (pidfile)
	{
#ifdef _WIN32
		fprintf(pidfile, "%d\n", static_cast<int>(GetCurrentProcessId()));
#else
		fprintf(pidfile, "%d\n", static_cast<int>(getpid()));
#endif
		fclose(pidfile);
		atexit(remove_pidfile);
	}
	else
		throw CoreException("Can not write to PID file " + Config->PIDFilename);
}

void Anope::Init(int ac, char **av)
{
	/* Set file creation mask and group ID. */
#if defined(DEFUMASK) && HAVE_UMASK
	umask(DEFUMASK);
#endif

	Serialize::RegisterTypes();

	/* Parse command line arguments */
	ParseCommandLineArguments(ac, av);

	if (GetCommandLineArgument("version", 'v'))
	{
		Log(LOG_TERMINAL) << "Anope-" << Anope::Version() << " -- " << Anope::VersionBuildString();
		throw CoreException();
	}

	if (GetCommandLineArgument("help", 'h'))
	{
		Log(LOG_TERMINAL) << "Anope-" << Anope::Version() << " -- " << Anope::VersionBuildString();
		Log(LOG_TERMINAL) << "Anope IRC Services (http://www.anope.org)";
		Log(LOG_TERMINAL) << "Usage ./" << Anope::ServicesBin << " [options] ...";
		Log(LOG_TERMINAL) << "-c, --config=filename.conf";
		Log(LOG_TERMINAL) << "    --confdir=conf file direcory";
		Log(LOG_TERMINAL) << "    --dbdir=database directory";
		Log(LOG_TERMINAL) << "-d, --debug[=level]";
		Log(LOG_TERMINAL) << "-h, --help";
		Log(LOG_TERMINAL) << "    --localedir=locale directory";
		Log(LOG_TERMINAL) << "    --logdir=logs directory";
		Log(LOG_TERMINAL) << "    --modulesdir=modules directory";
		Log(LOG_TERMINAL) << "-e, --noexpire";
		Log(LOG_TERMINAL) << "-n, --nofork";
		Log(LOG_TERMINAL) << "    --nothird";
		Log(LOG_TERMINAL) << "    --protocoldebug";
		Log(LOG_TERMINAL) << "-r, --readonly";
		Log(LOG_TERMINAL) << "-s, --support";
		Log(LOG_TERMINAL) << "-v, --version";
		Log(LOG_TERMINAL) << "";
		Log(LOG_TERMINAL) << "Further support is available from http://www.anope.org";
		Log(LOG_TERMINAL) << "Or visit us on IRC at irc.anope.org #anope";
		throw CoreException();
	}

	if (GetCommandLineArgument("nofork", 'n'))
		Anope::NoFork = true;

	if (GetCommandLineArgument("support", 's'))
	{
		Anope::NoFork = Anope::NoThird = true;
		++Anope::Debug;
	}

	if (GetCommandLineArgument("readonly", 'r'))
		Anope::ReadOnly = true;

	if (GetCommandLineArgument("nothird"))
		Anope::NoThird = true;

	if (GetCommandLineArgument("noexpire", 'e'))
		Anope::NoExpire = true;

	if (GetCommandLineArgument("protocoldebug"))
		Anope::ProtocolDebug = true;

	Anope::string arg;
	if (GetCommandLineArgument("debug", 'd', arg))
	{
		if (!arg.empty())
		{
			int level = arg.is_number_only() ? convertTo<int>(arg) : -1;
			if (level > 0)
				Anope::Debug = level;
			else
				throw CoreException("Invalid option given to --debug");
		}
		else
			++Anope::Debug;
	}

	if (GetCommandLineArgument("config", 'c', arg))
	{
		if (arg.empty())
			throw CoreException("The --config option requires a file name");
		ServicesConf = ConfigurationFile(arg, false);
	}

	if (GetCommandLineArgument("confdir", 0, arg))
	{
		if (arg.empty())
			throw CoreException("The --confdir option requires a path");
		Anope::ConfigDir = arg;
	}

	if (GetCommandLineArgument("dbdir", 0, arg))
	{
		if (arg.empty())
			throw CoreException("The --dbdir option requires a path");
		Anope::DataDir = arg;
	}

	if (GetCommandLineArgument("localedir", 0, arg))
	{
		if (arg.empty())
			throw CoreException("The --localedir option requires a path");
		Anope::LocaleDir = arg;
	}

	if (GetCommandLineArgument("modulesdir", 0, arg))
	{
		if (arg.empty())
			throw CoreException("The --modulesdir option requires a path");
		Anope::ModuleDir = arg;
	}

	if (GetCommandLineArgument("logdir", 0, arg))
	{
		if (arg.empty())
			throw CoreException("The --logdir option requires a path");
		Anope::LogDir = arg;
	}

	/* Chdir to Services data directory. */
	if (chdir(Anope::ServicesDir.c_str()) < 0)
	{
		throw CoreException("Unable to chdir to " + Anope::ServicesDir + ": " + Anope::LastError());
	}

	Log(LOG_TERMINAL) << "Anope " << Anope::Version() << ", " << Anope::VersionBuildString();
#ifdef _WIN32
	Log(LOG_TERMINAL) << "Using configuration file " << Anope::ConfigDir << "\\" << ServicesConf.GetName();
#else
	Log(LOG_TERMINAL) << "Using configuration file " << Anope::ConfigDir << "/" << ServicesConf.GetName();
#endif

	/* Initialize the socket engine */
	SocketEngine::Init();

	/* Read configuration file; exit if there are problems. */
	try
	{
		Config = new ServerConfig();
	}
	catch (const ConfigException &ex)
	{
		Log(LOG_TERMINAL) << ex.GetReason();
		Log(LOG_TERMINAL) << "*** Support resources: Read through the services.conf self-contained";
		Log(LOG_TERMINAL) << "*** documentation. Read the documentation files found in the 'docs'";
		Log(LOG_TERMINAL) << "*** folder. Visit our portal located at http://www.anope.org/. Join";
		Log(LOG_TERMINAL) << "*** our support channel on /server irc.anope.org channel #anope.";
		throw CoreException("Configuration file failed to validate");
	}

#ifdef _WIN32
	if (!SupportedWindowsVersion())
		throw CoreException(GetWindowsVersion() + " is not a supported version of Windows");
#else
	/* If we're root, issue a warning now */
	if (!getuid() && !getgid())
	{
		std::cerr << "WARNING: You are currently running Anope as the root superuser. Anope does not" << std::endl;
		std::cerr << "         require root privileges to run, and it is discouraged that you run Anope" << std::endl;
		std::cerr << "         as the root superuser." << std::endl;
		sleep(3);
	}

	if (!Anope::NoFork && Anope::AtTerm())
	{
		/* Install these before fork() - it is possible for the child to
		 * connect and kill() the parent before it is able to install the
		 * handler.
		 */
		struct sigaction sa, old_sigusr2, old_sigchld;

		sa.sa_flags = 0;
		sigemptyset(&sa.sa_mask);
		sa.sa_handler = parent_signal_handler;

		sigaction(SIGUSR2, &sa, &old_sigusr2);
		sigaction(SIGCHLD, &sa, &old_sigchld);

		int i = fork();
		if (i > 0)
		{
			sigset_t mask;

			sigemptyset(&mask);
			sigsuspend(&mask);

			exit(Anope::ReturnValue);
		}
		else if (i == -1)
		{
			Log() << "Error, unable to fork: " << Anope::LastError();
			Anope::NoFork = true;
		}

		/* Child doesn't need these */
		sigaction(SIGUSR2, &old_sigusr2, NULL);
		sigaction(SIGCHLD, &old_sigchld, NULL);
	}
#endif

	/* Write our PID to the PID file. */
	write_pidfile();

	/* Create me */
	Me = new Server(NULL, Config->ServerName, 0, Config->ServerDesc, Config->Numeric);
	for (botinfo_map::const_iterator it = BotListByNick->begin(), it_end = BotListByNick->end(); it != it_end; ++it)
	{
		it->second->server = Me;
		++Me->users;
	}

	/* Announce ourselves to the logfile. */
	Log() << "Anope " << Anope::Version() << " starting up" << (Anope::Debug || Anope::ReadOnly ? " (options:" : "") << (Anope::Debug ? " debug" : "") << (Anope::ReadOnly ? " readonly" : "") << (Anope::Debug || Anope::ReadOnly ? ")" : "");

	InitSignals();

	/* Initialize multi-language support */
	Language::InitLanguages();

	/* Initialize random number generator */
	srand(Config->Seed);

	/* load modules */
	Log() << "Loading modules...";
	for (std::list<Anope::string>::iterator it = Config->ModulesAutoLoad.begin(), it_end = Config->ModulesAutoLoad.end(); it != it_end; ++it)
		ModuleManager::LoadModule(*it, NULL);

	Module *protocol = ModuleManager::FindFirstOf(PROTOCOL);
	if (protocol == NULL)
		throw CoreException("You must load a protocol module!");

	Log() << "Using IRCd protocol " << protocol->name;

	/* Load up databases */
	Log() << "Loading databases...";
	EventReturn MOD_RESULT;
	FOREACH_RESULT(I_OnLoadDatabase, OnLoadDatabase());
	Log() << "Databases loaded";
}

/*************************************************************************/
