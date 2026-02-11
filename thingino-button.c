/*
 * thingino_button
 *
 * Monitor input events specified via the Linux generic input device interface
 * and execute commands based on the actions lodaed from the configuration file.
 *
 * It is primarily aimed at embedded systems where specific actions need to be
 * triggered by pressing, releasing, or holding down buttons on the device.
 *
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#define DEFAULT_CONFIG_FILE  "/etc/thingino-button.conf"
#define DEFAULT_INPUT_DEVICE "/dev/input/event0"

/*
 * Data types and structures
 */

#define EV_KEY 0x01

/*
 * Structure used to receive events from the event device file. The field types
 * have been chosen to work on 32-bit and 64-bit architectures.
 */
struct input_event {
	long unsigned int tv_sec;
	long unsigned int tv_usec;
	unsigned short type;
	unsigned short code;
	unsigned int value;
};

/*
 * Enumeration used to record the action type, which can be key pressed, key
 * released, key held down and then released after a time, and key held down
 * for a time.
 */
enum action_code {
	ACTION_UNASSIGNED = -1,
	ACTION_PRESS = 0,		// key pressed
	ACTION_RELEASE,			// key released
	ACTION_TIMED,			// key held for a time then released
	ACTION_TIMED_FIRE,		// key held for a time
	ACTION_CODE_MAX,
};

/*
 * Structure used to record a complete action
 */
struct action {
	int key_code;			// key code of the key (e.g. KEY_ENTER)
	enum action_code action_code;	// action type (press, release, etc.)
	float time;			// held down time for timed actions
	const char *command;		// command to execute
};

/*
 * This structure holds the status of each key that has one or more actions
 * associated with it. The pressed_at field is used to record the last time
 * the key was pressed, which is used for key release and long press actions.
 */
struct key {
	int key_code;			// key code of the key (e.g. KEY_ENTER)
	struct timeval pressed_at;	// the last time the key was pressed
	int action[ACTION_CODE_MAX];	// index of each action type
};

/*
 * This structure is used to track TIMED_FIRE actions. These fire when a key
 * is held down for a set length of time. When a key pressed event is received,
 * we calculate the time the action will fire if the key remains held down. The
 * `time' field is used to avoid looking it up from the action table and is not
 * strictly necessary.
 */
struct held_timer {
	struct timeval fires_at;	// when this action will fire
	int action_idx;			// index into action_table
	int key_idx;			// index into key table
	float time;			// copy of time from action table
};

/*
 * Constants
 */

/* loglevel table used when logging to stdout */
static const char *const loglevels[] = {
	"emergency",
	"alert",
	"critical",
	"error",
	"warning",
	"notice",
	"info",
	"debug"
};

/*
 * String lists used to parse keywords in the configuration file.
 */

static const char *const action_names =
	"PRESS\0"
	"RELEASE\0"
	"TIMED\0"
	"TIMED_FIRE\0" ;

static const char event_codes[] = {
	28, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12
};

// Supports 0-9, MINUS, and ENTER
static const char *const event_names =
/* KEY_ENTER = 28 */ "ENTER\0"
/* KEY_1     =  2 */ "1\0"
/* KEY_2     =  3 */ "2\0"
/* KEY_3     =  4 */ "3\0"
/* KEY_4     =  5 */ "4\0"
/* KEY_5     =  6 */ "5\0"
/* KEY_6     =  7 */ "6\0"
/* KEY_7     =  8 */ "7\0"
/* KEY_8     =  9 */ "8\0"
/* KEY_9     = 10 */ "9\0"
/* KEY_0     = 11 */ "0\0"
/* KEY_MINUS = 12 */ "MINUS\0";

/*
 * Global variables
 */

static char *program_name   = NULL;

static char *input_device   = NULL;
static char *cmdline_device = NULL;

static char *config_file    = DEFAULT_CONFIG_FILE;

static int run_as_daemon = 0;
static int log_to_syslog = 0;
static int log_activated = 0;
static volatile sig_atomic_t exit_now = 0;

static struct action *action_tbl = NULL;
static struct key *key_tbl = NULL;

static int action_tbl_size = 0;
static int key_tbl_size = 0;

static struct held_timer *held_timer_tbl = NULL;

/*
 * The held timer differs from the action and key tables in that the number of
 * entries varies over time.
 */
