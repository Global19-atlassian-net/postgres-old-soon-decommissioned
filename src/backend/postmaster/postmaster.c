/*-------------------------------------------------------------------------
 *
 * postmaster.c
 *	  This program acts as a clearing house for requests to the
 *	  POSTGRES system.	Frontend programs send a startup message
 *	  to the Postmaster and the postmaster uses the info in the
 *	  message to setup a backend process.
 *
 *	  The postmaster also manages system-wide operations such as
 *	  startup and shutdown.	The postmaster itself doesn't do those
 *	  operations, mind you --- it just forks off a subprocess to do them
 *	  at the right times.  It also takes care of resetting the system
 *	  if a backend crashes.
 *
 *	  The postmaster process creates the shared memory and semaphore
 *	  pools during startup, but as a rule does not touch them itself.
 *	  In particular, it is not a member of the PGPROC array of backends
 *	  and so it cannot participate in lock-manager operations.	Keeping
 *	  the postmaster away from shared memory operations makes it simpler
 *	  and more reliable.  The postmaster is almost always able to recover
 *	  from crashes of individual backends by resetting shared memory;
 *	  if it did much with shared memory then it would be prone to crashing
 *	  along with the backends.
 *
 *	  When a request message is received, we now fork() immediately.
 *	  The child process performs authentication of the request, and
 *	  then becomes a backend if successful.  This allows the auth code
 *	  to be written in a simple single-threaded style (as opposed to the
 *	  crufty "poor man's multitasking" code that used to be needed).
 *	  More importantly, it ensures that blockages in non-multithreaded
 *	  libraries like SSL or PAM cannot cause denial of service to other
 *	  clients.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL$
 *
 * NOTES
 *
 * Initialization:
 *		The Postmaster sets up shared memory data structures
 *		for the backends.
 *
 * Synchronization:
 *		The Postmaster shares memory with the backends but should avoid
 *		touching shared memory, so as not to become stuck if a crashing
 *		backend screws up locks or shared memory.  Likewise, the Postmaster
 *		should never block on messages from frontend clients.
 *
 * Garbage Collection:
 *		The Postmaster cleans up after backends if they have an emergency
 *		exit and/or core dump.
 *
 * Error Reporting:
 *		Use write_stderr() only for reporting "interactive" errors
 *		(essentially, bogus arguments on the command line).  Once the
 *		postmaster is launched, use ereport().  In particular, don't use
 *		write_stderr() for anything that occurs after pmdaemonize.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/wait.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <sys/param.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <limits.h>

#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#ifdef HAVE_GETOPT_H
#include <getopt.h>
#endif

#ifdef USE_RENDEZVOUS
#include <DNSServiceDiscovery/DNSServiceDiscovery.h>
#endif

#include "catalog/pg_database.h"
#include "commands/async.h"
#include "lib/dllist.h"
#include "libpq/auth.h"
#include "libpq/crypt.h"
#include "libpq/libpq.h"
#include "libpq/pqcomm.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "nodes/nodes.h"
#include "postmaster/postmaster.h"
#include "postmaster/pgarch.h"
#include "postmaster/syslogger.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/pg_shmem.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/bufmgr.h"
#include "access/xlog.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "bootstrap/bootstrap.h"
#include "pgstat.h"


/*
 * List of active backends (or child processes anyway; we don't actually
 * know whether a given child has become a backend or is still in the
 * authorization phase).  This is used mainly to keep track of how many
 * children we have and send them appropriate signals when necessary.
 *
 * "Special" children such as the startup and bgwriter tasks are not in
 * this list.
 */
typedef struct bkend
{
	pid_t		pid;			/* process id of backend */
	long		cancel_key;		/* cancel key for cancels for this backend */
} Backend;

static Dllist *BackendList;

#ifdef EXEC_BACKEND
#define NUM_BACKENDARRAY_ELEMS (2*MaxBackends)
static Backend *ShmemBackendArray;
#endif

/* The socket number we are listening for connections on */
int			PostPortNumber;
char	   *UnixSocketDir;
char	   *ListenAddresses;

/*
 * ReservedBackends is the number of backends reserved for superuser use.
 * This number is taken out of the pool size given by MaxBackends so
 * number of backend slots available to non-superusers is
 * (MaxBackends - ReservedBackends).  Note what this really means is
 * "if there are <= ReservedBackends connections available, only superusers
 * can make new connections" --- pre-existing superuser connections don't
 * count against the limit.
 */
int			ReservedBackends;


static const char *progname = NULL;

/* The socket(s) we're listening to. */
#define MAXLISTEN	10
static int	ListenSocket[MAXLISTEN];

/*
 * Set by the -o option
 */
static char ExtraOptions[MAXPGPATH];

/*
 * These globals control the behavior of the postmaster in case some
 * backend dumps core.	Normally, it kills all peers of the dead backend
 * and reinitializes shared memory.  By specifying -s or -n, we can have
 * the postmaster stop (rather than kill) peers and not reinitialize
 * shared data structures.
 */
static bool Reinit = true;
static int	SendStop = false;

/* still more option variables */
bool		EnableSSL = false;
bool		SilentMode = false; /* silent mode (-S) */

int			PreAuthDelay = 0;
int			AuthenticationTimeout = 60;

bool		log_hostname;		/* for ps display and logging */
bool		Log_connections = false;
bool		Db_user_namespace = false;

char	   *rendezvous_name;

/* list of library:init-function to be preloaded */
char	   *preload_libraries_string = NULL;

/* PIDs of special child processes; 0 when not running */
static pid_t StartupPID = 0,
			BgWriterPID = 0,
			PgArchPID = 0,
			PgStatPID = 0,
			SysLoggerPID = 0;

/* Startup/shutdown state */
#define			NoShutdown		0
#define			SmartShutdown	1
#define			FastShutdown	2

static int	Shutdown = NoShutdown;

static bool FatalError = false; /* T if recovering from backend crash */

bool		ClientAuthInProgress = false;		/* T during new-client
												 * authentication */

/*
 * State for assigning random salts and cancel keys.
 * Also, the global MyCancelKey passes the cancel key assigned to a given
 * backend from the postmaster to that backend (via fork).
 */
static unsigned int random_seed = 0;

static int	debug_flag = 0;

extern char *optarg;
extern int	optind,
			opterr;

#ifdef HAVE_INT_OPTRESET
extern int	optreset;
#endif

/*
 * postmaster.c - function prototypes
 */
static void checkDataDir(const char *checkdir);
static bool onlyConfigSpecified(const char *checkdir);
#ifdef USE_RENDEZVOUS
static void reg_reply(DNSServiceRegistrationReplyErrorType errorCode,
					  void *context);
#endif
static void pmdaemonize(void);
static Port *ConnCreate(int serverFd);
static void ConnFree(Port *port);
static void reset_shared(unsigned short port);
static void SIGHUP_handler(SIGNAL_ARGS);
static void pmdie(SIGNAL_ARGS);
static void reaper(SIGNAL_ARGS);
static void sigusr1_handler(SIGNAL_ARGS);
static void dummy_handler(SIGNAL_ARGS);
static void CleanupBackend(int pid, int exitstatus);
static void HandleChildCrash(int pid, int exitstatus, const char *procname);
static void LogChildExit(int lev, const char *procname,
			 int pid, int exitstatus);
static int	BackendRun(Port *port);
static void ExitPostmaster(int status);
static void usage(const char *);
static int	ServerLoop(void);
static int	BackendStartup(Port *port);
static int	ProcessStartupPacket(Port *port, bool SSLdone);
static void processCancelRequest(Port *port, void *pkt);
static int	initMasks(fd_set *rmask);
static void report_fork_failure_to_client(Port *port, int errnum);
static enum CAC_state canAcceptConnections(void);
static long PostmasterRandom(void);
static void RandomSalt(char *cryptSalt, char *md5Salt);
static void SignalChildren(int signal);
static int	CountChildren(void);
static bool CreateOptsFile(int argc, char *argv[], char *fullprogname);
static pid_t StartChildProcess(int xlop);

#ifdef EXEC_BACKEND

#ifdef WIN32
static pid_t win32_forkexec(const char *path, char *argv[]);
static void win32_AddChild(pid_t pid, HANDLE handle);
static void win32_RemoveChild(pid_t pid);
static pid_t win32_waitpid(int *exitstatus);
static DWORD WINAPI win32_sigchld_waiter(LPVOID param);

static pid_t *win32_childPIDArray;
static HANDLE *win32_childHNDArray;
static unsigned long win32_numChildren = 0;

HANDLE PostmasterHandle;
#endif

static pid_t backend_forkexec(Port *port);
static pid_t internal_forkexec(int argc, char *argv[], Port *port);

static void read_backend_variables(char *filename, Port *port);
static bool write_backend_variables(char *filename, Port *port);

static void ShmemBackendArrayAdd(Backend *bn);
static void ShmemBackendArrayRemove(pid_t pid);

#endif /* EXEC_BACKEND */

#define StartupDataBase()		StartChildProcess(BS_XLOG_STARTUP)
#define StartBackgroundWriter() StartChildProcess(BS_XLOG_BGWRITER)


/*
 * Postmaster main entry point
 */
int
PostmasterMain(int argc, char *argv[])
{
	int			opt;
	int			status;
	char	   *userPGDATA = NULL;
	int			i;

	progname = get_progname(argv[0]);

	MyProcPid = PostmasterPid = getpid();

	IsPostmasterEnvironment = true;

	/*
	 * Catch standard options before doing much else.  This even works on
	 * systems without getopt_long.
	 */
	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			usage(progname);
			ExitPostmaster(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("postmaster (PostgreSQL) " PG_VERSION);
			ExitPostmaster(0);
		}
	}

	/*
	 * for security, no dir or file created can be group or other
	 * accessible
	 */
	umask((mode_t) 0077);

	/*
	 * Fire up essential subsystems: memory management
	 */
	MemoryContextInit();

	/*
	 * By default, palloc() requests in the postmaster will be allocated
	 * in the PostmasterContext, which is space that can be recycled by
	 * backends.  Allocated data that needs to be available to backends
	 * should be allocated in TopMemoryContext.
	 */
	PostmasterContext = AllocSetContextCreate(TopMemoryContext,
											  "Postmaster",
											  ALLOCSET_DEFAULT_MINSIZE,
											  ALLOCSET_DEFAULT_INITSIZE,
											  ALLOCSET_DEFAULT_MAXSIZE);
	MemoryContextSwitchTo(PostmasterContext);

	IgnoreSystemIndexes(false);

	if (find_my_exec(argv[0], my_exec_path) < 0)
		elog(FATAL, "%s: could not locate my own executable path",
			 argv[0]);

	get_pkglib_path(my_exec_path, pkglib_path);

	/*
	 * Options setup
	 */
	InitializeGUCOptions();

	userPGDATA = getenv("PGDATA");		/* default value */
	
	opterr = 1;

	while ((opt = getopt(argc, argv, "A:a:B:b:c:D:d:Fh:ik:lm:MN:no:p:Ss-:")) != -1)
	{
		switch (opt)
		{
			case 'A':
#ifdef USE_ASSERT_CHECKING
				SetConfigOption("debug_assertions", optarg, PGC_POSTMASTER, PGC_S_ARGV);
#else
				write_stderr("%s: assert checking is not compiled in\n", progname);
#endif
				break;
			case 'a':
				/* Can no longer set authentication method. */
				break;
			case 'B':
				SetConfigOption("shared_buffers", optarg, PGC_POSTMASTER, PGC_S_ARGV);
				break;
			case 'b':
				/* Can no longer set the backend executable file to use. */
				break;
			case 'D':
				userPGDATA = optarg;
				break;
			case 'd':
				{
					/* Turn on debugging for the postmaster. */
					char	   *debugstr = palloc(strlen("debug") + strlen(optarg) + 1);

					sprintf(debugstr, "debug%s", optarg);
					SetConfigOption("log_min_messages", debugstr,
									PGC_POSTMASTER, PGC_S_ARGV);
					pfree(debugstr);
					debug_flag = atoi(optarg);
					break;
				}
			case 'F':
				SetConfigOption("fsync", "false", PGC_POSTMASTER, PGC_S_ARGV);
				break;
			case 'h':
				SetConfigOption("listen_addresses", optarg, PGC_POSTMASTER, PGC_S_ARGV);
				break;
			case 'i':
				SetConfigOption("listen_addresses", "*", PGC_POSTMASTER, PGC_S_ARGV);
				break;
			case 'k':
				SetConfigOption("unix_socket_directory", optarg, PGC_POSTMASTER, PGC_S_ARGV);
				break;
#ifdef USE_SSL
			case 'l':
				SetConfigOption("ssl", "true", PGC_POSTMASTER, PGC_S_ARGV);
				break;
#endif
			case 'm':
				/* Multiplexed backends no longer supported. */
				break;
			case 'M':

				/*
				 * ignore this flag.  This may be passed in because the
				 * program was run as 'postgres -M' instead of
				 * 'postmaster'
				 */
				break;
			case 'N':
				/* The max number of backends to start. */
				SetConfigOption("max_connections", optarg, PGC_POSTMASTER, PGC_S_ARGV);
				break;
			case 'n':
				/* Don't reinit shared mem after abnormal exit */
				Reinit = false;
				break;
			case 'o':

				/*
				 * Other options to pass to the backend on the command line
				 */
				snprintf(ExtraOptions + strlen(ExtraOptions),
						 sizeof(ExtraOptions) - strlen(ExtraOptions),
						 " %s", optarg);
				break;
			case 'p':
				SetConfigOption("port", optarg, PGC_POSTMASTER, PGC_S_ARGV);
				break;
			case 'S':

				/*
				 * Start in 'S'ilent mode (disassociate from controlling
				 * tty). You may also think of this as 'S'ysV mode since
				 * it's most badly needed on SysV-derived systems like
				 * SVR4 and HP-UX.
				 */
				SetConfigOption("silent_mode", "true", PGC_POSTMASTER, PGC_S_ARGV);
				break;
			case 's':

				/*
				 * In the event that some backend dumps core, send
				 * SIGSTOP, rather than SIGQUIT, to all its peers.	This
				 * lets the wily post_hacker collect core dumps from
				 * everyone.
				 */
				SendStop = true;
				break;
			case 'c':
			case '-':
				{
					char	   *name,
							   *value;

					ParseLongOption(optarg, &name, &value);
					if (!value)
					{
						if (opt == '-')
							ereport(ERROR,
									(errcode(ERRCODE_SYNTAX_ERROR),
									 errmsg("--%s requires a value",
											optarg)));
						else
							ereport(ERROR,
									(errcode(ERRCODE_SYNTAX_ERROR),
									 errmsg("-c %s requires a value",
											optarg)));
					}

					SetConfigOption(name, value, PGC_POSTMASTER, PGC_S_ARGV);
					free(name);
					if (value)
						free(value);
					break;
				}

			default:
				write_stderr("Try \"%s --help\" for more information.\n",
							 progname);
				ExitPostmaster(1);
		}
	}

	/*
	 * Postmaster accepts no non-option switch arguments.
	 */
	if (optind < argc)
	{
		write_stderr("%s: invalid argument: \"%s\"\n",
					 progname, argv[optind]);
		write_stderr("Try \"%s --help\" for more information.\n",
					 progname);
		ExitPostmaster(1);
	}

	if (userPGDATA)
	{
		userPGDATA = strdup(userPGDATA);
		canonicalize_path(userPGDATA);
	}

	if (onlyConfigSpecified(userPGDATA))
	{
		/*
		 *	It is either a file name or a directory with no
		 *	global/pg_control file, and hence not a data directory.
		 */
		user_pgconfig = userPGDATA;
		ProcessConfigFile(PGC_POSTMASTER);

		if (!guc_pgdata)	/* Got a pgdata from the config file? */
		{
			write_stderr("%s does not know where to find the database system data.\n"
						 "This should be specified as \"pgdata\" in %s%s.\n",
						 progname, userPGDATA,
						 user_pgconfig_is_dir ? "/postgresql.conf" : "");
			ExitPostmaster(2);
		}
		checkDataDir(guc_pgdata);
		SetDataDir(guc_pgdata);
	}
	else
	{
		/* Now we can set the data directory, and then read postgresql.conf. */
		checkDataDir(userPGDATA);
		SetDataDir(userPGDATA);
		ProcessConfigFile(PGC_POSTMASTER);
	}

	if (external_pidfile)
	{
		FILE *fpidfile = fopen(external_pidfile, "w");

		if (fpidfile)
		{
			fprintf(fpidfile, "%d\n", MyProcPid);
			fclose(fpidfile);
			/* Should we remove the pid file on postmaster exit? */
		}
		else
			fprintf(stderr,
				gettext("%s could not write to external pid file %s\n"),
				progname, external_pidfile);
	}

	/* If timezone is not set, determine what the OS uses */
	pg_timezone_initialize();

