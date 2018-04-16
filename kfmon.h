/*
	KFMon: Kobo inotify-based launcher
	Copyright (C) 2016-2018 NiLuJe <ninuje@gmail.com>

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU Affero General Public License as
	published by the Free Software Foundation, either version 3 of the
	License, or (at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU Affero General Public License for more details.

	You should have received a copy of the GNU Affero General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// For syscall, and the expected version of strerror_r
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <sys/types.h>
#include <limits.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <fts.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/inotify.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>
#include <mntent.h>
#include <string.h>
#include <linux/limits.h>
#include <syslog.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <sqlite3.h>
#include "inih/ini.h"

// Fallback version tag...
#ifndef KFMON_VERSION
#define KFMON_VERSION "v0.9.5"
#endif

// Do an ifdef check to allow overriding those at compile-time...
#ifndef KFMON_TARGET_MOUNTPOINT
#define KFMON_TARGET_MOUNTPOINT "/mnt/onboard"
#endif
// Use my debug paths on demand...
#ifndef NILUJE
#define KOBO_DB_PATH KFMON_TARGET_MOUNTPOINT "/.kobo/KoboReader.sqlite"
#define KFMON_LOGFILE "/usr/local/kfmon/kfmon.log"
#define KFMON_CONFIGPATH KFMON_TARGET_MOUNTPOINT "/.adds/kfmon/config"
#else
#define KOBO_DB_PATH "/home/niluje/Kindle/Staging/KoboReader.sqlite"
#define KFMON_LOGFILE "/home/niluje/Kindle/Staging/kfmon.log"
#define KFMON_CONFIGPATH "/home/niluje/Kindle/Staging/kfmon"
#endif

// Log everything to stderr (which actually points to our logfile)
#define LOG(prio, fmt, ...) ({													\
	if (daemon_config.use_syslog) {												\
		syslog(prio, fmt "\n", ## __VA_ARGS__);										\
	} else {														\
		fprintf(stderr, "[KFMon] [%s] [%s] " fmt "\n", get_current_time(), get_log_prefix(prio), ## __VA_ARGS__);	\
	}															\
})																\

// Slight variation without date/time handling to ensure thread safety
#define MTLOG(fmt, ...) ({									\
	if (daemon_config.use_syslog) {								\
		syslog(LOG_NOTICE, fmt "\n", ## __VA_ARGS__);					\
	} else {										\
		fprintf(stderr, "[KFMon] [THRD: " fmt "\n", ## __VA_ARGS__);			\
	}											\
})												\

// Some extra verbose stuff is relegated to DEBUG builds... (c.f., https://stackoverflow.com/questions/1644868)
#ifdef DEBUG
#define DEBUG_LOG 1
#else
#define DEBUG_LOG 0
#endif
#define DBGLOG(fmt, ...) ({				\
	if (DEBUG_LOG) {				\
		LOG(LOG_DEBUG, fmt, ## __VA_ARGS__);	\
	}						\
})							\

// What the daemon config should look like
typedef struct
{
	int db_timeout;
	long int tmp_use_syslog;
	bool use_syslog;
} DaemonConfig;

// What a watch config should look like
#define DB_SZ_MAX 128
typedef struct
{
	char filename[PATH_MAX];
	char action[PATH_MAX];
	long int tmp_do_db_update;
	bool do_db_update;
	long int tmp_skip_db_checks;
	bool skip_db_checks;
	char db_title[DB_SZ_MAX];
	char db_author[DB_SZ_MAX];
	char db_comment[DB_SZ_MAX];
	int inotify_wd;
	bool wd_was_destroyed;
} WatchConfig;

// Hardcode the max amount of watches we handle
#define WATCH_MAX 16

// Used to keep track of our spawned processes, by storing their pids, and their watch idx.
// c.f., https://stackoverflow.com/a/35235950 & https://stackoverflow.com/a/8976461
// As well as issue #2 for details of past failures w/ a SIGCHLD handler
struct process_table {
	pid_t spawn_pids[WATCH_MAX];
	int spawn_watchids[WATCH_MAX];
} PT;
pthread_mutex_t ptlock = PTHREAD_MUTEX_INITIALIZER;
static void init_process_table(void);
static int get_next_available_pt_entry(void);
static void add_process_to_table(int, pid_t, unsigned int);
static void remove_process_from_table(int);

// SQLite macros inspired from http://www.lemoda.net/c/sqlite-insert/ :)
#define CALL_SQLITE(f) ({					\
	int i;							\
	i = sqlite3_ ## f;					\
	if (i != SQLITE_OK) {					\
		LOG(LOG_CRIT, "%s failed with status %d: %s",	\
			#f, i, sqlite3_errmsg(db));		\
		return is_processed;				\
	}							\
})								\

// Remember stdin/stdout/stderr to restore them in our children
int orig_stdin;
int orig_stdout;
int orig_stderr;
static int daemonize(void);

struct tm *get_localtime(struct tm *);
char *get_current_time(void);
const char *get_log_prefix(int);

static bool is_target_mounted(void);
static void wait_for_target_mountpoint(void);

static long int sane_atoi(const char *);
static int daemon_handler(void *, const char *, const char *, const char *);
static bool validate_daemon_config(void *);
static int watch_handler(void *, const char *, const char *, const char *);
static bool validate_watch_config(void *);
static int load_config(void);
// Ugly global. Remember how many watches we set up...
size_t watch_count = 0;
// Make our config global, because I'm terrible at C.
DaemonConfig daemon_config = {0};
WatchConfig watch_config[WATCH_MAX] = {0};

static unsigned int qhash(const unsigned char *, size_t);
static bool is_target_processed(unsigned int, bool);

void *reaper_thread(void *);
static pid_t spawn(char *const *, unsigned int);

static bool is_watch_already_spawned(unsigned int);
static pid_t get_spawn_pid_for_watch(unsigned int);

static bool handle_events(int);