static int held_timer_entries = 0;

/** Log a message to syslog or stdout.
 *
 */
static void log_message(const int level, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	if (log_activated) {
		vsyslog(level, format, args);
	} else {
		printf("%s: ", loglevels[level]);
		vprintf(format, args);
		putchar('\n');
	}
	va_end(args);
}

/*
 * The compiler isn't efficient when inlining variadic functions so use #define
 * to make these convenience functions.
 */
#ifdef NDEBUG
#define	log_debug(...)
#else
#define log_debug(...)     log_message(LOG_DEBUG,   __VA_ARGS__)
#endif
#define log_info(...)      log_message(LOG_INFO,    __VA_ARGS__)
#define log_notice(...)    log_message(LOG_NOTICE,  __VA_ARGS__)
#define log_warning(...)   log_message(LOG_WARNING, __VA_ARGS__)
#define log_error(...)     log_message(LOG_ERR,     __VA_ARGS__)
#define fatal_error(...) { log_message(LOG_ERR,     __VA_ARGS__);\
			   exit(EXIT_FAILURE);                   }

/** Allocate memory for key table.
 *
 * Search the action table and count the total number of unique key codes, then
 * allocate memory for that many in the key table. The action table is sorted
 * by key code, so we count the number of times the key code changes.
 */
static void alloc_key_tbl()
{
	int key_code = -1;
	int size = 0;
	for (int action_idx = 0; action_idx < action_tbl_size; ++action_idx) {
		if (action_tbl[action_idx].key_code != key_code) {
			key_code = action_tbl[action_idx].key_code;
			++size;
		}
	}
	key_tbl = calloc(size, sizeof(key_tbl[0]));
	if (key_tbl == NULL)
		fatal_error("%s() failed, exiting: %m", "calloc");
}

/** Build the key table.
 *
 * Search the action table (sorted by key code), and for each new key code,
 * record the index into the action table of the first action for each of
 * the four action types (or -1 if there are none of that type).
 */
static void build_key_tbl()
{
	alloc_key_tbl();

	int key_code = -1;
	enum action_code action_code = ACTION_UNASSIGNED;
	int key_idx = -1;

	for (int action_idx = 0; action_idx < action_tbl_size; ++action_idx) {
		if (action_tbl[action_idx].key_code != key_code) {
			key_idx += 1;
			key_code = action_tbl[action_idx].key_code;
			action_code = ACTION_UNASSIGNED;
			key_tbl[key_idx].key_code = key_code;
			for (int i = 0; i < ACTION_CODE_MAX; ++i)
				key_tbl[key_idx].action[i] = -1;
		}
		else if (action_tbl[action_idx].action_code == action_code)
			continue;

		// Either a new action code or a new key (therefore action code)
		action_code = action_tbl[action_idx].action_code;
		key_tbl[key_idx].action[action_code] = action_idx;
	}

	key_tbl_size = key_idx + 1;
}

/** Search a string list for a string.
 *
 * A string list is of the format stringa\0stringb\0...stringn\0\0. Search
 * the string list for a given string and return a zero-based index (e.g.
 * if it was the second string in the list, return 1). If the string is not
 * found, return -1.
 */
static int strsearch(const char *const list, const char *const str)
{
	int i = -1;

	const char *lp = list;
	while (*lp) {
		i += 1;
		const char *sp = str;
		while (*lp == *sp) {
			if (*lp == 0 && *sp == 0)
				return i;
			++lp; ++sp;
		}
		while (*lp++ != 0)
			;
	}

	return -1;
}

/** Parse action code.
 *
 * Convert the action string into an enum action_code by searching the
 * action_names string.
 */
static enum action_code parse_action_code(const char *const action)
{
	return (enum action_code)strsearch(action_names, action);
}

/** Parse key code.
 *
 * Convert the key code string into an integer key code by searching the
 * event_names string. As all they keys are prefixed KEY_ we strip that
 * off the beginning to save a little memory.
 */
static int parse_key_code(const char *const name)
{
	const char *str;

	// To speed up the search and save a small amount of space, we strip
	// off the `KEY_' prefix
	if (name[0] == 'K' &&
	    name[1] == 'E' &&
	    name[2] == 'Y' &&
	    name[3] == '_')
		str = &name[4];
	else
		return -1;

	int i = strsearch(event_names, str);
	if (i >= 0)
		return (int)event_codes[i];
	else
		return -1;
}