#ifdef EXEC_BACKEND
	write_nondefault_variables(PGC_POSTMASTER);
#endif

	/*
	 * Check for invalid combinations of GUC settings.
	 */
	if (NBuffers < 2 * MaxBackends || NBuffers < 16)
	{
		/*
		 * Do not accept -B so small that backends are likely to starve
		 * for lack of buffers.  The specific choices here are somewhat
		 * arbitrary.
		 */
		write_stderr("%s: the number of buffers (-B) must be at least twice the number of allowed connections (-N) and at least 16\n", progname);
		ExitPostmaster(1);
	}

	if (ReservedBackends >= MaxBackends)
	{
		write_stderr("%s: superuser_reserved_connections must be less than max_connections\n", progname);
		ExitPostmaster(1);
	}

	/*
	 * Other one-time internal sanity checks can go here.
	 */
	if (!CheckDateTokenTables())
	{
		write_stderr("%s: invalid datetoken tables, please fix\n", progname);
		ExitPostmaster(1);
	}

	/*
	 * Now that we are done processing the postmaster arguments, reset
	 * getopt(3) library so that it will work correctly in subprocesses.
	 */
	optind = 1;
#ifdef HAVE_INT_OPTRESET
	optreset = 1;				/* some systems need this too */
#endif

	/* For debugging: display postmaster environment */
	{
		extern char **environ;
		char	  **p;

		ereport(DEBUG3,
			(errmsg_internal("%s: PostmasterMain: initial environ dump:",
							 progname)));
		ereport(DEBUG3,
		 (errmsg_internal("-----------------------------------------")));
		for (p = environ; *p; ++p)
			ereport(DEBUG3,
					(errmsg_internal("\t%s", *p)));
		ereport(DEBUG3,
		 (errmsg_internal("-----------------------------------------")));
	}

#ifdef EXEC_BACKEND
	if (find_other_exec(argv[0], "postgres", PG_VERSIONSTR,
						postgres_exec_path) < 0)
		ereport(FATAL,
				(errmsg("%s: could not locate matching postgres executable",
						progname)));
#endif

	/*
	 * Initialize SSL library, if specified.
	 */
#ifdef USE_SSL
	if (EnableSSL)
		secure_initialize();
#endif

	/*
	 * process any libraries that should be preloaded and optionally
	 * pre-initialized
	 */
	if (preload_libraries_string)
		process_preload_libraries(preload_libraries_string);

	/*
	 * Fork away from controlling terminal, if -S specified.
	 *
	 * Must do this before we grab any interlock files, else the interlocks
	 * will show the wrong PID.
	 */
	if (SilentMode)
		pmdaemonize();

	/*
	 * Create lockfile for data directory.
	 *
	 * We want to do this before we try to grab the input sockets, because
	 * the data directory interlock is more reliable than the socket-file
	 * interlock (thanks to whoever decided to put socket files in /tmp
	 * :-(). For the same reason, it's best to grab the TCP socket(s) before
	 * the Unix socket.
	 */
	CreateDataDirLockFile(DataDir, true);

	/*
	 * Remove old temporary files.	At this point there can be no other
	 * Postgres processes running in this directory, so this should be
	 * safe.
	 */
	RemovePgTempFiles();

	/*
	 * Establish input sockets.
	 */
	for (i = 0; i < MAXLISTEN; i++)
		ListenSocket[i] = -1;

	if (ListenAddresses)
	{
		char	   *curhost,
				   *endptr;
		char		c;

		curhost = ListenAddresses;
		for (;;)
		{
			/* ignore whitespace */
			while (isspace((unsigned char) *curhost))
				curhost++;
			if (*curhost == '\0')
				break;
			endptr = curhost;
			while (*endptr != '\0' && !isspace((unsigned char) *endptr))
				endptr++;
			c = *endptr;
			*endptr = '\0';
			if (strcmp(curhost, "*") == 0)
				status = StreamServerPort(AF_UNSPEC, NULL,
										  (unsigned short) PostPortNumber,
										  UnixSocketDir,
										  ListenSocket, MAXLISTEN);
			else
				status = StreamServerPort(AF_UNSPEC, curhost,
										  (unsigned short) PostPortNumber,
										  UnixSocketDir,
										  ListenSocket, MAXLISTEN);
			if (status != STATUS_OK)
				ereport(WARNING,
					 (errmsg("could not create listen socket for \"%s\"",
							 curhost)));
			*endptr = c;
			if (c != '\0')
				curhost = endptr + 1;
			else
				break;
		}
	}

#ifdef USE_RENDEZVOUS
	/* Register for Rendezvous only if we opened TCP socket(s) */
	if (ListenSocket[0] != -1 && rendezvous_name != NULL)
	{
		DNSServiceRegistrationCreate(rendezvous_name,
									 "_postgresql._tcp.",
									 "",
									 htonl(PostPortNumber),
									 "",
								 (DNSServiceRegistrationReply) reg_reply,
									 NULL);
	}
#endif

#ifdef HAVE_UNIX_SOCKETS
	status = StreamServerPort(AF_UNIX, NULL,
							  (unsigned short) PostPortNumber,
							  UnixSocketDir,
							  ListenSocket, MAXLISTEN);
	if (status != STATUS_OK)
		ereport(WARNING,
				(errmsg("could not create Unix-domain socket")));
#endif

	/*
	 * check that we have some socket to listen on
	 */
	if (ListenSocket[0] == -1)
		ereport(FATAL,
				(errmsg("no socket created for listening")));

	XLOGPathInit();

	/*
	 * Set up shared memory and semaphores.
	 */
	reset_shared(PostPortNumber);

	/*
	 * Estimate number of openable files.  This must happen after setting
	 * up semaphores, because on some platforms semaphores count as open
	 * files.
	 */
	set_max_safe_fds();

	/*
	 * Initialize the list of active backends.
	 */
	BackendList = DLNewList();

#ifdef WIN32
	/*
	 * Initialize the child pid/HANDLE arrays for signal handling.
	 */
	win32_childPIDArray = (pid_t *)
		malloc(NUM_BACKENDARRAY_ELEMS * sizeof(pid_t));
	win32_childHNDArray = (HANDLE *)
		malloc(NUM_BACKENDARRAY_ELEMS * sizeof(HANDLE));
	if (!win32_childPIDArray || !win32_childHNDArray)
		ereport(FATAL,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));

	/*
	 * Set up a handle that child processes can use to check whether the
	 * postmaster is still running.
	 */
	if (DuplicateHandle(GetCurrentProcess(),
						GetCurrentProcess(),
						GetCurrentProcess(),
						&PostmasterHandle,
						0,
						TRUE,
						DUPLICATE_SAME_ACCESS) == 0)
		ereport(FATAL,
				(errmsg_internal("could not duplicate postmaster handle: %d",
								 (int) GetLastError())));
#endif

	/*
	 * Record postmaster options.  We delay this till now to avoid
	 * recording bogus options (eg, NBuffers too high for available
	 * memory).
	 */
	if (!CreateOptsFile(argc, argv, my_exec_path))
		ExitPostmaster(1);

	/*
	 * Set up signal handlers for the postmaster process.
	 *
	 * CAUTION: when changing this list, check for side-effects on the signal
	 * handling setup of child processes.  See tcop/postgres.c,
	 * bootstrap/bootstrap.c, postmaster/bgwriter.c, postmaster/pgarch.c,
	 * postmaster/pgstat.c, and postmaster/syslogger.c.
	 */
	pqinitmask();
	PG_SETMASK(&BlockSig);

	pqsignal(SIGHUP, SIGHUP_handler);	/* reread config file and have
										 * children do same */
	pqsignal(SIGINT, pmdie);	/* send SIGTERM and shut down */
	pqsignal(SIGQUIT, pmdie);	/* send SIGQUIT and die */
	pqsignal(SIGTERM, pmdie);	/* wait for children and shut down */
	pqsignal(SIGALRM, SIG_IGN); /* ignored */
	pqsignal(SIGPIPE, SIG_IGN); /* ignored */
	pqsignal(SIGUSR1, sigusr1_handler); /* message from child process */
	pqsignal(SIGUSR2, dummy_handler);	/* unused, reserve for children */
	pqsignal(SIGCHLD, reaper);	/* handle child termination */
	pqsignal(SIGTTIN, SIG_IGN); /* ignored */
	pqsignal(SIGTTOU, SIG_IGN); /* ignored */
	/* ignore SIGXFSZ, so that ulimit violations work like disk full */
#ifdef SIGXFSZ
	pqsignal(SIGXFSZ, SIG_IGN); /* ignored */
#endif

	/*
	 * If enabled, start up syslogger collection subprocess
	 */
	SysLoggerPID = SysLogger_Start();

	/*
	 * Reset whereToSendOutput from Debug (its starting state) to None.
	 * This stops ereport from sending log messages to stderr unless
	 * Log_destination permits.  We don't do this until the postmaster
	 * is fully launched, since startup failures may as well be
	 * reported to stderr.
	 */
	whereToSendOutput = None;

	/*
	 * Initialize the statistics collector stuff
	 */
	pgstat_init();

	/*
	 * Load cached files for client authentication.
	 */
	load_hba();
	load_ident();
	load_user();
	load_group();

	/*
	 * We're ready to rock and roll...
	 */
	StartupPID = StartupDataBase();

#ifdef EXEC_BACKEND
	write_nondefault_variables(PGC_POSTMASTER);
#endif

	status = ServerLoop();

	/*
	 * ServerLoop probably shouldn't ever return, but if it does, close
	 * down.
	 */
	ExitPostmaster(status != STATUS_OK);

	return 0;					/* not reached */
}



static bool
onlyConfigSpecified(const char *checkdir)
{
	char	path[MAXPGPATH];
	struct stat stat_buf;

	if (checkdir == NULL)			/* checkDataDir handles this */
		return FALSE;

	if (stat(checkdir, &stat_buf) == -1)	/* ditto */
		return FALSE;

	if (S_ISREG(stat_buf.st_mode))		/* It's a regular file, so assume it's explict */
		return TRUE;
	else if (S_ISDIR(stat_buf.st_mode))	/* It's a directory, is it a config or system dir? */
	{
		snprintf(path, MAXPGPATH, "%s/global/pg_control", checkdir);
		/* If this is not found, it is a config-only directory */
		if (stat(path, &stat_buf) == -1)
			return TRUE;
	}
	return FALSE;
}


/*
 * Validate the proposed data directory
 */
static void
checkDataDir(const char *checkdir)
{
	char		path[MAXPGPATH];
	FILE	   *fp;
	struct stat stat_buf;

	if (checkdir == NULL)
	{
		write_stderr("%s does not know where to find the database system data.\n"
					 "You must specify the directory that contains the database system\n"
					 "either by specifying the -D invocation option or by setting the\n"
					 "PGDATA environment variable.\n",
					 progname);
		ExitPostmaster(2);
	}

	if (stat(checkdir, &stat_buf) == -1)
	{
		if (errno == ENOENT)
			ereport(FATAL,
					(errcode_for_file_access(),
					 errmsg("data directory \"%s\" does not exist",
							checkdir)));
		else
			ereport(FATAL,
					(errcode_for_file_access(),
			 errmsg("could not read permissions of directory \"%s\": %m",
					checkdir)));
	}

	/*
	 * Check if the directory has group or world access.  If so, reject.
	 *
	 * XXX temporarily suppress check when on Windows, because there may not
	 * be proper support for Unix-y file permissions.  Need to think of a
	 * reasonable check to apply on Windows.
	 */
#if !defined(__CYGWIN__) && !defined(WIN32)
	if (stat_buf.st_mode & (S_IRWXG | S_IRWXO))
		ereport(FATAL,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("data directory \"%s\" has group or world access",
						checkdir),
				 errdetail("Permissions should be u=rwx (0700).")));
#endif

	/* Look for PG_VERSION before looking for pg_control */
	ValidatePgVersion(checkdir);

	snprintf(path, sizeof(path), "%s/global/pg_control", checkdir);

	fp = AllocateFile(path, PG_BINARY_R);
	if (fp == NULL)
	{
		write_stderr("%s: could not find the database system\n"
					 "Expected to find it in the directory \"%s\",\n"
					 "but could not open file \"%s\": %s\n",
					 progname, checkdir, path, strerror(errno));
		ExitPostmaster(2);
	}
	FreeFile(fp);
}


#ifdef USE_RENDEZVOUS

/*
 * empty callback function for DNSServiceRegistrationCreate()
 */
static void
reg_reply(DNSServiceRegistrationReplyErrorType errorCode, void *context)
{

}

#endif /* USE_RENDEZVOUS */


/*
 * Fork away from the controlling terminal (-S option)
 */
static void
pmdaemonize(void)
{
#ifndef WIN32
	int			i;
	pid_t		pid;

#ifdef LINUX_PROFILE
	struct itimerval prof_itimer;
#endif

#ifdef LINUX_PROFILE
	/* see comments in BackendStartup */
	getitimer(ITIMER_PROF, &prof_itimer);
#endif

	pid = fork();
	if (pid == (pid_t) -1)
	{
		write_stderr("%s: could not fork background process: %s\n",
					 progname, strerror(errno));
		ExitPostmaster(1);
	}
	else if (pid)
	{							/* parent */
		/* Parent should just exit, without doing any atexit cleanup */
		_exit(0);
	}

#ifdef LINUX_PROFILE
	setitimer(ITIMER_PROF, &prof_itimer, NULL);
#endif

	MyProcPid = PostmasterPid = getpid();	/* reset PID vars to child */

/* GH: If there's no setsid(), we hopefully don't need silent mode.
 * Until there's a better solution.
 */
#ifdef HAVE_SETSID
	if (setsid() < 0)
	{
		write_stderr("%s: could not dissociate from controlling TTY: %s\n",
					 progname, strerror(errno));
		ExitPostmaster(1);
	}
#endif
	i = open(NULL_DEV, O_RDWR);
	dup2(i, 0);
	dup2(i, 1);
	dup2(i, 2);
	close(i);
#else  /* WIN32 */
	/* not supported */
	elog(FATAL, "SilentMode not supported under WIN32");
#endif /* WIN32 */
}


/*
 * Print out help message
 */
static void
usage(const char *progname)
{
	printf(gettext("%s is the PostgreSQL server.\n\n"), progname);
	printf(gettext("Usage:\n  %s [OPTION]...\n\n"), progname);
	printf(gettext("Options:\n"));
#ifdef USE_ASSERT_CHECKING
	printf(gettext("  -A 1|0          enable/disable run-time assert checking\n"));
#endif
	printf(gettext("  -B NBUFFERS     number of shared buffers\n"));
	printf(gettext("  -c NAME=VALUE   set run-time parameter\n"));
	printf(gettext("  -d 1-5          debugging level\n"));
	printf(gettext("  -D DATADIR      database directory\n"));
	printf(gettext("  -F              turn fsync off\n"));
	printf(gettext("  -h HOSTNAME     host name or IP address to listen on\n"));
	printf(gettext("  -i              enable TCP/IP connections\n"));
	printf(gettext("  -k DIRECTORY    Unix-domain socket location\n"));
#ifdef USE_SSL
	printf(gettext("  -l              enable SSL connections\n"));
#endif
	printf(gettext("  -N MAX-CONNECT  maximum number of allowed connections\n"));
	printf(gettext("  -o OPTIONS      pass \"OPTIONS\" to each server process\n"));
	printf(gettext("  -p PORT         port number to listen on\n"));
	printf(gettext("  -S              silent mode (start in background without logging output)\n"));
	printf(gettext("  --help          show this help, then exit\n"));
	printf(gettext("  --version       output version information, then exit\n"));

	printf(gettext("\nDeveloper options:\n"));
	printf(gettext("  -n              do not reinitialize shared memory after abnormal exit\n"));
	printf(gettext("  -s              send SIGSTOP to all backend servers if one dies\n"));

	printf(gettext("\nPlease read the documentation for the complete list of run-time\n"
				   "configuration settings and how to set them on the command line or in\n"
				   "the configuration file.\n\n"
				   "Report bugs to <pgsql-bugs@postgresql.org>.\n"));
}


/*
 * Main idle loop of postmaster
 */
static int
ServerLoop(void)
{
	fd_set		readmask;
	int			nSockets;
	time_t		now,
				last_touch_time;
	struct timeval earlier,
				later;
	struct timezone tz;

	gettimeofday(&earlier, &tz);
	last_touch_time = time(NULL);

	nSockets = initMasks(&readmask);

	for (;;)
	{
		Port	   *port;
		fd_set		rmask;
		struct timeval timeout;
		int			selres;
		int			i;

		/*
		 * Wait for something to happen.
		 *
		 * We wait at most one minute, to ensure that the other background
		 * tasks handled below get done even when no requests are arriving.
		 */
		memcpy((char *) &rmask, (char *) &readmask, sizeof(fd_set));

		timeout.tv_sec = 60;
		timeout.tv_usec = 0;

		PG_SETMASK(&UnBlockSig);

		selres = select(nSockets, &rmask, NULL, NULL, &timeout);

		/*
		 * Block all signals until we wait again.  (This makes it safe for
		 * our signal handlers to do nontrivial work.)
		 */
		PG_SETMASK(&BlockSig);

		if (selres < 0)
		{
			if (errno != EINTR && errno != EWOULDBLOCK)
			{
				ereport(LOG,
						(errcode_for_socket_access(),
						 errmsg("select() failed in postmaster: %m")));
				return STATUS_ERROR;
			}
		}

		/*
		 * New connection pending on any of our sockets? If so, fork a
		 * child process to deal with it.
		 */
		if (selres > 0)
		{
			/*
			 * Select a random seed at the time of first receiving a request.
			 */
			while (random_seed == 0)
			{
				gettimeofday(&later, &tz);

				/*
				 * We are not sure how much precision is in tv_usec, so we
				 * swap the nibbles of 'later' and XOR them with 'earlier'. On
				 * the off chance that the result is 0, we loop until it isn't.
				 */
				random_seed = earlier.tv_usec ^
					((later.tv_usec << 16) |
					 ((later.tv_usec >> 16) & 0xffff));
			}

			for (i = 0; i < MAXLISTEN; i++)
			{
				if (ListenSocket[i] == -1)
					break;
				if (FD_ISSET(ListenSocket[i], &rmask))
				{
					port = ConnCreate(ListenSocket[i]);
					if (port)
					{
						BackendStartup(port);

						/*
						 * We no longer need the open socket or port structure
						 * in this process
						 */
						StreamClose(port->sock);
						ConnFree(port);
					}
				}
			}
		}

		/* If we have lost the system logger, try to start a new one */
		if (SysLoggerPID == 0 && Redirect_stderr)
			SysLoggerPID = SysLogger_Start();

		/*
		 * If no background writer process is running, and we are not in
		 * a state that prevents it, start one.  It doesn't matter if this
		 * fails, we'll just try again later.
		 */
		if (BgWriterPID == 0 && StartupPID == 0 && !FatalError)
		{
			BgWriterPID = StartBackgroundWriter();
			/* If shutdown is pending, set it going */
			if (Shutdown > NoShutdown && BgWriterPID != 0)
				kill(BgWriterPID, SIGUSR2);
		}

		/* If we have lost the archiver, try to start a new one */
		if (XLogArchivingActive() && PgArchPID == 0 && 
            StartupPID == 0 && !FatalError && Shutdown == NoShutdown)
			PgArchPID = pgarch_start();
 
		/* If we have lost the stats collector, try to start a new one */
		if (PgStatPID == 0 &&
			StartupPID == 0 && !FatalError && Shutdown == NoShutdown)
			PgStatPID = pgstat_start();

		/*
		 * Touch the socket and lock file at least every ten minutes, to ensure
		 * that they are not removed by overzealous /tmp-cleaning tasks.
		 */
		now = time(NULL);
		if (now - last_touch_time >= 10 * 60)
		{
			TouchSocketFile();
			TouchSocketLockFile();
			last_touch_time = now;
		}
	}
}


/*
 * Initialise the masks for select() for the ports we are listening on.
 * Return the number of sockets to listen on.
 */
static int
initMasks(fd_set *rmask)
{
	int			nsocks = -1;
	int			i;

	FD_ZERO(rmask);

	for (i = 0; i < MAXLISTEN; i++)
	{
		int			fd = ListenSocket[i];

		if (fd == -1)
			break;
		FD_SET(fd, rmask);
		if (fd > nsocks)
			nsocks = fd;
	}

	return nsocks + 1;
}


/*
 * Read the startup packet and do something according to it.
 *
 * Returns STATUS_OK or STATUS_ERROR, or might call ereport(FATAL) and
 * not return at all.
 *
 * (Note that ereport(FATAL) stuff is sent to the client, so only use it
 * if that's what you want.  Return STATUS_ERROR if you don't want to
 * send anything to the client, which would typically be appropriate
 * if we detect a communications failure.)
 */