/** Insert action into action table.
 *
 * Scan the table and identify the correct location to insert the action.
 *
 * We maintain a list of actions sorted by key code, action code, and time.
 *
 */
static void insert_action(const int key_code,
			  const enum action_code action_code, float time,
			  const char *const command)
{
	int i;
	for (i = 0; i < action_tbl_size; ++i) {
		if ((key_code     < action_tbl[i].key_code)
				 ||
		   ((key_code    == action_tbl[i].key_code) &&
		    (action_code  < action_tbl[i].action_code))
				 ||
		   ((key_code    == action_tbl[i].key_code) &&
		    (action_code == action_tbl[i].action_code) &&
		    (time         < action_tbl[i].time))) {
			memmove(&action_tbl[i+1],
				&action_tbl[i],
				(action_tbl_size-i) * sizeof(struct action));
			break;
		}
	}

	action_tbl[i].key_code = key_code;
	action_tbl[i].action_code = action_code;
	action_tbl[i].time = time;
	action_tbl[i].command = command;
	++action_tbl_size;
}

/** Parse configuration file.
 *
 * Parse the configuration file, which is passed as an open file. This function
 * is called twice. The first time dry_run is set to 1 and it returns the
 * number of actions so that memory for the action table to be allocated. The
 * second time it adds the actions into the table and returns the number of
 * TIMED_FIRE (held down) timers, so that memory for the held timer table can
 * be allocated.
 */
static int parse_config(FILE *file, const int dry_run)
{
	char key[20], action[20], line[512];
	int action_count = 0;
	int held_timer_count = 0;
	int device_lines = 0;

	while (fgets(line, sizeof(line), file)) {
		// Remove trailing newline character from the line
		size_t len = strlen(line);
		if (line[len-1] != '\n')
			fatal_error("Config file line too long (>%d), exiting",
				    sizeof(line));
		line[--len] = '\0';

		// Skip blank and simple comment lines
		if (len == 0 || line[0] == '#')
			continue;

		// Parse DEVICE= lines
		if (dry_run) {
			int offset = 0;
			sscanf(line, " DEVICE = %*[^\n]%n", &offset);
			if (offset > 0) {
				++device_lines;
				continue;
			}
		}
		else {
			if (sscanf(line, " DEVICE = %m[^\n]",
				   &input_device)) {
				++device_lines;
				continue;
			}
		}

		// Parse standard line with time value
		float time;
		char *command;
		int parsed = sscanf(line, " %19s %19s %f %m[^\n]",
				    key, action, &time, &command);

		if (parsed < 4) {
			// Parse without it and default the time to zero
			time = 0.0;
			parsed = sscanf(line, " %19s %19s %m[^\n]",
					key, action, &command);
		}

		// Skip comments
		if (key[0] == '#')
			continue;

		if (parsed < 3)
			fatal_error("Cannot parse %s, exiting: '%s'",
				    "line", line);

		int key_code = parse_key_code(key);
		if (key_code == -1)
			fatal_error("Cannot parse %s, exiting: '%s'",
				    "key code", key);

		int action_code = parse_action_code(action);
		if (action_code == -1)
			fatal_error("Cannot parse %s, exiting: '%s'",
				    "action", action);

		if (action_code == ACTION_TIMED_FIRE)
			held_timer_count++;

		++action_count;

		if (!dry_run)
			insert_action(key_code, action_code, time, command);
		continue;
	}

	if (device_lines > 1)
		fatal_error("More than one device line, exiting", device_lines);

	return dry_run ? action_count : held_timer_count;
}

/** Load the configuration file.
 *
 * Opens the file and calls parse_config() in dry run mode to get the number of
 * actions, then allocates the action table. It resets the file offset then
 * calls parse_config() again to add the actions. The return value is the
 * number of "held down" actions, which is used to allocate memory for the held
 * timer table. Finally, the key table is created.
 */