static int
ProcessStartupPacket(Port *port, bool SSLdone)
{
	int32		len;
	void	   *buf;
	ProtocolVersion proto;
	MemoryContext oldcontext;

	if (pq_getbytes((char *) &len, 4) == EOF)
	{
		/*
		 * EOF after SSLdone probably means the client didn't like our
		 * response to NEGOTIATE_SSL_CODE.	That's not an error condition,
		 * so don't clutter the log with a complaint.
		 */
		if (!SSLdone)
			ereport(COMMERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("incomplete startup packet")));
		return STATUS_ERROR;
	}

	len = ntohl(len);
	len -= 4;

	if (len < (int32) sizeof(ProtocolVersion) ||
		len > MAX_STARTUP_PACKET_LENGTH)
	{
		ereport(COMMERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("invalid length of startup packet")));
		return STATUS_ERROR;
	}

	/*
	 * Allocate at least the size of an old-style startup packet, plus one
	 * extra byte, and make sure all are zeroes.  This ensures we will
	 * have null termination of all strings, in both fixed- and
	 * variable-length packet layouts.
	 */
	if (len <= (int32) sizeof(StartupPacket))
		buf = palloc0(sizeof(StartupPacket) + 1);
	else
		buf = palloc0(len + 1);

	if (pq_getbytes(buf, len) == EOF)
	{
		ereport(COMMERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("incomplete startup packet")));
		return STATUS_ERROR;
	}

	/*
	 * The first field is either a protocol version number or a special
	 * request code.
	 */
	port->proto = proto = ntohl(*((ProtocolVersion *) buf));

	if (proto == CANCEL_REQUEST_CODE)
	{
		processCancelRequest(port, buf);
		return 127;				/* XXX */
	}

	if (proto == NEGOTIATE_SSL_CODE && !SSLdone)
	{
		char		SSLok;

#ifdef USE_SSL
		/* No SSL when disabled or on Unix sockets */
		if (!EnableSSL || IS_AF_UNIX(port->laddr.addr.ss_family))
			SSLok = 'N';
		else
			SSLok = 'S';		/* Support for SSL */
#else
		SSLok = 'N';			/* No support for SSL */
#endif
		if (send(port->sock, &SSLok, 1, 0) != 1)
		{
			ereport(COMMERROR,
					(errcode_for_socket_access(),
				 errmsg("failed to send SSL negotiation response: %m")));
			return STATUS_ERROR;	/* close the connection */
		}

#ifdef USE_SSL
		if (SSLok == 'S' && secure_open_server(port) == -1)
			return STATUS_ERROR;
#endif
		/* regular startup packet, cancel, etc packet should follow... */
		/* but not another SSL negotiation request */
		return ProcessStartupPacket(port, true);
	}

	/* Could add additional special packet types here */

	/*
	 * Set FrontendProtocol now so that ereport() knows what format to
	 * send if we fail during startup.
	 */
	FrontendProtocol = proto;

	/* Check we can handle the protocol the frontend is using. */

	if (PG_PROTOCOL_MAJOR(proto) < PG_PROTOCOL_MAJOR(PG_PROTOCOL_EARLIEST) ||
	  PG_PROTOCOL_MAJOR(proto) > PG_PROTOCOL_MAJOR(PG_PROTOCOL_LATEST) ||
	(PG_PROTOCOL_MAJOR(proto) == PG_PROTOCOL_MAJOR(PG_PROTOCOL_LATEST) &&
	 PG_PROTOCOL_MINOR(proto) > PG_PROTOCOL_MINOR(PG_PROTOCOL_LATEST)))
		ereport(FATAL,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("unsupported frontend protocol %u.%u: server supports %u.0 to %u.%u",
					  PG_PROTOCOL_MAJOR(proto), PG_PROTOCOL_MINOR(proto),
						PG_PROTOCOL_MAJOR(PG_PROTOCOL_EARLIEST),
						PG_PROTOCOL_MAJOR(PG_PROTOCOL_LATEST),
						PG_PROTOCOL_MINOR(PG_PROTOCOL_LATEST))));

	/*
	 * Now fetch parameters out of startup packet and save them into the
	 * Port structure.	All data structures attached to the Port struct
	 * must be allocated in TopMemoryContext so that they won't disappear
	 * when we pass them to PostgresMain (see BackendRun).	We need not
	 * worry about leaking this storage on failure, since we aren't in the
	 * postmaster process anymore.
	 */
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);

	if (PG_PROTOCOL_MAJOR(proto) >= 3)
	{
		int32		offset = sizeof(ProtocolVersion);

		/*
		 * Scan packet body for name/option pairs.	We can assume any
		 * string beginning within the packet body is null-terminated,
		 * thanks to zeroing extra byte above.
		 */
		port->guc_options = NIL;

		while (offset < len)
		{
			char	   *nameptr = ((char *) buf) + offset;
			int32		valoffset;
			char	   *valptr;

			if (*nameptr == '\0')
				break;			/* found packet terminator */
			valoffset = offset + strlen(nameptr) + 1;
			if (valoffset >= len)
				break;			/* missing value, will complain below */
			valptr = ((char *) buf) + valoffset;

			if (strcmp(nameptr, "database") == 0)
				port->database_name = pstrdup(valptr);
			else if (strcmp(nameptr, "user") == 0)
				port->user_name = pstrdup(valptr);
			else if (strcmp(nameptr, "options") == 0)
				port->cmdline_options = pstrdup(valptr);
			else
			{
				/* Assume it's a generic GUC option */
				port->guc_options = lappend(port->guc_options,
											pstrdup(nameptr));
				port->guc_options = lappend(port->guc_options,
											pstrdup(valptr));
			}
			offset = valoffset + strlen(valptr) + 1;
		}

		/*
		 * If we didn't find a packet terminator exactly at the end of the
		 * given packet length, complain.
		 */
		if (offset != len - 1)
			ereport(FATAL,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("invalid startup packet layout: expected terminator as last byte")));
	}
	else
	{
		/*
		 * Get the parameters from the old-style, fixed-width-fields
		 * startup packet as C strings.  The packet destination was
		 * cleared first so a short packet has zeros silently added.  We
		 * have to be prepared to truncate the pstrdup result for oversize
		 * fields, though.
		 */
		StartupPacket *packet = (StartupPacket *) buf;

		port->database_name = pstrdup(packet->database);
		if (strlen(port->database_name) > sizeof(packet->database))
			port->database_name[sizeof(packet->database)] = '\0';
		port->user_name = pstrdup(packet->user);
		if (strlen(port->user_name) > sizeof(packet->user))
			port->user_name[sizeof(packet->user)] = '\0';
		port->cmdline_options = pstrdup(packet->options);
		if (strlen(port->cmdline_options) > sizeof(packet->options))
			port->cmdline_options[sizeof(packet->options)] = '\0';
		port->guc_options = NIL;
	}

	/* Check a user name was given. */
	if (port->user_name == NULL || port->user_name[0] == '\0')
		ereport(FATAL,
				(errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
		 errmsg("no PostgreSQL user name specified in startup packet")));

	/* The database defaults to the user name. */
	if (port->database_name == NULL || port->database_name[0] == '\0')
		port->database_name = pstrdup(port->user_name);

	if (Db_user_namespace)
	{
		/*
		 * If user@, it is a global user, remove '@'. We only want to do
		 * this if there is an '@' at the end and no earlier in the user
		 * string or they may fake as a local user of another database
		 * attaching to this database.
		 */
		if (strchr(port->user_name, '@') ==
			port->user_name + strlen(port->user_name) - 1)
			*strchr(port->user_name, '@') = '\0';
		else
		{
			/* Append '@' and dbname */
			char	   *db_user;

			db_user = palloc(strlen(port->user_name) +
							 strlen(port->database_name) + 2);
			sprintf(db_user, "%s@%s", port->user_name, port->database_name);
			port->user_name = db_user;
		}
	}

	/*
	 * Truncate given database and user names to length of a Postgres
	 * name.  This avoids lookup failures when overlength names are given.
	 */
	if (strlen(port->database_name) >= NAMEDATALEN)
		port->database_name[NAMEDATALEN - 1] = '\0';
	if (strlen(port->user_name) >= NAMEDATALEN)
		port->user_name[NAMEDATALEN - 1] = '\0';

	/*
	 * Done putting stuff in TopMemoryContext.
	 */
	MemoryContextSwitchTo(oldcontext);

	/*
	 * If we're going to reject the connection due to database state, say
	 * so now instead of wasting cycles on an authentication exchange.
	 * (This also allows a pg_ping utility to be written.)
	 */
	switch (port->canAcceptConnections)
	{
		case CAC_STARTUP:
			ereport(FATAL,
					(errcode(ERRCODE_CANNOT_CONNECT_NOW),
					 errmsg("the database system is starting up")));
			break;
		case CAC_SHUTDOWN:
			ereport(FATAL,
					(errcode(ERRCODE_CANNOT_CONNECT_NOW),
					 errmsg("the database system is shutting down")));
			break;
		case CAC_RECOVERY:
			ereport(FATAL,
					(errcode(ERRCODE_CANNOT_CONNECT_NOW),
					 errmsg("the database system is in recovery mode")));
			break;
		case CAC_TOOMANY:
			ereport(FATAL,
					(errcode(ERRCODE_TOO_MANY_CONNECTIONS),
					 errmsg("sorry, too many clients already")));
			break;
		case CAC_OK:
		default:
			break;
	}

	return STATUS_OK;
}


/*
 * The client has sent a cancel request packet, not a normal
 * start-a-new-connection packet.  Perform the necessary processing.
 * Nothing is sent back to the client.
 */
static void
processCancelRequest(Port *port, void *pkt)
{
	CancelRequestPacket *canc = (CancelRequestPacket *) pkt;
	int			backendPID;
	long		cancelAuthCode;
	Backend    *bp;
#ifndef EXEC_BACKEND
	Dlelem	   *curr;
#else
	int			i;
#endif

	backendPID = (int) ntohl(canc->backendPID);
	cancelAuthCode = (long) ntohl(canc->cancelAuthCode);

	/*
	 * See if we have a matching backend.  In the EXEC_BACKEND case, we
	 * can no longer access the postmaster's own backend list, and must
	 * rely on the duplicate array in shared memory.
	 */
#ifndef EXEC_BACKEND
	for (curr = DLGetHead(BackendList); curr; curr = DLGetSucc(curr))
	{
		bp = (Backend *) DLE_VAL(curr);
#else
	for (i = 0; i < NUM_BACKENDARRAY_ELEMS; i++)
	{
		bp = (Backend *) &ShmemBackendArray[i];
#endif
		if (bp->pid == backendPID)
		{
			if (bp->cancel_key == cancelAuthCode)
			{
				/* Found a match; signal that backend to cancel current op */
				ereport(DEBUG2,
						(errmsg_internal("processing cancel request: sending SIGINT to process %d",
										 backendPID)));
				kill(bp->pid, SIGINT);
			}
			else
				/* Right PID, wrong key: no way, Jose */
				ereport(DEBUG2,
						(errmsg_internal("bad key in cancel request for process %d",
										 backendPID)));
			return;
		}
	}

	/* No matching backend */
	ereport(DEBUG2,
			(errmsg_internal("bad pid in cancel request for process %d",
							 backendPID)));
}

/*
 * canAcceptConnections --- check to see if database state allows connections.
 */
static enum CAC_state
canAcceptConnections(void)
{
	/* Can't start backends when in startup/shutdown/recovery state. */
	if (Shutdown > NoShutdown)
		return CAC_SHUTDOWN;
	if (StartupPID)
		return CAC_STARTUP;
	if (FatalError)
		return CAC_RECOVERY;

	/*
	 * Don't start too many children.
	 *
	 * We allow more connections than we can have backends here because some
	 * might still be authenticating; they might fail auth, or some
	 * existing backend might exit before the auth cycle is completed. The
	 * exact MaxBackends limit is enforced when a new backend tries to
	 * join the shared-inval backend array.
	 */
	if (CountChildren() >= 2 * MaxBackends)
		return CAC_TOOMANY;

	return CAC_OK;
}


/*
 * ConnCreate -- create a local connection data structure
 */
static Port *
ConnCreate(int serverFd)
{
	Port	   *port;

	if (!(port = (Port *) calloc(1, sizeof(Port))))
	{
		ereport(LOG,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
		ExitPostmaster(1);
	}

	if (StreamConnection(serverFd, port) != STATUS_OK)
	{
		StreamClose(port->sock);
		ConnFree(port);
		port = NULL;
	}
	else
	{
		/*
		 * Precompute password salt values to use for this connection.
		 * It's slightly annoying to do this long in advance of knowing
		 * whether we'll need 'em or not, but we must do the random()
		 * calls before we fork, not after.  Else the postmaster's random
		 * sequence won't get advanced, and all backends would end up
		 * using the same salt...
		 */
		RandomSalt(port->cryptSalt, port->md5Salt);
	}

	return port;
}


/*
 * ConnFree -- free a local connection data structure
 */
static void
ConnFree(Port *conn)
{
#ifdef USE_SSL
	secure_close(conn);
#endif
	free(conn);
}


/*
 * ClosePostmasterPorts -- close all the postmaster's open sockets
 *
 * This is called during child process startup to release file descriptors
 * that are not needed by that child process.  The postmaster still has
 * them open, of course.
 *
 * Note: we pass am_syslogger as a boolean because we don't want to set
 * the global variable yet when this is called.
 */
void
ClosePostmasterPorts(bool am_syslogger)
{
	int			i;

	/* Close the listen sockets */
	for (i = 0; i < MAXLISTEN; i++)
	{
		if (ListenSocket[i] != -1)
		{
			StreamClose(ListenSocket[i]);
			ListenSocket[i] = -1;
		}
	}

	/* If using syslogger, close the read side of the pipe */
	if (!am_syslogger)
	{
#ifndef WIN32
		if (syslogPipe[0] >= 0)
			close(syslogPipe[0]);
		syslogPipe[0] = -1;
#else
		if (syslogPipe[0])
			CloseHandle(syslogPipe[0]);
		syslogPipe[0] = 0;
#endif
	}
}


/*
 * reset_shared -- reset shared memory and semaphores
 */
static void
reset_shared(unsigned short port)
{
	/*
	 * Create or re-create shared memory and semaphores.
	 *
	 * Note: in each "cycle of life" we will normally assign the same IPC
	 * keys (if using SysV shmem and/or semas), since the port number is
	 * used to determine IPC keys.	This helps ensure that we will clean
	 * up dead IPC objects if the postmaster crashes and is restarted.
	 */
	CreateSharedMemoryAndSemaphores(false, MaxBackends, port);
}


/*
 * SIGHUP -- reread config files, and tell children to do same
 */
static void
SIGHUP_handler(SIGNAL_ARGS)
{
	int			save_errno = errno;

	PG_SETMASK(&BlockSig);

	if (Shutdown <= SmartShutdown)
	{
		ereport(LOG,
			 (errmsg("received SIGHUP, reloading configuration files")));
		ProcessConfigFile(PGC_SIGHUP);
		SignalChildren(SIGHUP);
		if (BgWriterPID != 0)
			kill(BgWriterPID, SIGHUP);
		if (PgArchPID != 0)
			kill(PgArchPID, SIGHUP);
		if (SysLoggerPID != 0)
			kill(SysLoggerPID, SIGHUP);
		/* PgStatPID does not currently need SIGHUP */
		load_hba();
		load_ident();

#ifdef EXEC_BACKEND
		/* Update the starting-point file for future children */
		write_nondefault_variables(PGC_SIGHUP);
#endif
	}

	PG_SETMASK(&UnBlockSig);

	errno = save_errno;
}


/*
 * pmdie -- signal handler for processing various postmaster signals.
 */
static void
pmdie(SIGNAL_ARGS)
{
	int			save_errno = errno;

	PG_SETMASK(&BlockSig);

	ereport(DEBUG2,
			(errmsg_internal("postmaster received signal %d",
							 postgres_signal_arg)));

	switch (postgres_signal_arg)
	{
		case SIGTERM:
			/*
			 * Smart Shutdown:
			 *
			 * Wait for children to end their work, then shut down.
			 */
			if (Shutdown >= SmartShutdown)
				break;
			Shutdown = SmartShutdown;
			ereport(LOG,
					(errmsg("received smart shutdown request")));

			if (DLGetHead(BackendList))
				break;			/* let reaper() handle this */

			/*
			 * No children left. Begin shutdown of data base system.
			 */
			if (StartupPID != 0 || FatalError)
				break;			/* let reaper() handle this */
			/* Start the bgwriter if not running */
			if (BgWriterPID == 0)
				BgWriterPID = StartBackgroundWriter();
			/* And tell it to shut down */
			if (BgWriterPID != 0)
				kill(BgWriterPID, SIGUSR2);
			/* Tell pgarch to shut down too; nothing left for it to do */
			if (PgArchPID != 0)
				kill(PgArchPID, SIGQUIT);
			/* Tell pgstat to shut down too; nothing left for it to do */
			if (PgStatPID != 0)
				kill(PgStatPID, SIGQUIT);
			break;

		case SIGINT:
			/*
			 * Fast Shutdown:
			 *
			 * Abort all children with SIGTERM (rollback active transactions
			 * and exit) and shut down when they are gone.
			 */
			if (Shutdown >= FastShutdown)
				break;
			Shutdown = FastShutdown;
			ereport(LOG,
					(errmsg("received fast shutdown request")));

			if (DLGetHead(BackendList))
			{
				if (!FatalError)
				{
					ereport(LOG,
							(errmsg("aborting any active transactions")));
					SignalChildren(SIGTERM);
					/* reaper() does the rest */
				}
				break;
			}

			/*
			 * No children left. Begin shutdown of data base system.
			 *
			 * Note: if we previously got SIGTERM then we may send SIGUSR2
			 * to the bgwriter a second time here.  This should be harmless.
			 */
			if (StartupPID != 0 || FatalError)
				break;			/* let reaper() handle this */
			/* Start the bgwriter if not running */
			if (BgWriterPID == 0)
				BgWriterPID = StartBackgroundWriter();
			/* And tell it to shut down */
			if (BgWriterPID != 0)
				kill(BgWriterPID, SIGUSR2);
			/* Tell pgarch to shut down too; nothing left for it to do */
			if (PgArchPID != 0)
				kill(PgArchPID, SIGQUIT);
			/* Tell pgstat to shut down too; nothing left for it to do */
			if (PgStatPID != 0)
				kill(PgStatPID, SIGQUIT);
			break;

		case SIGQUIT:
			/*
			 * Immediate Shutdown:
			 *
			 * abort all children with SIGQUIT and exit without attempt to
			 * properly shut down data base system.
			 */
			ereport(LOG,
					(errmsg("received immediate shutdown request")));
			if (StartupPID != 0)
				kill(StartupPID, SIGQUIT);
			if (BgWriterPID != 0)
				kill(BgWriterPID, SIGQUIT);
			if (PgArchPID != 0)
				kill(PgArchPID, SIGQUIT);
			if (PgStatPID != 0)
				kill(PgStatPID, SIGQUIT);
			if (DLGetHead(BackendList))
				SignalChildren(SIGQUIT);
			ExitPostmaster(0);
			break;
	}

	PG_SETMASK(&UnBlockSig);

	errno = save_errno;
}

/*
 * Reaper -- signal handler to cleanup after a backend (child) dies.
 */
static void
reaper(SIGNAL_ARGS)
{
	int			save_errno = errno;

#ifdef HAVE_WAITPID
	int			status;			/* backend exit status */

#else
#ifndef WIN32
	union wait	status;			/* backend exit status */
#endif
#endif
	int			exitstatus;
	int			pid;			/* process id of dead backend */

	PG_SETMASK(&BlockSig);

	ereport(DEBUG4,
			(errmsg_internal("reaping dead processes")));
#ifdef HAVE_WAITPID
	while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
	{
		exitstatus = status;
#else
#ifndef WIN32
	while ((pid = wait3(&status, WNOHANG, NULL)) > 0)
	{
		exitstatus = status.w_status;
#else
	while ((pid = win32_waitpid(&exitstatus)) > 0)
	{
		/*
		 * We need to do this here, and not in CleanupBackend, since this is
		 * to be called on all children when we are done with them. Could
		 * move to LogChildExit, but that seems like asking for future
		 * trouble...
		 */
		win32_RemoveChild(pid);
#endif /* WIN32 */
#endif /* HAVE_WAITPID */

		/*
		 * Check if this child was a startup process.
		 */
		if (StartupPID != 0 && pid == StartupPID)
		{
			StartupPID = 0;
			if (exitstatus != 0)
			{
				LogChildExit(LOG, gettext("startup process"),
							 pid, exitstatus);
				ereport(LOG,
						(errmsg("aborting startup due to startup process failure")));
				ExitPostmaster(1);
			}

			/*
			 * Startup succeeded - we are done with system startup or recovery.
			 */
			FatalError = false;

			/*
			 * Crank up the background writer.  It doesn't matter if this
			 * fails, we'll just try again later.
			 */
			Assert(BgWriterPID == 0);
			BgWriterPID = StartBackgroundWriter();

			/*
			 * Go to shutdown mode if a shutdown request was pending.
			 * Otherwise, try to start the archiver and stats collector too.
			 */
			if (Shutdown > NoShutdown && BgWriterPID != 0)
				kill(BgWriterPID, SIGUSR2);
			else if (Shutdown == NoShutdown) {
                    if (XLogArchivingActive() && PgArchPID == 0)
        				PgArchPID = pgarch_start();
                    if (PgStatPID == 0)
        				PgStatPID = pgstat_start();
            }

			continue;
		}

		/*
		 * Was it the bgwriter?
		 */
		if (BgWriterPID != 0 && pid == BgWriterPID)
		{
			BgWriterPID = 0;
			if (exitstatus == 0 && Shutdown > NoShutdown &&
				!FatalError && !DLGetHead(BackendList))
			{
				/*
				 * Normal postmaster exit is here: we've seen normal
				 * exit of the bgwriter after it's been told to shut down.
				 * We expect that it wrote a shutdown checkpoint.  (If
				 * for some reason it didn't, recovery will occur on next
				 * postmaster start.)
				 *
				 * Note: we do not wait around for exit of the archiver or
				 * stats processes.  They've been sent SIGQUIT by this
				 * point, and in any case contain logic to commit hara-kiri
				 * if they notice the postmaster is gone.
				 */
				ExitPostmaster(0);
			}
			/*
			 * Any unexpected exit of the bgwriter is treated as a crash.
			 */
			HandleChildCrash(pid, exitstatus,
							 gettext("background writer process"));
			continue;
		}

		/*
		 * Was it the archiver?  If so, just try to start a new
		 * one; no need to force reset of the rest of the system.  (If fail,
		 * we'll try again in future cycles of the main loop.)
		 */
		if (PgArchPID != 0 && pid == PgArchPID)
		{
			PgArchPID = 0;
			if (exitstatus != 0)
				LogChildExit(LOG, gettext("archiver process"),
							 pid, exitstatus);
			if (XLogArchivingActive() &&
				StartupPID == 0 && !FatalError && Shutdown == NoShutdown)
				PgArchPID = pgarch_start();
			continue;
		}

		/*
		 * Was it the statistics collector?  If so, just try to start a new
		 * one; no need to force reset of the rest of the system.  (If fail,
		 * we'll try again in future cycles of the main loop.)
		 */
		if (PgStatPID != 0 && pid == PgStatPID)
		{
			PgStatPID = 0;
			if (exitstatus != 0)
				LogChildExit(LOG, gettext("statistics collector process"),
							 pid, exitstatus);
			if (StartupPID == 0 && !FatalError && Shutdown == NoShutdown)
				PgStatPID = pgstat_start();
			continue;
		}

		/* Was it the system logger? try to start a new one */
		if (SysLoggerPID != 0 && pid == SysLoggerPID)
		{
			SysLoggerPID = 0;
			/* for safety's sake, launch new logger *first* */
			SysLoggerPID = SysLogger_Start();
			if (exitstatus != 0)
				LogChildExit(LOG, gettext("system logger process"),
							 pid, exitstatus);
			continue;
		}

		/*
		 * Else do standard backend child cleanup.
		 */
		CleanupBackend(pid, exitstatus);
	}							/* loop over pending child-death reports */

	if (FatalError)
	{
		/*
		 * Wait for all important children to exit, then reset shmem and
		 * StartupDataBase.  (We can ignore the archiver and stats processes
		 * here since they are not connected to shmem.)
		 */
		if (DLGetHead(BackendList) || StartupPID != 0 || BgWriterPID != 0)
			goto reaper_done;
		ereport(LOG,
			(errmsg("all server processes terminated; reinitializing")));

		shmem_exit(0);
		reset_shared(PostPortNumber);

		StartupPID = StartupDataBase();

		goto reaper_done;
	}

	if (Shutdown > NoShutdown)
	{
		if (DLGetHead(BackendList) || StartupPID != 0)
			goto reaper_done;
		/* Start the bgwriter if not running */
		if (BgWriterPID == 0)
			BgWriterPID = StartBackgroundWriter();
		/* And tell it to shut down */
		if (BgWriterPID != 0)
			kill(BgWriterPID, SIGUSR2);
		/* Tell pgarch to shut down too; nothing left for it to do */
		if (PgArchPID != 0)
			kill(PgArchPID, SIGQUIT);
		/* Tell pgstat to shut down too; nothing left for it to do */
		if (PgStatPID != 0)
			kill(PgStatPID, SIGQUIT);
	}

reaper_done:
	PG_SETMASK(&UnBlockSig);

	errno = save_errno;
}


/*
 * CleanupBackend -- cleanup after terminated backend.
 *
 * Remove all local state associated with backend.
 */
static void
CleanupBackend(int pid,
			   int exitstatus)		/* child's exit status. */
{
	Dlelem	   *curr;

	LogChildExit(DEBUG2, gettext("server process"), pid, exitstatus);

	/*
	 * If a backend dies in an ugly way (i.e. exit status not 0) then we
	 * must signal all other backends to quickdie.	If exit status is zero
	 * we assume everything is hunky dory and simply remove the backend
	 * from the active backend list.
	 */
	if (exitstatus != 0)
	{
		HandleChildCrash(pid, exitstatus, gettext("server process"));
		return;
	}

	for (curr = DLGetHead(BackendList); curr; curr = DLGetSucc(curr))
	{
		Backend    *bp = (Backend *) DLE_VAL(curr);

		if (bp->pid == pid)
		{
			DLRemove(curr);
			free(bp);
			DLFreeElem(curr);
#ifdef EXEC_BACKEND
			ShmemBackendArrayRemove(pid);
#endif
			/* Tell the collector about backend termination */
			pgstat_beterm(pid);
			break;
		}
	}
}

/*
 * HandleChildCrash -- cleanup after failed backend or bgwriter.
 *
 * The objectives here are to clean up our local state about the child
 * process, and to signal all other remaining children to quickdie.
 */
static void
HandleChildCrash(int pid, int exitstatus, const char *procname)
{
	Dlelem	   *curr,
			   *next;
	Backend    *bp;

	/*
	 * Make log entry unless there was a previous crash (if so, nonzero
	 * exit status is to be expected in SIGQUIT response; don't clutter log)
	 */
	if (!FatalError)
	{
		LogChildExit(LOG, procname, pid, exitstatus);
		ereport(LOG,
				(errmsg("terminating any other active server processes")));
	}

	/* Process regular backends */
	for (curr = DLGetHead(BackendList); curr; curr = next)
	{
		next = DLGetSucc(curr);
		bp = (Backend *) DLE_VAL(curr);
		if (bp->pid == pid)
		{
			/*
			 * Found entry for freshly-dead backend, so remove it.
			 */
			DLRemove(curr);
			free(bp);
			DLFreeElem(curr);
#ifdef EXEC_BACKEND
			ShmemBackendArrayRemove(pid);
#endif
			/* Tell the collector about backend termination */
			pgstat_beterm(pid);
			/* Keep looping so we can signal remaining backends */
		}
		else
		{
			/*
			 * This backend is still alive.  Unless we did so already,
			 * tell it to commit hara-kiri.
			 *
			 * SIGQUIT is the special signal that says exit without proc_exit
			 * and let the user know what's going on. But if SendStop is
			 * set (-s on command line), then we send SIGSTOP instead, so
			 * that we can get core dumps from all backends by hand.
			 */
			if (!FatalError)
			{
				ereport(DEBUG2,
						(errmsg_internal("sending %s to process %d",
									  (SendStop ? "SIGSTOP" : "SIGQUIT"),
										 (int) bp->pid)));
				kill(bp->pid, (SendStop ? SIGSTOP : SIGQUIT));
			}
		}
	}

	/* Take care of the bgwriter too */
	if (pid == BgWriterPID)
		BgWriterPID = 0;
	else if (BgWriterPID != 0 && !FatalError)
	{
		ereport(DEBUG2,
				(errmsg_internal("sending %s to process %d",
								 (SendStop ? "SIGSTOP" : "SIGQUIT"),
								 (int) BgWriterPID)));
		kill(BgWriterPID, (SendStop ? SIGSTOP : SIGQUIT));
	}

	/* Force a power-cycle of the pgarch process too */
	/* (Shouldn't be necessary, but just for luck) */
	if (PgArchPID != 0 && !FatalError)
	{
		ereport(DEBUG2,
				(errmsg_internal("sending %s to process %d",
								 "SIGQUIT",
								 (int) PgArchPID)));
		kill(PgArchPID, SIGQUIT);
	}

	/* Force a power-cycle of the pgstat processes too */
	/* (Shouldn't be necessary, but just for luck) */
	if (PgStatPID != 0 && !FatalError)
	{
		ereport(DEBUG2,
				(errmsg_internal("sending %s to process %d",
								 "SIGQUIT",
								 (int) PgStatPID)));
		kill(PgStatPID, SIGQUIT);
	}

	/* We do NOT restart the syslogger */

	FatalError = true;
}

/*
 * Log the death of a child process.
 */
static void
LogChildExit(int lev, const char *procname, int pid, int exitstatus)
{
	if (WIFEXITED(exitstatus))
		ereport(lev,

		/*
		 * translator: %s is a noun phrase describing a child process,
		 * such as "server process"
		 */
				(errmsg("%s (PID %d) exited with exit code %d",
						procname, pid, WEXITSTATUS(exitstatus))));
	else if (WIFSIGNALED(exitstatus))
		ereport(lev,

		/*
		 * translator: %s is a noun phrase describing a child process,
		 * such as "server process"
		 */
				(errmsg("%s (PID %d) was terminated by signal %d",
						procname, pid, WTERMSIG(exitstatus))));
	else
		ereport(lev,

		/*
		 * translator: %s is a noun phrase describing a child process,
		 * such as "server process"
		 */
				(errmsg("%s (PID %d) exited with unexpected status %d",
						procname, pid, exitstatus)));
}

/*
 * Send a signal to all backend children (but NOT special children)
 */
static void
SignalChildren(int signal)
{
	Dlelem	   *curr;

	for (curr = DLGetHead(BackendList); curr; curr = DLGetSucc(curr))
	{
		Backend    *bp = (Backend *) DLE_VAL(curr);

		ereport(DEBUG4,
				(errmsg_internal("sending signal %d to process %d",
								 signal, (int) bp->pid)));
		kill(bp->pid, signal);
	}
}

/*
 * BackendStartup -- start backend process
 *
 * returns: STATUS_ERROR if the fork failed, STATUS_OK otherwise.
 */
static int
BackendStartup(Port *port)
{
	Backend    *bn;				/* for backend cleanup */
	pid_t		pid;

#ifdef LINUX_PROFILE
	struct itimerval prof_itimer;
#endif

	/*
	 * Compute the cancel key that will be assigned to this backend. The
	 * backend will have its own copy in the forked-off process' value of
	 * MyCancelKey, so that it can transmit the key to the frontend.
	 */
	MyCancelKey = PostmasterRandom();

	/*
	 * Make room for backend data structure.  Better before the fork() so
	 * we can handle failure cleanly.
	 */
	bn = (Backend *) malloc(sizeof(Backend));
	if (!bn)
	{
		ereport(LOG,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
		return STATUS_ERROR;
	}

	/* Pass down canAcceptConnections state (kluge for EXEC_BACKEND case) */
	port->canAcceptConnections = canAcceptConnections();

	/*
	 * Flush stdio channels just before fork, to avoid double-output
	 * problems. Ideally we'd use fflush(NULL) here, but there are still a
	 * few non-ANSI stdio libraries out there (like SunOS 4.1.x) that
	 * coredump if we do. Presently stdout and stderr are the only stdio
	 * output channels used by the postmaster, so fflush'ing them should
	 * be sufficient.
	 */
	fflush(stdout);
	fflush(stderr);

#ifdef EXEC_BACKEND

	pid = backend_forkexec(port);

#else /* !EXEC_BACKEND */

#ifdef LINUX_PROFILE

	/*
	 * Linux's fork() resets the profiling timer in the child process. If
	 * we want to profile child processes then we need to save and restore
	 * the timer setting.  This is a waste of time if not profiling,
	 * however, so only do it if commanded by specific -DLINUX_PROFILE
	 * switch.
	 */
	getitimer(ITIMER_PROF, &prof_itimer);
#endif

#ifdef __BEOS__
	/* Specific beos actions before backend startup */
	beos_before_backend_startup();
#endif

	pid = fork();

	if (pid == 0)				/* child */
	{
#ifdef LINUX_PROFILE
		setitimer(ITIMER_PROF, &prof_itimer, NULL);
#endif

#ifdef __BEOS__
		/* Specific beos backend startup actions */
		beos_backend_startup();
#endif
		free(bn);

		proc_exit(BackendRun(port));
	}

#endif /* EXEC_BACKEND */

	if (pid < 0)
	{
		/* in parent, fork failed */
		int			save_errno = errno;

#ifdef __BEOS__
		/* Specific beos backend startup actions */
		beos_backend_startup_failed();
#endif
		free(bn);
		errno = save_errno;
		ereport(LOG,
			  (errmsg("could not fork new process for connection: %m")));
		report_fork_failure_to_client(port, save_errno);
		return STATUS_ERROR;
	}

	/* in parent, successful fork */
	ereport(DEBUG2,
			(errmsg_internal("forked new backend, pid=%d socket=%d",
							 (int) pid, port->sock)));

	/*
	 * Everything's been successful, it's safe to add this backend to our
	 * list of backends.
	 */
	bn->pid = pid;
	bn->cancel_key = MyCancelKey;
	DLAddHead(BackendList, DLNewElem(bn));
#ifdef EXEC_BACKEND
	ShmemBackendArrayAdd(bn);
#endif

	return STATUS_OK;
}

/*
 * Try to report backend fork() failure to client before we close the
 * connection.	Since we do not care to risk blocking the postmaster on
 * this connection, we set the connection to non-blocking and try only once.
 *
 * This is grungy special-purpose code; we cannot use backend libpq since
 * it's not up and running.
 */
static void
report_fork_failure_to_client(Port *port, int errnum)
{
	char		buffer[1000];

	/* Format the error message packet (always V2 protocol) */
	snprintf(buffer, sizeof(buffer), "E%s%s\n",
			 gettext("could not fork new process for connection: "),
			 strerror(errnum));

	/* Set port to non-blocking.  Don't do send() if this fails */
	if (!set_noblock(port->sock))
		return;

	send(port->sock, buffer, strlen(buffer) + 1, 0);
}


/*
 * split_opts -- split a string of options and append it to an argv array
 *
 * NB: the string is destructively modified!
 *
 * Since no current POSTGRES arguments require any quoting characters,
 * we can use the simple-minded tactic of assuming each set of space-
 * delimited characters is a separate argv element.
 *
 * If you don't like that, well, we *used* to pass the whole option string
 * as ONE argument to execl(), which was even less intelligent...
 */
static void
split_opts(char **argv, int *argcp, char *s)
{
	while (s && *s)
	{
		while (isspace((unsigned char) *s))
			++s;
		if (*s == '\0')
			break;
		argv[(*argcp)++] = s;
		while (*s && !isspace((unsigned char) *s))
			++s;
		if (*s)
			*s++ = '\0';
	}
}


/*
 * BackendRun -- perform authentication, and if successful,
 *				set up the backend's argument list and invoke PostgresMain()
 *
 * returns:
 *		Shouldn't return at all.
 *		If PostgresMain() fails, return status.
 */
static int
BackendRun(Port *port)
{
	int			status;
	struct timeval now;
	struct timezone tz;
	char		remote_host[NI_MAXHOST];
	char		remote_port[NI_MAXSERV];
	char		remote_ps_data[NI_MAXHOST];
	char	  **av;
	int			maxac;
	int			ac;
	char		debugbuf[32];
	char		protobuf[32];
	int			i;

	IsUnderPostmaster = true;	/* we are a postmaster subprocess now */

	/*
	 * Let's clean up ourselves as the postmaster child, and close the
	 * postmaster's listen sockets
	 */
	ClosePostmasterPorts(false);

	/* We don't want the postmaster's proc_exit() handlers */
	on_exit_reset();

	/*
	 * Signal handlers setting is moved to tcop/postgres...
	 */

	/* Save port etc. for ps status */
	MyProcPort = port;

	/* Reset MyProcPid to new backend's pid */
	MyProcPid = getpid();

	/*
	 * PreAuthDelay is a debugging aid for investigating problems in the
	 * authentication cycle: it can be set in postgresql.conf to allow
	 * time to attach to the newly-forked backend with a debugger. (See
	 * also the -W backend switch, which we allow clients to pass through
	 * PGOPTIONS, but it is not honored until after authentication.)
	 */
	if (PreAuthDelay > 0)
		pg_usleep(PreAuthDelay * 1000000L);

	ClientAuthInProgress = true;	/* limit visibility of log messages */

	/* save start time for end of session reporting */
	gettimeofday(&(port->session_start), NULL);

	/* set these to empty in case they are needed before we set them up */
	port->remote_host = "";
	port->remote_port = "";
	port->commandTag = "";

	/*
	 * Initialize libpq and enable reporting of ereport errors to the
	 * client. Must do this now because authentication uses libpq to send
	 * messages.
	 */
	pq_init();					/* initialize libpq to talk to client */
	whereToSendOutput = Remote; /* now safe to ereport to client */

	/*
	 * We arrange for a simple exit(0) if we receive SIGTERM or SIGQUIT
	 * during any client authentication related communication. Otherwise
	 * the postmaster cannot shutdown the database FAST or IMMED cleanly
	 * if a buggy client blocks a backend during authentication.
	 */
	pqsignal(SIGTERM, authdie);
	pqsignal(SIGQUIT, authdie);
	pqsignal(SIGALRM, authdie);
	PG_SETMASK(&AuthBlockSig);

	/*
	 * Get the remote host name and port for logging and status display.
	 */
	remote_host[0] = '\0';
	remote_port[0] = '\0';
	if (getnameinfo_all(&port->raddr.addr, port->raddr.salen,
						remote_host, sizeof(remote_host),
						remote_port, sizeof(remote_port),
				   (log_hostname ? 0 : NI_NUMERICHOST) | NI_NUMERICSERV))
	{
		int			ret = getnameinfo_all(&port->raddr.addr, port->raddr.salen,
										remote_host, sizeof(remote_host),
										remote_port, sizeof(remote_port),
										NI_NUMERICHOST | NI_NUMERICSERV);

		if (ret)
			ereport(WARNING,
					(errmsg("getnameinfo_all() failed: %s",
							gai_strerror(ret))));
	}
	snprintf(remote_ps_data, sizeof(remote_ps_data),
			 remote_port[0] == '\0' ? "%s" : "%s(%s)",
			 remote_host, remote_port);

	if (Log_connections)
		ereport(LOG,
				(errmsg("connection received: host=%s port=%s",
						remote_host, remote_port)));

	/*
	 * save remote_host and remote_port in port stucture
	 */
	port->remote_host = strdup(remote_host);
	port->remote_port = strdup(remote_port);

	/*
	 * In EXEC_BACKEND case, we didn't inherit the contents of pg_hba.c
	 * etcetera from the postmaster, and have to load them ourselves.
	 * Build the PostmasterContext (which didn't exist before, in this
	 * process) to contain the data.
	 *
	 * FIXME: [fork/exec] Ugh.  Is there a way around this overhead?
	 */
#ifdef EXEC_BACKEND
	Assert(PostmasterContext == NULL);
	PostmasterContext = AllocSetContextCreate(TopMemoryContext,
											  "Postmaster",
											  ALLOCSET_DEFAULT_MINSIZE,
											  ALLOCSET_DEFAULT_INITSIZE,
											  ALLOCSET_DEFAULT_MAXSIZE);
	MemoryContextSwitchTo(PostmasterContext);

	load_hba();
	load_ident();
	load_user();
	load_group();
#endif

	/*
	 * Ready to begin client interaction.  We will give up and exit(0)
	 * after a time delay, so that a broken client can't hog a connection
	 * indefinitely.  PreAuthDelay doesn't count against the time limit.
	 */
	if (!enable_sig_alarm(AuthenticationTimeout * 1000, false))
		elog(FATAL, "could not set timer for authorization timeout");

	/*
	 * Receive the startup packet (which might turn out to be a cancel
	 * request packet).
	 */
	status = ProcessStartupPacket(port, false);

	if (status != STATUS_OK)
		proc_exit(0);

	/*
	 * Now that we have the user and database name, we can set the process
	 * title for ps.  It's good to do this as early as possible in
	 * startup.
	 */
	init_ps_display(port->user_name, port->database_name, remote_ps_data);
	set_ps_display("authentication");

	/*
	 * Now perform authentication exchange.
	 */
	ClientAuthentication(port); /* might not return, if failure */

	/*
	 * Done with authentication.  Disable timeout, and prevent
	 * SIGTERM/SIGQUIT again until backend startup is complete.
	 */
	if (!disable_sig_alarm(false))
		elog(FATAL, "could not disable timer for authorization timeout");
	PG_SETMASK(&BlockSig);

	if (Log_connections)
		ereport(LOG,
				(errmsg("connection authorized: user=%s database=%s",
						port->user_name, port->database_name)));

	/*
	 * Don't want backend to be able to see the postmaster random number
	 * generator state.  We have to clobber the static random_seed *and*
	 * start a new random sequence in the random() library function.
	 */
	random_seed = 0;
	gettimeofday(&now, &tz);
	srandom((unsigned int) now.tv_usec);


	/* ----------------
	 * Now, build the argv vector that will be given to PostgresMain.
	 *
	 * The layout of the command line is
	 *		postgres [secure switches] -p databasename [insecure switches]
	 * where the switches after -p come from the client request.
	 *
	 * The maximum possible number of commandline arguments that could come
	 * from ExtraOptions or port->cmdline_options is (strlen + 1) / 2; see
	 * split_opts().
	 * ----------------
	 */
	maxac = 10;					/* for fixed args supplied below */
	maxac += (strlen(ExtraOptions) + 1) / 2;
	if (port->cmdline_options)
		maxac += (strlen(port->cmdline_options) + 1) / 2;

	av = (char **) MemoryContextAlloc(TopMemoryContext,
									  maxac * sizeof(char *));
	ac = 0;

	av[ac++] = "postgres";

	/*
	 * Pass the requested debugging level along to the backend.
	 */
	if (debug_flag > 0)
	{
		snprintf(debugbuf, sizeof(debugbuf), "-d%d", debug_flag);
		av[ac++] = debugbuf;
	}

	/*
	 * Pass any backend switches specified with -o in the postmaster's own
	 * command line.  We assume these are secure.  (It's OK to mangle
	 * ExtraOptions now, since we're safely inside a subprocess.)
	 */
	split_opts(av, &ac, ExtraOptions);

	/* Tell the backend what protocol the frontend is using. */
	snprintf(protobuf, sizeof(protobuf), "-v%u", port->proto);
	av[ac++] = protobuf;

	/*
	 * Tell the backend it is being called from the postmaster, and which
	 * database to use.  -p marks the end of secure switches.
	 */
	av[ac++] = "-p";
	av[ac++] = port->database_name;

	/*
	 * Pass the (insecure) option switches from the connection request.
	 * (It's OK to mangle port->cmdline_options now.)
	 */
	if (port->cmdline_options)
		split_opts(av, &ac, port->cmdline_options);

	av[ac] = NULL;

	Assert(ac < maxac);

	/*
	 * Release postmaster's working memory context so that backend can
	 * recycle the space.  Note this does not trash *MyProcPort, because
	 * ConnCreate() allocated that space with malloc() ... else we'd need
	 * to copy the Port data here.	Also, subsidiary data such as the
	 * username isn't lost either; see ProcessStartupPacket().
	 */
	MemoryContextSwitchTo(TopMemoryContext);
	MemoryContextDelete(PostmasterContext);
	PostmasterContext = NULL;

	/*
	 * Debug: print arguments being passed to backend
	 */
	ereport(DEBUG3,
			(errmsg_internal("%s child[%d]: starting with (",
							 progname, getpid())));
	for (i = 0; i < ac; ++i)
		ereport(DEBUG3,
				(errmsg_internal("\t%s", av[i])));
	ereport(DEBUG3,
			(errmsg_internal(")")));

	ClientAuthInProgress = false;		/* client_min_messages is active
										 * now */

	return (PostgresMain(ac, av, port->user_name));
}


#ifdef EXEC_BACKEND

/*
 * postmaster_forkexec -- fork and exec a postmaster subprocess
 *
 * The caller must have set up the argv array already, except for argv[2]
 * which will be filled with the name of the temp variable file.
 *
 * Returns the child process PID, or -1 on fork failure (a suitable error
 * message has been logged on failure).
 *
 * All uses of this routine will dispatch to SubPostmasterMain in the
 * child process.
 */
pid_t
postmaster_forkexec(int argc, char *argv[])
{
	Port		port;

	/* This entry point passes dummy values for the Port variables */
	memset(&port, 0, sizeof(port));
	return internal_forkexec(argc, argv, &port);
}

/*
 * backend_forkexec -- fork/exec off a backend process
 *
 * returns the pid of the fork/exec'd process, or -1 on failure
 */
static pid_t
backend_forkexec(Port *port)
{
	char	   *av[4];
	int			ac = 0;

	av[ac++] = "postgres";
	av[ac++] = "-forkbackend";
	av[ac++] = NULL;			/* filled in by internal_forkexec */

	av[ac] = NULL;
	Assert(ac < lengthof(av));

	return internal_forkexec(ac, av, port);
}

static pid_t
internal_forkexec(int argc, char *argv[], Port *port)
{
	pid_t		pid;
	char		tmpfilename[MAXPGPATH];

	if (!write_backend_variables(tmpfilename, port))
		return -1;				/* log made by write_backend_variables */

	/* Make sure caller set up argv properly */
	Assert(argc >= 3);
	Assert(argv[argc] == NULL);
	Assert(strncmp(argv[1], "-fork", 5) == 0);
	Assert(argv[2] == NULL);

	/* Insert temp file name after -fork argument */
	argv[2] = tmpfilename;

#ifdef WIN32
	pid = win32_forkexec(postgres_exec_path, argv);
#else
	/* Fire off execv in child */
	if ((pid = fork()) == 0)
	{
		if (execv(postgres_exec_path, argv) < 0)
		{
			ereport(LOG,
					(errmsg("could not exec backend process \"%s\": %m",
							postgres_exec_path)));
			/* We're already in the child process here, can't return */
			exit(1);
		}
	}
#endif

	return pid;					/* Parent returns pid, or -1 on fork failure */
}

/*
 * SubPostmasterMain -- Get the fork/exec'd process into a state equivalent
 *			to what it would be if we'd simply forked on Unix, and then
 *			dispatch to the appropriate place.
 *
 * The first two command line arguments are expected to be "-forkFOO"
 * (where FOO indicates which postmaster child we are to become), and
 * the name of a variables file that we can read to load data that would
 * have been inherited by fork() on Unix.  Remaining arguments go to the
 * subprocess FooMain() routine.
 */
int
SubPostmasterMain(int argc, char *argv[])
{
	Port		port;

	/* Do this sooner rather than later... */
	IsUnderPostmaster = true;	/* we are a postmaster subprocess now */

	MyProcPid = getpid();		/* reset MyProcPid */

	/* In EXEC_BACKEND case we will not have inherited these settings */
	IsPostmasterEnvironment = true;
	whereToSendOutput = None;
	pqinitmask();
	PG_SETMASK(&BlockSig);

	/* Setup essential subsystems */
	MemoryContextInit();
	InitializeGUCOptions();

	/* Check we got appropriate args */
	if (argc < 3)
		elog(FATAL, "invalid subpostmaster invocation");

	/* Read in file-based context */
	memset(&port, 0, sizeof(Port));
	read_backend_variables(argv[2], &port);
	read_nondefault_variables();

	/* Run backend or appropriate child */
	if (strcmp(argv[1], "-forkbackend") == 0)
	{
		/* BackendRun will close sockets */

		/* Attach process to shared segments */
		CreateSharedMemoryAndSemaphores(false, MaxBackends, 0);

		Assert(argc == 3);		/* shouldn't be any more args */
		proc_exit(BackendRun(&port));
	}
	if (strcmp(argv[1], "-forkboot") == 0)
	{
		/* Close the postmaster's sockets */
		ClosePostmasterPorts(false);

		/* Attach process to shared segments */
		CreateSharedMemoryAndSemaphores(false, MaxBackends, 0);

		BootstrapMain(argc - 2, argv + 2);
		proc_exit(0);
	}
	if (strcmp(argv[1], "-forkarch") == 0)
	{
		/* Close the postmaster's sockets */
		ClosePostmasterPorts(false);

		/* Do not want to attach to shared memory */

		PgArchiverMain(argc, argv);
		proc_exit(0);
	}
	if (strcmp(argv[1], "-forkbuf") == 0)
	{
		/* Close the postmaster's sockets */
		ClosePostmasterPorts(false);

		/* Do not want to attach to shared memory */

		PgstatBufferMain(argc, argv);
		proc_exit(0);
	}
	if (strcmp(argv[1], "-forkcol") == 0)
	{
		/*
		 * Do NOT close postmaster sockets here, because we are forking from
		 * pgstat buffer process, which already did it.
		 */

		/* Do not want to attach to shared memory */

		PgstatCollectorMain(argc, argv);
		proc_exit(0);
	}
	if (strcmp(argv[1], "-forklog") == 0)
	{
		/* Close the postmaster's sockets */
		ClosePostmasterPorts(true);

		/* Do not want to attach to shared memory */

		SysLoggerMain(argc, argv);
		proc_exit(0);
	}

	return 1;					/* shouldn't get here */
}

#endif /* EXEC_BACKEND */


/*
 * ExitPostmaster -- cleanup
 *
 * Do NOT call exit() directly --- always go through here!
 */
static void
ExitPostmaster(int status)
{
	/* should cleanup shared memory and kill all backends */

	/*
	 * Not sure of the semantics here.	When the Postmaster dies, should
	 * the backends all be killed? probably not.
	 *
	 * MUST		-- vadim 05-10-1999
	 */

	proc_exit(status);
}

/*
 * sigusr1_handler - handle signal conditions from child processes
 */
static void
sigusr1_handler(SIGNAL_ARGS)
{
	int			save_errno = errno;

	PG_SETMASK(&BlockSig);

	if (CheckPostmasterSignal(PMSIGNAL_PASSWORD_CHANGE))
	{
		/*
		 * Password or group file has changed.
		 */
		load_user();
		load_group();
	}

	if (CheckPostmasterSignal(PMSIGNAL_WAKEN_CHILDREN))
	{
		/*
		 * Send SIGUSR1 to all children (triggers
		 * CatchupInterruptHandler). See storage/ipc/sinval[adt].c for the
		 * use of this.
		 */
		if (Shutdown <= SmartShutdown)
			SignalChildren(SIGUSR1);
	}

	if (PgArchPID != 0 && Shutdown == NoShutdown)
	{
		if (CheckPostmasterSignal(PMSIGNAL_WAKEN_ARCHIVER))
		{
			/*
			 * Send SIGUSR1 to archiver process, to wake it up and begin
			 * archiving next transaction log file.
			 */
            kill(PgArchPID, SIGUSR1);
		}
    }

	PG_SETMASK(&UnBlockSig);

	errno = save_errno;
}


/*
 * Dummy signal handler
 *
 * We use this for signals that we don't actually use in the postmaster,
 * but we do use in backends.  If we were to SIG_IGN such signals in the
 * postmaster, then a newly started backend might drop a signal that arrives
 * before it's able to reconfigure its signal processing.  (See notes in
 * tcop/postgres.c.)
 */
static void
dummy_handler(SIGNAL_ARGS)
{
}


/*
 * CharRemap: given an int in range 0..61, produce textual encoding of it
 * per crypt(3) conventions.
 */
static char
CharRemap(long ch)
{
	if (ch < 0)
		ch = -ch;
	ch = ch % 62;

	if (ch < 26)
		return 'A' + ch;

	ch -= 26;
	if (ch < 26)
		return 'a' + ch;

	ch -= 26;
	return '0' + ch;
}

/*
 * RandomSalt
 */
static void
RandomSalt(char *cryptSalt, char *md5Salt)
{
	long		rand = PostmasterRandom();

	cryptSalt[0] = CharRemap(rand % 62);
	cryptSalt[1] = CharRemap(rand / 62);

	/*
	 * It's okay to reuse the first random value for one of the MD5 salt
	 * bytes, since only one of the two salts will be sent to the client.
	 * After that we need to compute more random bits.
	 *
	 * We use % 255, sacrificing one possible byte value, so as to ensure
	 * that all bits of the random() value participate in the result.
	 * While at it, add one to avoid generating any null bytes.
	 */
	md5Salt[0] = (rand % 255) + 1;
	rand = PostmasterRandom();
	md5Salt[1] = (rand % 255) + 1;
	rand = PostmasterRandom();
	md5Salt[2] = (rand % 255) + 1;
	rand = PostmasterRandom();
	md5Salt[3] = (rand % 255) + 1;
}

/*
 * PostmasterRandom
 */
static long
PostmasterRandom(void)
{
	static bool initialized = false;

	if (!initialized)
	{
		Assert(random_seed != 0);
		srandom(random_seed);
		initialized = true;
	}

	return random();
}

/*
 * Count up number of child processes (regular backends only)
 */
static int
CountChildren(void)
{
	Dlelem	   *curr;
	int			cnt = 0;

	for (curr = DLGetHead(BackendList); curr; curr = DLGetSucc(curr))
	{
		cnt++;
	}
	return cnt;
}


/*
 * StartChildProcess -- start a non-backend child process for the postmaster
 *
 * xlog determines what kind of child will be started.  All child types
 * initially go to BootstrapMain, which will handle common setup.
 *
 * Return value of StartChildProcess is subprocess' PID, or 0 if failed
 * to start subprocess.
 */
static pid_t
StartChildProcess(int xlop)
{
	pid_t		pid;
	char	   *av[10];
	int			ac = 0;
	char		xlbuf[32];
#ifdef LINUX_PROFILE
	struct itimerval prof_itimer;
#endif

	/*
	 * Set up command-line arguments for subprocess
	 */
	av[ac++] = "postgres";

#ifdef EXEC_BACKEND
	av[ac++] = "-forkboot";
	av[ac++] = NULL;			/* filled in by postmaster_forkexec */
#endif

	snprintf(xlbuf, sizeof(xlbuf), "-x%d", xlop);
	av[ac++] = xlbuf;

	av[ac++] = "-p";
	av[ac++] = "template1";

	av[ac] = NULL;
	Assert(ac < lengthof(av));

	/*
	 * Flush stdio channels (see comments in BackendStartup)
	 */
	fflush(stdout);
	fflush(stderr);

#ifdef EXEC_BACKEND

	pid = postmaster_forkexec(ac, av);

#else /* !EXEC_BACKEND */

#ifdef LINUX_PROFILE
	/* see comments in BackendStartup */
	getitimer(ITIMER_PROF, &prof_itimer);
#endif

#ifdef __BEOS__
	/* Specific beos actions before backend startup */
	beos_before_backend_startup();
#endif

	pid = fork();

	if (pid == 0)				/* child */
	{
#ifdef LINUX_PROFILE
		setitimer(ITIMER_PROF, &prof_itimer, NULL);
#endif

#ifdef __BEOS__
		/* Specific beos actions after backend startup */
		beos_backend_startup();
#endif

		IsUnderPostmaster = true;	/* we are a postmaster subprocess now */

		/* Close the postmaster's sockets */
		ClosePostmasterPorts(false);

		/* Lose the postmaster's on-exit routines and port connections */
		on_exit_reset();

		/* Release postmaster's working memory context */
		MemoryContextSwitchTo(TopMemoryContext);
		MemoryContextDelete(PostmasterContext);
		PostmasterContext = NULL;

		BootstrapMain(ac, av);
		ExitPostmaster(0);
	}

#endif /* EXEC_BACKEND */

	if (pid < 0)
	{
		/* in parent, fork failed */
		int			save_errno = errno;

#ifdef __BEOS__
		/* Specific beos actions before backend startup */
		beos_backend_startup_failed();
#endif
		errno = save_errno;
		switch (xlop)
		{
			case BS_XLOG_STARTUP:
				ereport(LOG,
						(errmsg("could not fork startup process: %m")));
				break;
			case BS_XLOG_BGWRITER:
				ereport(LOG,
						(errmsg("could not fork background writer process: %m")));
				break;
			default:
				ereport(LOG,
						(errmsg("could not fork process: %m")));
				break;
		}

		/*
		 * fork failure is fatal during startup, but there's no need
		 * to choke immediately if starting other child types fails.
		 */
		if (xlop == BS_XLOG_STARTUP)
			ExitPostmaster(1);
		return 0;
	}

	/*
	 * in parent, successful fork
	 */
	return pid;
}


/*
 * Create the opts file
 */
static bool
CreateOptsFile(int argc, char *argv[], char *fullprogname)
{
	char		filename[MAXPGPATH];
	FILE	   *fp;
	int			i;

	snprintf(filename, sizeof(filename), "%s/postmaster.opts", DataDir);

	if ((fp = fopen(filename, "w")) == NULL)
	{
		elog(LOG, "could not create file \"%s\": %m", filename);
		return false;
	}

	fprintf(fp, "%s", fullprogname);
	for (i = 1; i < argc; i++)
		fprintf(fp, " '%s'", argv[i]);
	fputs("\n", fp);

	if (fclose(fp))
	{
		elog(LOG, "could not write file \"%s\": %m", filename);
		return false;
	}

	return true;
}


#ifdef EXEC_BACKEND

/*
 * The following need to be available to the read/write_backend_variables
 * functions
 */
#include "storage/spin.h"

extern slock_t *ShmemLock;
extern slock_t *ShmemIndexLock;
extern void *ShmemIndexAlloc;
typedef struct LWLock LWLock;
extern LWLock *LWLockArray;
extern slock_t *ProcStructLock;
extern int	pgStatSock;

#define write_var(var,fp) fwrite((void*)&(var),sizeof(var),1,fp)
#define read_var(var,fp)  fread((void*)&(var),sizeof(var),1,fp)
#define write_array_var(var,fp) fwrite((void*)(var),sizeof(var),1,fp)
#define read_array_var(var,fp)  fread((void*)(var),sizeof(var),1,fp)

static bool
write_backend_variables(char *filename, Port *port)
{
	static unsigned long tmpBackendFileNum = 0;
	FILE	   *fp;
	char		str_buf[MAXPGPATH];

	/* Calculate name for temp file in caller's buffer */
	Assert(DataDir);
	snprintf(filename, MAXPGPATH, "%s/%s/%s.backend_var.%d.%lu",
			 DataDir, PG_TEMP_FILES_DIR, PG_TEMP_FILE_PREFIX,
			 MyProcPid, ++tmpBackendFileNum);

	/* Open file */
	fp = AllocateFile(filename, PG_BINARY_W);
	if (!fp)
	{
		/* As per OpenTemporaryFile... */
		char		dirname[MAXPGPATH];

		snprintf(dirname, MAXPGPATH, "%s/%s", DataDir, PG_TEMP_FILES_DIR);
		mkdir(dirname, S_IRWXU);

		fp = AllocateFile(filename, PG_BINARY_W);
		if (!fp)
		{
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not create file \"%s\": %m",
							filename)));
			return false;
		}
	}

	/* Write vars */
	write_var(port->sock, fp);
	write_var(port->proto, fp);
	write_var(port->laddr, fp);
	write_var(port->raddr, fp);
	write_var(port->canAcceptConnections, fp);
	write_var(port->cryptSalt, fp);
	write_var(port->md5Salt, fp);

	/*
	 * XXX FIXME later: writing these strings as MAXPGPATH bytes always is
	 * probably a waste of resources
	 */

	StrNCpy(str_buf, DataDir, MAXPGPATH);
	write_array_var(str_buf, fp);

	write_array_var(ListenSocket, fp);

	write_var(MyCancelKey, fp);

	write_var(UsedShmemSegID, fp);
	write_var(UsedShmemSegAddr, fp);

	write_var(ShmemLock, fp);
	write_var(ShmemIndexLock, fp);
	write_var(ShmemVariableCache, fp);
	write_var(ShmemIndexAlloc, fp);
	write_var(ShmemBackendArray, fp);

	write_var(LWLockArray, fp);
	write_var(ProcStructLock, fp);
	write_var(pgStatSock, fp);

	write_var(debug_flag, fp);
	write_var(PostmasterPid, fp);
#ifdef WIN32
	write_var(PostmasterHandle, fp);
#endif

	write_var(syslogPipe[0], fp);
	write_var(syslogPipe[1], fp);

	StrNCpy(str_buf, my_exec_path, MAXPGPATH);
	write_array_var(str_buf, fp);

	write_array_var(ExtraOptions, fp);

	StrNCpy(str_buf, setlocale(LC_COLLATE, NULL), MAXPGPATH);
	write_array_var(str_buf, fp);
	StrNCpy(str_buf, setlocale(LC_CTYPE, NULL), MAXPGPATH);
	write_array_var(str_buf, fp);

	/* Release file */
	if (FreeFile(fp))
	{
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m", filename)));
		return false;
	}

	return true;
}

static void
read_backend_variables(char *filename, Port *port)
{
	FILE	   *fp;
	char		str_buf[MAXPGPATH];

	/* Open file */
	fp = AllocateFile(filename, PG_BINARY_R);
	if (!fp)
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg("could not read from backend variables file \"%s\": %m",
						filename)));

	/* Read vars */
	read_var(port->sock, fp);
	read_var(port->proto, fp);
	read_var(port->laddr, fp);
	read_var(port->raddr, fp);
	read_var(port->canAcceptConnections, fp);
	read_var(port->cryptSalt, fp);
	read_var(port->md5Salt, fp);

	read_array_var(str_buf, fp);
	SetDataDir(str_buf);

	read_array_var(ListenSocket, fp);

	read_var(MyCancelKey, fp);

	read_var(UsedShmemSegID, fp);
	read_var(UsedShmemSegAddr, fp);

	read_var(ShmemLock, fp);
	read_var(ShmemIndexLock, fp);
	read_var(ShmemVariableCache, fp);
	read_var(ShmemIndexAlloc, fp);
	read_var(ShmemBackendArray, fp);

	read_var(LWLockArray, fp);
	read_var(ProcStructLock, fp);
	read_var(pgStatSock, fp);

	read_var(debug_flag, fp);
	read_var(PostmasterPid, fp);
#ifdef WIN32
	read_var(PostmasterHandle, fp);
#endif

	read_var(syslogPipe[0], fp);
	read_var(syslogPipe[1], fp);

	read_array_var(str_buf, fp);
	StrNCpy(my_exec_path, str_buf, MAXPGPATH);

	read_array_var(ExtraOptions, fp);

	read_array_var(str_buf, fp);
	setlocale(LC_COLLATE, str_buf);
	read_array_var(str_buf, fp);
	setlocale(LC_CTYPE, str_buf);

	/* Release file */
	FreeFile(fp);
	if (unlink(filename) != 0)
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not remove file \"%s\": %m", filename)));
}