static void load_config()
{
	FILE *file = fopen(config_file, "r");
	if (!file)
		fatal_error("Failed to open config file '%s': %m", config_file);

	// Parse file in "dry run" mode to calculate the number of actions
	int action_count = parse_config(file, /* dry run */ 1);
	action_tbl = calloc(action_count, sizeof(struct action));
	if (action_tbl == NULL)
		fatal_error("%s() failed, exiting: %m", "calloc");

	// Parse file again, this time filling the action table
	rewind(file);
	int held_timer_count = parse_config(file, /* for real */ 0);
	fclose(file);

	// Allocate space for the button held down timers
	held_timer_tbl = calloc(held_timer_count, sizeof(struct held_timer));
	if (held_timer_tbl == NULL)
		fatal_error("%s() failed, exiting: %m", "calloc");


	// device priority: default unless config file unless command line
	if (cmdline_device != NULL)
		input_device = cmdline_device;
	if (input_device == NULL)
		input_device = DEFAULT_INPUT_DEVICE;

	build_key_tbl();

	log_info("Loaded %d key(s) and %d action mapping(s) from %s",
		 key_tbl_size, action_tbl_size, config_file);
}

/** Execute the given command.
 *
 * Use fork() and execl() to execute the given command.
 */
static void execute_command(const char *const command)
{
	log_notice("Executing command: [%s]", command);

	pid_t pid = fork();
	if (pid < 0) {
		// This may be a temporary condition, so continue
		log_error("%s() failed, continuing: %m", "fork");
		return;
	}
	if (pid == 0) {
		execl("/bin/sh", "sh", "-c", command, (char *)NULL);
		fatal_error("%s() failed, exiting: %m", "execl");
	}
}

/** Add time to a struct timeval.
 *
 * Given a floating point offset in seconds, add that offset and return a
 * new struct timeval adjusted accordingly.
 */
static void timeraddfloat(const struct timeval *const tv, const float delta,
			  struct timeval *const res)
{
        const long ONE_SECOND_US = 1000000;
        float integral;
        long delta_us = (int)(modff(delta, &integral) * (float)ONE_SECOND_US);
        long s = (int)integral;
        long usec = tv->tv_usec + delta_us;
        if (usec > ONE_SECOND_US) {
		usec -= ONE_SECOND_US;
		s += 1;
        }
        res->tv_sec  = tv->tv_sec + s;
        res->tv_usec = usec;
}

/** Subtract two struct timevals.
 *
 * Given two struct timevals, subtract the first from the second and return the
 * result as a float.
 */
static float timersubfloat(const struct timeval start, const struct timeval end)
{
	return (end.tv_sec - start.tv_sec) +
	       (end.tv_usec - start.tv_usec) / 1000000.0;
}

/** Find the given key_code.
 *
 * Linear search the key_tbl and return the index of the given key_code or
 * -1 if it is not found. Since the number of keys in embedded systems is
 *  very small, a linear search is fine.
 */
static int find_key(const int key_code)
{
	for (int i = 0; i < key_tbl_size; ++i)
		if (key_tbl[i].key_code == key_code)
			return i;
	return -1;
}

/** Process a simple (non-timed) action.
 *
 * Starting from the given element in the action table, execute each of the
 * actions, stopping when we get to the end of the array or the action code
 * and/or key code changes.
 *
 * This is called for simple press and release events.
 */
static void process_simple_action(int idx)
{
	if (idx == -1)
		return;

	int action_code = action_tbl[idx].action_code;
	int key_code = action_tbl[idx].key_code;
	while (action_tbl[idx].action_code == action_code &&
	       action_tbl[idx].key_code == key_code && idx < action_tbl_size)
		execute_command(action_tbl[idx++].command);

}

/** Process long press (TIMED) actions.
 *
 * Starting from the given element in the action table, execute each of the
 * long press (TIMED) actions where the time required for the key to be pressed
 * has been reached. Stop when we get to the end of the array, the action code
 * and/or key code changes, or the action time is no longer reached.
 */
static void process_long_press(int idx, const float press_time)
{
	if (idx == -1)
		return;

	int key_code = action_tbl[idx].key_code;
	while (action_tbl[idx].action_code == ACTION_TIMED &&
	       action_tbl[idx].key_code    == key_code     &&
	       action_tbl[idx].time         < press_time   &&
	       idx < action_tbl_size) {
		execute_command(action_tbl[idx].command);
		++idx;
	}
}

/** Insert held_timer (TIMED_FIRE) actions.
 *
 * Given the fields of a TIMED_FIRE action, insert action into held_timer_tbl
 * so that they fire in the future if the key is held down for the required
 * length of time.
 */