size_t
ShmemBackendArraySize(void)
{
	return (NUM_BACKENDARRAY_ELEMS * sizeof(Backend));
}

void
ShmemBackendArrayAllocation(void)
{
	size_t		size = ShmemBackendArraySize();

	ShmemBackendArray = (Backend *) ShmemAlloc(size);
	/* Mark all slots as empty */
	memset(ShmemBackendArray, 0, size);
}

static void
ShmemBackendArrayAdd(Backend *bn)
{
	int			i;

	/* Find an empty slot */
	for (i = 0; i < NUM_BACKENDARRAY_ELEMS; i++)
	{
		if (ShmemBackendArray[i].pid == 0)
		{
			ShmemBackendArray[i] = *bn;
			return;
		}
	}

	ereport(FATAL,
			(errmsg_internal("no free slots in shmem backend array")));
}

static void
ShmemBackendArrayRemove(pid_t pid)
{
	int			i;

	for (i = 0; i < NUM_BACKENDARRAY_ELEMS; i++)
	{
		if (ShmemBackendArray[i].pid == pid)
		{
			/* Mark the slot as empty */
			ShmemBackendArray[i].pid = 0;
			return;
		}
	}

	ereport(WARNING,
			(errmsg_internal("could not find backend entry with pid %d",
							 (int) pid)));
}

#endif /* EXEC_BACKEND */


#ifdef WIN32

static pid_t
win32_forkexec(const char *path, char *argv[])
{
	STARTUPINFO si;
	PROCESS_INFORMATION pi;
	int			i;
	int			j;
	char		cmdLine[MAXPGPATH * 2];
	HANDLE		childHandleCopy;
	HANDLE		waiterThread;

	/* Format the cmd line */
	cmdLine[sizeof(cmdLine)-1] = '\0';
	cmdLine[sizeof(cmdLine)-2] = '\0';
	snprintf(cmdLine, sizeof(cmdLine)-1, "\"%s\"", path);
	i = 0;
	while (argv[++i] != NULL)
	{
		j = strlen(cmdLine);
		snprintf(cmdLine+j, sizeof(cmdLine)-1-j, " \"%s\"", argv[i]);
	}
	if (cmdLine[sizeof(cmdLine)-2] != '\0')
	{
		elog(LOG, "subprocess command line too long");
		return -1;
	}

	memset(&pi, 0, sizeof(pi));
	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);
	if (!CreateProcess(NULL, cmdLine, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi))
	{
		elog(LOG, "CreateProcess call failed (%d): %m", (int) GetLastError());
		return -1;
	}

	if (!IsUnderPostmaster)
	{
		/* We are the Postmaster creating a child... */
		win32_AddChild(pi.dwProcessId, pi.hProcess);
	}

	if (DuplicateHandle(GetCurrentProcess(),
						pi.hProcess,
						GetCurrentProcess(),
						&childHandleCopy,
						0,
						FALSE,
						DUPLICATE_SAME_ACCESS) == 0)
		ereport(FATAL,
				(errmsg_internal("could not duplicate child handle: %d",
								 (int) GetLastError())));

	waiterThread = CreateThread(NULL, 64 * 1024, win32_sigchld_waiter,
								(LPVOID) childHandleCopy, 0, NULL);
	if (!waiterThread)
		ereport(FATAL,
				(errmsg_internal("could not create sigchld waiter thread: %d",
								 (int) GetLastError())));
	CloseHandle(waiterThread);

	if (IsUnderPostmaster)
		CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);

	return pi.dwProcessId;
}