static void insert_held_timer(const int action_idx, const int key_idx,
			      const struct timeval *pressed_at)
{
	struct timeval fires_at;
	float press_time = action_tbl[action_idx].time;
	timeraddfloat(pressed_at, press_time, &fires_at);

	int i;
	for (i = 0; i < held_timer_entries; ++i) {
		if (timercmp(&held_timer_tbl[i].fires_at, &fires_at, >)) {
			memmove(&held_timer_tbl[i+1],
				&held_timer_tbl[i],
				(held_timer_entries - i) *
				sizeof(struct held_timer));
			break;
		}
	}

	log_debug("Adding timer for key %d being held down for %g second(s)",
		  key_tbl[key_idx].key_code, press_time);

	held_timer_tbl[i].fires_at = fires_at;
	held_timer_tbl[i].action_idx = action_idx;
	held_timer_tbl[i].key_idx = key_idx;
	held_timer_tbl[i].time = press_time;
	++held_timer_entries;
}

/** Insert TIMED_FIRE timers into the held down key list.
 *
 * Insert any associated TIMED_FIRE timers into the held down timers list for
 * the given key.
 */
static void insert_held_timers(const int key_idx)
{
	if (key_idx == -1)
		return;

	int idx = key_tbl[key_idx].action[ACTION_TIMED_FIRE];
	if (idx == -1)
		return;

	const struct timeval *pressed_at = &key_tbl[key_idx].pressed_at;

	while (action_tbl[idx].action_code == ACTION_TIMED_FIRE &&
	       idx < action_tbl_size) {
		insert_held_timer(idx, key_idx, pressed_at);
		++idx;
	}
}

/** Remove held_timer entry.
 *
 * Remove the given entry from the held_timer_tbl.
 */
static void remove_held_timer(const int i)
{
	log_debug("Removing timer for key %d",
		  key_tbl[held_timer_tbl[i].key_idx].key_code);

	if (i < held_timer_entries - 1)
		memmove(&held_timer_tbl[i], &held_timer_tbl[i+1],
			(held_timer_entries - i - 1) *
			sizeof(struct held_timer));

	--held_timer_entries;
}

/** Remove held timers for the given key.
 *
 * Given an index into the key table, remove all unfired TIMED_FIRE entries
 * from the held timer table.
 */
static void remove_held_timers(const int key_idx)
{
	if (key_idx == -1)
		return;

	for (int i = 0; i < held_timer_entries;) {
		if (held_timer_tbl[i].key_idx == key_idx)
			remove_held_timer(i);
		else
			++i;
	}
}

/** Process pending held timers.
 *
 * Iterate through the held_timer_tbl and identify where the key has been held
 * down long enough to execute the required command. Note, the iteration is
 * achieved by successfully removing processed actions from the head of the
 * array. Returns a struct timeval value that represents how long it will be
 * until the next held timer will fire.
 */
static void process_pending_held_timers(struct timeval *const delay)
{
	struct timeval now;
	gettimeofday(&now, NULL);
	while (held_timer_entries > 0) {
		if (timercmp(&held_timer_tbl[0].fires_at, &now, <=)) {
			int key_code;
			int action_idx;

			// If the held timer is in the past (or now) execute
			// the command, then remove it from the list
			key_code = key_tbl[held_timer_tbl[0].key_idx].key_code;
			action_idx = held_timer_tbl[0].action_idx;

			log_info("Key %d held down for %g seconds", key_code,
				 held_timer_tbl[0].time);

			execute_command(action_tbl[action_idx].command);
			remove_held_timer(0);
		}
		else {
			// If the held timer is in the future, the timeout is
			// calculated to fire then
			timersub(&held_timer_tbl[0].fires_at, &now, delay);
			return;
		}
	}

	// There are no longer any pending held timers
	timerclear(delay);
	return;
}

/** Process key press actions.
 *
 * Record the time the key was pressed, execute any immediate press actions and
 * add a timer for any "key held down" TIMED_FIRE actions.
 */
static void process_key_press(const int key_idx, const int key_code,
			      const struct timeval evtime)
{
#if 0
	// XXX I have not been able create a scenario where there are multiple
	// key press events without a key release so disabling this code.

	// If there is a duplicate key press, warn about it, and remove any
	// pending TIMED_FIRE actions for the key before re-adding them.
	if (timerisset(&key_tbl[key_idx].pressed_at)) {
		log_warning("Duplicate key press for key %d", key_code);
		remove_held_timers(key_idx);
	}
#endif

	// Record time the key was pressed to calculate how long it was pressed
	// later, when it is released
	key_tbl[key_idx].pressed_at = evtime;

	// Process PRESS actions
	log_notice("Key %d pressed, executing actions", key_code);
	process_simple_action(key_tbl[key_idx].action[ACTION_PRESS]);

	// Add timers for "key held down" TIMED_FIRE actions
	insert_held_timers(key_idx);
}

/** Process key release actions.
 *
 * Take the appropriate action when a key/button is released. If we don't have
 * a record of when it was pressed (i.e. prior to the program starting or if
 * we somehow missed the event), then we don't attempt to process any TIMED or
 * TIMED_FIRE actions.
 */
static void process_key_release(const int key_idx, const int key_code,
				const struct timeval evtime)
{
	float hold_time;
	struct timeval pressed_at = key_tbl[key_idx].pressed_at;
	int orphan = !timerisset(&pressed_at);
	if (!orphan) {
		hold_time = timersubfloat(pressed_at, evtime);
		log_info("Key %d released after %g seconds, executing actions",
			 key_code, hold_time);
	}
	else
		log_warning("Key %d released without prior press, executing "
			    "RELEASE actions only", key_code);

	// Process release actions regardless
	process_simple_action(key_tbl[key_idx].action[ACTION_RELEASE]);

	if (!orphan) {
		// Process TIMED actions
		process_long_press(key_tbl[key_idx].action[ACTION_TIMED],
				   hold_time);

		// Remove any unfired "key held down" TIMED_FIRE actions
		remove_held_timers(key_idx);

		// clear pressed_at in case of future stray key release events
		timerclear(&key_tbl[key_idx].pressed_at);
	}
}

/** Process events read from the input device.
 *
 * Use select() to monitor for events arriving on the input device, setting a
 * timeout if there are pending held timers. Fire the appropriate actions when
 * keys are pressed, released, released after a defined length of time or held
 * down for a defined length of time.
 */
static void process_events()
{
	int fd = open(input_device, O_RDONLY | O_NONBLOCK | O_NOCTTY);
	if (fd < 0)
		fatal_error("Failed to open event device '%s'", input_device);

	log_notice("Reading events from '%s'", input_device);

	while (!exit_now) {
		struct input_event ev;
		struct timeval evtime, delay;
		process_pending_held_timers(&delay);

		fd_set rfds;
		FD_ZERO(&rfds);
		FD_SET(fd, &rfds);
		int retval = select(fd+1, &rfds, NULL, NULL,
				    timerisset(&delay) ? &delay : NULL);
		if (retval < 0) {
			if (errno == EINTR)
				continue;
			fatal_error("%s() failed, exiting: %m", "select");
		}
		else if (retval == 0)
			continue;

		int got = read(fd, &ev, sizeof(ev));
		if (got != sizeof(struct input_event)) {
			if (got == -1 && errno == EAGAIN)
				continue;
			fatal_error("%s() failed, exiting: %m", "read");
		}

		if (ev.type != EV_KEY)
			continue;

		// Create proper struct timeval from the event time data
		evtime.tv_sec = ev.tv_sec;
		evtime.tv_usec = ev.tv_usec;

		int key_code = ev.code;
		int key_idx = find_key(key_code);
		if (key_idx == -1) {
			log_debug("ignoring key %d value %d", key_code,
				  ev.value);
			continue;
		}

		switch (ev.value) {
		case 1: process_key_press(key_idx, key_code, evtime);
			break;

		case 0: process_key_release(key_idx, key_code, evtime);
			break;
		}
	}

	log_notice("SIGTERM received, exiting");
	(void)close(fd);
}

/** SIGTERM signal handler.
 *
 * Set exit_now to 1, process_events() will do the rest.
 */
void handle_SIGTERM(__attribute__((unused)) int sig)
{
	exit_now = 1;
}

/** Initialise signal handling.
 *
 * Initialise signal handlers:
 * - Graceful exit on reception of SIGTERM;
 * - Ignore SIGCHLD to allow child processes to be automatically reaped;
 * - If running as a daemon, ignore SIGHUP.
 */