/*
 * Note: The following three functions must not be interrupted (eg. by
 * signals).  As the Postgres Win32 signalling architecture (currently)
 * requires polling, or APC checking functions which aren't used here, this
 * is not an issue.
 *
 * We keep two separate arrays, instead of a single array of pid/HANDLE
 * structs, to avoid having to re-create a handle array for
 * WaitForMultipleObjects on each call to win32_waitpid.
 */

static void
win32_AddChild(pid_t pid, HANDLE handle)
{
	Assert(win32_childPIDArray && win32_childHNDArray);
	if (win32_numChildren < NUM_BACKENDARRAY_ELEMS)
	{
		win32_childPIDArray[win32_numChildren] = pid;
		win32_childHNDArray[win32_numChildren] = handle;
		++win32_numChildren;
	}
	else
		ereport(FATAL,
				(errmsg_internal("no room for child entry with pid %lu",
								 (unsigned long) pid)));
}

static void
win32_RemoveChild(pid_t pid)
{
	int			i;

	Assert(win32_childPIDArray && win32_childHNDArray);

	for (i = 0; i < win32_numChildren; i++)
	{
		if (win32_childPIDArray[i] == pid)
		{
			CloseHandle(win32_childHNDArray[i]);

			/* Swap last entry into the "removed" one */
			--win32_numChildren;
			win32_childPIDArray[i] = win32_childPIDArray[win32_numChildren];
			win32_childHNDArray[i] = win32_childHNDArray[win32_numChildren];
			return;
		}
	}

	ereport(WARNING,
			(errmsg_internal("could not find child entry with pid %lu",
							 (unsigned long) pid)));
}

static pid_t
win32_waitpid(int *exitstatus)
{
	Assert(win32_childPIDArray && win32_childHNDArray);
	elog(DEBUG3, "waiting on %lu children", win32_numChildren);

	if (win32_numChildren > 0)
	{
		/*
		 * Note: Do NOT use WaitForMultipleObjectsEx, as we don't want to
		 * run queued APCs here.
		 */
		int			index;
		DWORD		exitCode;
		DWORD		ret;

		ret = WaitForMultipleObjects(win32_numChildren, win32_childHNDArray,
									 FALSE, 0);
		switch (ret)
		{
			case WAIT_FAILED:
				ereport(LOG,
				   (errmsg_internal("failed to wait on %lu children: %d",
							  win32_numChildren, (int) GetLastError())));
				return -1;

			case WAIT_TIMEOUT:
				/* No children have finished */
				return -1;

			default:

				/*
				 * Get the exit code, and return the PID of, the
				 * respective process
				 */
				index = ret - WAIT_OBJECT_0;
				Assert(index >= 0 && index < win32_numChildren);
				if (!GetExitCodeProcess(win32_childHNDArray[index], &exitCode))
				{
					/*
					 * If we get this far, this should never happen, but,
					 * then again... No choice other than to assume a
					 * catastrophic failure.
					 */
					ereport(FATAL,
							(errmsg_internal("failed to get exit code for child %lu",
										   win32_childPIDArray[index])));
				}
				*exitstatus = (int) exitCode;
				return win32_childPIDArray[index];
		}
	}

	/* No children */
	return -1;
}

/*
 * Note! Code below executes on separate threads, one for
 * each child process created
 */
static DWORD WINAPI
win32_sigchld_waiter(LPVOID param)
{
	HANDLE		procHandle = (HANDLE) param;

	DWORD		r = WaitForSingleObject(procHandle, INFINITE);

	if (r == WAIT_OBJECT_0)
		pg_queue_signal(SIGCHLD);
	else
		write_stderr("ERROR: failed to wait on child process handle: %d\n",
				(int) GetLastError());
	CloseHandle(procHandle);
	return 0;
}

#endif /* WIN32 */