static void init_signal_handling()
{
	(void)signal(SIGTERM, handle_SIGTERM);
	(void)signal(SIGCHLD, SIG_IGN);
	if (run_as_daemon)
		(void)signal(SIGHUP, SIG_IGN);
}

/** Daemonize the process.
 *
 * Perform all the necessary actions to become a daemon process as documented
 * in the Linux daemon(7) manual page section on SysV daemons.
 */
static void daemonize()
{
	// Errors should be extremely rare so if anything goes wrong, just exit

	// 1. Close all open file descriptors except stdin, stdout, and stderr
	struct rlimit rl;
	getrlimit(RLIMIT_NOFILE, &rl);
	for (int fd = 3; fd < (int)rl.rlim_max; ++fd)
		(void)close(fd);

	// 2. Reset all signal handlers to their default
	for (int sig = 1; sig < _NSIG; ++sig)
		(void)signal(sig, SIG_DFL);

	// 3. Reset the signal mask to the empty set
	sigset_t ss;
	(void)sigemptyset(&ss);
	(void)sigprocmask(SIG_SETMASK, &ss, NULL);

	// 4. Sanitise the environment
	// TODO

	// 5. Create a background process
	pid_t pid = fork();
	if (pid < 0)
		exit(EXIT_FAILURE);

	if (pid > 0) {
		// 6. Detach from any terminal to create session leader
		(void)setsid();

		// 7. Call fork() again, to prevent re-acquiring a controlling
		//    terminal
		pid = fork();
		if (pid < 0)
			exit(EXIT_FAILURE);

		// 8. Call exit() in the first child
		if (pid > 0)
			exit(EXIT_SUCCESS);

		// 9. Connect /dev/null to standard input, output, and error
		for (int fd = 0; fd < 3; ++fd)
			close(fd);
		int fd = open("/dev/null", O_RDWR | O_NOCTTY, 0644);
		if (fd != STDIN_FILENO)
			exit(EXIT_FAILURE);
		if (dup2(STDIN_FILENO, STDOUT_FILENO) != STDOUT_FILENO)
			exit(EXIT_FAILURE);
		if (dup2(STDIN_FILENO, STDERR_FILENO) != STDERR_FILENO)
			exit(EXIT_FAILURE);

		// 10. Reset the umask to 0
		umask(0);

		// 11. Avoid holding open other mounted filesystems
		if (chdir("/") < 0)
			exit(EXIT_FAILURE);

		// 12. Write the daemon PID to a file
		// TODO

		// 13. Drop privileges, if appropriate
		// TODO (a bit difficult as we are running scripts as root)

		// 14. Notify the original process initialisation is complete
		// TODO
	}
	else {
		// 15. Call exit() in the original process, after receiving
		//     confirmation the daemon process has initialised
		// TODO (the second part)
		exit(EXIT_SUCCESS);
	}
}

/** Display usage message.
 */
static void usage()
{
	fprintf(stderr, "Usage: %s [-s] [-d] [-c <config_file>] "
			"[<input_device>]\n", program_name);
	exit(EXIT_FAILURE);
}

/** Parse arguments.
 */
static void parse_arguments(const int argc, char *const argv[])
{
	int opt;

	program_name = basename(argv[0]);

	while ((opt = getopt(argc, argv, "hdsc:")) != -1) {
		switch (opt) {
			case 'd':
				run_as_daemon = 1;
				// fall through
			case 's':
				log_to_syslog = 1;
				break;
			case 'c':
				config_file = optarg;
				break;
			default: /* '?' or 'h' */
				usage();
		}
	}

	// There should not be more than one argument remaining
	if (optind < argc - 1)
		usage();

	// If there is an argument remaining, it is the input device file
	if (optind < argc)
		cmdline_device = strdup(argv[optind]);
}

/** main function.
 */
int main(const int argc, char *const argv[])
{
	parse_arguments(argc, argv);

	if (run_as_daemon)
		daemonize();

	if (log_to_syslog) {
		openlog(program_name, LOG_PID | LOG_CONS, LOG_DAEMON);
		setlogmask(LOG_UPTO(LOG_DEBUG));
		log_activated = 1;
	}

	load_config();

	init_signal_handling();

	process_events();

	if (log_to_syslog)
		closelog();

	return 0;
}
