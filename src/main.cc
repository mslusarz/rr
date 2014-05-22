/* -*- Mode: C++; tab-width: 8; c-basic-offset: 8; indent-tabs-mode: t; -*- */

#include <assert.h>
#include <dlfcn.h>
#include <getopt.h>
#include <linux/version.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sysexits.h>
#include <sys/prctl.h>
#include <sys/utsname.h>
#include <sys/wait.h>

#include <limits>
#include <sstream>

#include "preload/syscall_buffer.h"

#include "config.h"
#include "hpc.h"
#include "log.h"
#include "recorder.h"
#include "recorder_sched.h"
#include "replayer.h"
#include "task.h"
#include "trace.h"
#include "util.h"

using namespace std;

extern char** environ;

/**
 * Dump all events from the current to trace that match |spec| to
 * |out|.  |spec| has the following syntax: /\d+(-\d+)?/, expressing
 * either a single event number of a range, and may be null to
 * indicate "dump all events".
 *
 * This function is side-effect-y, in that the trace file isn't
 * rewound in between matching each spec.  Therefore specs should be
 * constructed so as to match properly on a serial linear scan; that
 * is, they should comprise disjoint and monotonically increasing
 * event sets.  No attempt is made to enforce this or normalize specs.
 */
static void dump_events_matching(TraceIfstream& trace,
				 FILE* out, const char* spec)
{
	
	uint32_t start = 0, end = numeric_limits<uint32_t>::max();

	// Try to parse the "range" syntax '[start]-[end]'.
	if (spec && 2 > sscanf(spec, "%u-%u", &start, &end)) {
		// Fall back on assuming the spec is a single event
		// number, however it parses out with atoi().
		start = end = atoi(spec);
	}

	while (trace.good()) {
		struct trace_frame frame;
		trace >> frame;
		if (end < frame.global_time) {
			return;
		}
		if (start <= frame.global_time && frame.global_time <= end) {
			frame.dump(out, rr_flags()->raw_dump);
		}
	}
}

static void start_dumping(int argc, char* argv[], char** envp)
{
	FILE* out = stdout;
	auto trace = TraceIfstream::open(argc, argv);

	fprintf(out,
		"global_time thread_time tid reason entry/exit "
		"hw_interrupts page_faults adapted_rbc instructions "
		"eax ebx ecx edx esi edi ebp orig_eax esp eip eflags\n");

	if (1 == argc) {
		// No specs => dump all events.
		return dump_events_matching(*trace, stdout,
					    nullptr/*all events*/);
	}
	for (int i = 1; i < argc; ++i) {
		dump_events_matching(*trace, stdout, argv[i]);
	}
}

static void start(const char* rr_exe, int argc, char* argv[], char** envp)
{

	switch (rr_flags()->option) {
	case RECORD:
		return record(rr_exe, argc, argv, envp);
	case REPLAY:
		return replay(argc, argv, envp);
	case DUMP_EVENTS:
		return start_dumping(argc, argv, envp);
	default:
		FATAL() <<"Uknown option "<< rr_flags()->option;
	}
}

/**
 * Open |filename| and scan it as if it contains a single integer
 * value.  Return the integer on success, or -1 on failure.
 */
static int read_int_file(const char* filename)
{
	FILE* inf = fopen(filename, "r");
	int val;
	if (!inf) {
		return -1;
	}
	if (1 != fscanf(inf, "%d", &val)) {
		FATAL() <<"Failed to scan integer from " << filename;
	}
	fclose(inf);
	return val;
}

static void assert_prerequisites(struct flags* flags)
{
	int ptrace_scope_val =
		read_int_file("/proc/sys/kernel/yama/ptrace_scope");
	if (ptrace_scope_val > 0) {
		FATAL() <<"Can't write to process memory; ptrace_scope is "
			<< ptrace_scope_val;
	}

	struct utsname uname_buf;
	memset(&uname_buf, 0, sizeof(uname_buf));
	if (!uname(&uname_buf)) {
		unsigned int major, minor;
		char dot;
		stringstream stream(uname_buf.release);
		stream >> major >> dot >> minor;
		if (KERNEL_VERSION(major, minor, 0) < KERNEL_VERSION(3, 4, 0)) {
			FATAL() << "Kernel doesn't support necessary ptrace "
				<< "functionality; need 3.4.0 or better.";
		}

		if (flags->use_syscall_buffer &&
		    KERNEL_VERSION(major, minor, 0) < KERNEL_VERSION(3, 5, 0)) {
			FATAL() << "Your kernel does not support syscall "
				<< "filtering; please use the -n option";
		}
	}
}

static void check_performance_settings()
{
	// NB: we hard-code "cpu0" here because rr pins itself and all
	// tracees to cpu 0.  We don't care about the other CPUs.
	int fd = open("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor",
		      O_RDONLY);
	if (0 > fd) {
		// If the file doesn't exist, the system probably
		// doesn't have the ability to frequency-scale, for
		// example a VM.
		LOG(warn) <<"Unable to check CPU-frequency governor.";
		return;
	}
	char governor[PATH_MAX];
	ssize_t nread = read(fd, governor, sizeof(governor) - 1);
	if (0 > nread) {
		FATAL() <<"Unable to read cpu0's frequency governor.";
	}
	governor[nread] = '\0';
	ssize_t len = strlen(governor);
	if (len > 0) {
		// Eat the '\n'.
		governor[len - 1] = '\0';
	}
	LOG(info) <<"cpu0's frequency governor is '"<< governor <<"'";
	if (strcmp("performance", governor)) {
		fprintf(stderr,
"\n"
"rr: Warning: Your CPU frequency governor is '%s'.  rr strongly\n"
"    recommends that you use the 'performance' governor.  Not using the\n"
"    'performance' governor can cause rr to be at least 2x slower\n"
"    on laptops.\n"
"\n"
"    On Fedora-based systems, you can enable the 'performance' governor\n"
"    by running the following commands:\n"
"\n"
"    $ sudo yum install kernel-tools\n"
"    $ sudo cpupower frequency-set -g performance\n"
"\n",
			governor);
		// TODO: It would be nice to bail here or do something
		// clever to enable the 'performance' just for rr, but
		// that seems too hard at the moment.
	}
}

static void print_usage(void)
{
	fputs(
"Usage: rr [OPTION] (record|replay|dump) [OPTION]... [ARG]...\n"
"\n"
"Common options\n"
"  -c, --checksum={on-syscalls,on-all-events}|FROM_TIME\n"
"                             compute and store (during recording) or\n"
"                             read and verify (during replay) checksums\n"
"                             of each of a tracee's memory mappings either\n"
"                             at the end of all syscalls (`on-syscalls'),\n"
"                             at all events (`on-all-events'), or \n"
"                             starting from a global timepoint FROM_TIME\n"
"  -d, --dump-on=<SYSCALL_NUM|-SIGNAL_NUM>\n"
"                             dump memory at SYSCALL or SIGNAL to the\n"
"                             file `[trace_dir]/[tid].[time]_{rec,rep}':\n"
"                             `_rec' for dumps during recording, `_rep'\n"
"                             for dumps during replay\n"
"  -f, --force-enable-debugger\n"
"                             always allow emergency debugging, even\n"
"                             when it doesn't seem like a good idea, for\n"
"                             example if stderr isn't a tty.\n"
"  -k, --check-cached-mmaps   verify that cached task mmaps match /proc/maps\n"
"  -m, --mark-stdio           mark stdio writes with [rr.<EVENT-NO>],\n"
"                             where EVENT-NO is the global trace time at\n"
"                             which the write occures.\n"
"  -s, --suppress-performance-warnings\n"
"                             suppress warnings related to potential\n"
"                             performance issues\n"
"  -t, --dump-at=TIME         dump memory at global timepoint TIME\n"
"  -u, --cpu-unbound          allow tracees to run on any virtual CPU.\n"
"                             Default is to bind to CPU 0.  This option\n"
"                             can cause replay divergence: use with\n"
"                             caution.\n"
"  -v, --verbose              log messages that may not be urgently \n"
"                             critical to the user\n"
"  -w, --wait-secs=<NUM_SECS> wait NUM_SECS seconds just after startup,\n"
"                             before initiating recording or replaying\n"
"\n"
"Syntax for `record'\n"
" rr record [OPTION]... <exe> [exe-args]...\n"
"  -b, --force-syscall-buffer force the syscall buffer preload library\n"
"                             to be used, even if that's probably a bad\n"
"                             idea\n"
"  -c, --num-cpu-ticks=<NUM>  maximum number of 'CPU ticks' (currently \n"
"                             retired conditional branches) to allow a \n"
"                             task to run before interrupting it\n"
"  -e, --num-events=<NUM>     maximum number of events (syscall \n"
"                             enter/exit, signal, CPU interrupt, ...) \n"
"                             to allow a task before descheduling it\n"
"  -i, --ignore-signal=<SIG>  block <SIG> from being delivered to tracees.\n"
"                             Probably only useful for unit tests.\n"
"  -n, --no-syscall-buffer    disable the syscall buffer preload library\n"
"                             even if it would otherwise be used\n"
"\n"
"Syntax for `replay'\n"
" rr replay [OPTION]... [<trace-dir>]\n"
"  -a, --autopilot            replay without debugger server\n"
"  -f, --onfork=<PID>         start a debug server when <PID> has been\n"
"                             fork()d, AND the target event has been\n"
"                             reached.\n"
"  -g, --goto=<EVENT-NUM>     start a debug server on reaching <EVENT-NUM>\n"
"                             in the trace.  See -m above.\n"
"  -p, --onprocess=<PID>      start a debug server when <PID> has been\n"
"                             exec()d, AND the target event has been\n"
"                             reached.\n"
"  -q, --no-redirect-output   don't replay writes to stdout/stderr\n"
"  -s, --dbgport=<PORT>       only start a debug server on <PORT>;\n"
"                             don't automatically launch the debugger\n"
"                             client too.\n"
"  -x, --gdb-x=<FILE>         execute gdb commands from <FILE>\n"
"\n"
"Syntax for `dump`\n"
" rr dump [OPTIONS] <trace_dir> [<event-spec>...]\n"
"  Event specs can be either an event number like `127', or a range\n"
"  like `1000-5000'.  By default, all events are dumped.\n"
"  -r, --raw                  dump trace frames in a more easily\n"
"                             machine-parseable format instead of the\n"
"                             default human-readable format\n"
"\n"
"A command line like `rr (-h|--help|help)...' will print this message.\n"
, stderr);
}

static int parse_record_args(int cmdi, int argc, char** argv,
			     struct flags* flags)
{
	struct option opts[] = {
		{ "force-syscall-buffer", no_argument, NULL, 'b' },
		{ "ignore-signal", required_argument, NULL, 'i' },
		{ "num-cpu-ticks", required_argument, NULL, 'c' },
		{ "num-events", required_argument, NULL, 'e' },
		{ "no-syscall-buffer", no_argument, NULL, 'n' },
		{ 0 }
	};
	optind = cmdi + 1;
	while (1) {
		int i = 0;
		switch (getopt_long(argc, argv, "+c:be:i:n", opts, &i)) {
		case -1:
			return optind;
		case 'b':
			flags->use_syscall_buffer = true;
			break;
		case 'c':
			flags->max_rbc = MAX(1, atoi(optarg));
			break;
		case 'e':
			flags->max_events = MAX(1, atoi(optarg));
			break;
		case 'i':
			flags->ignore_sig = MIN(_NSIG - 1,
						MAX(1, atoi(optarg)));
			break;
		case 'n':
			flags->use_syscall_buffer = false;
			break;
		default:
			return -1;
		}
	}
}

static int parse_replay_args(int cmdi, int argc, char** argv,
			     struct flags* flags)
{
	struct option opts[] = {
		{ "autopilot", no_argument, NULL, 'a' },
		{ "dbgport", required_argument, NULL, 's' },
		{ "goto", required_argument, NULL, 'g' },
		{ "no-redirect-output", no_argument, NULL, 'q' },
		{ "onfork", required_argument, NULL, 'f' },
		{ "onprocess", required_argument, NULL, 'p' },
		{ "gdb-x", required_argument, NULL, 'x' },
		{ 0 }
	};
	optind = cmdi + 1;
	while (1) {
		int i = 0;
		switch (getopt_long(argc, argv, "+af:g:p:qs:x:", opts, &i)) {
		case -1:
			return optind;
		case 'a':
			flags->goto_event = numeric_limits<decltype(
				flags->goto_event)>::max();
			flags->dont_launch_debugger = true;
			break;
		case 'f':
			flags->target_process = atoi(optarg);
			flags->process_created_how = CREATED_FORK;
			break;
		case 'g':
			flags->goto_event = atoi(optarg);
			break;
		case 'p':
			flags->target_process = atoi(optarg);
			flags->process_created_how = CREATED_EXEC;
			break;
		case 'q':
			flags->redirect = false;
			break;
		case 's':
			flags->dbgport = atoi(optarg);
			flags->dont_launch_debugger = true;
			break;
		case 'x':
			flags->gdb_command_file_path = optarg;
			break;
		default:
			return -1;
		}
	}
}

static int parse_dump_args(int cmdi, int argc, char** argv,
			   struct flags* flags)
{
	struct option opts[] = {
		{ "raw", no_argument, NULL, 'r' },
	};
	optind = cmdi + 1;
	while (1) {
		int i = 0;
		switch (getopt_long(argc, argv, "r", opts, &i)) {
		case -1:
			return optind;
		case 'r':
			flags->raw_dump = true;
			break;
		default:
			return -1;
		}
	}
}

static int parse_common_args(int argc, char** argv, struct flags* flags)
{
	struct option opts[] = {
		{ "checksum", required_argument, NULL, 'c' },
		{ "check-cached-mmaps", no_argument, NULL, 'k' },
		{ "cpu-unbound", no_argument, NULL, 'u' },
		{ "dump-at", required_argument, NULL, 't' },
		{ "dump-on", required_argument, NULL, 'd' },
		{ "force-enable-debugger", no_argument, NULL, 'f' },
		{ "mark-stdio", no_argument, NULL, 'm' },
		{ "suppress-performance-warnings", no_argument, NULL, 's' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "wait-secs", required_argument, NULL, 'w' },
		{ 0 }
	};
	while (1) {
		int i = 0;
		switch (getopt_long(argc, argv, "+c:d:fkmst:uvw:", opts, &i)) {
		case -1:
			return optind;
		case 'c':
			if (!strcmp("on-syscalls", optarg)) {
				LOG(info) <<"checksumming on syscall exit";
				flags->checksum = CHECKSUM_SYSCALL;
			} else if (!strcmp("on-all-events", optarg)) {
				LOG(info) <<"checksumming on all events";
				flags->checksum = CHECKSUM_ALL;
			} else {
				flags->checksum = atoi(optarg);
				LOG(info) <<"checksumming on at event "
					  << flags->checksum;
			}
			break;
		case 'd':
			flags->dump_on = atoi(optarg);
			break;
		case 'f':
			flags->force_enable_debugger = true;
			break;
		case 'k':
			flags->check_cached_mmaps = true;
			break;
		case 'm':
			flags->mark_stdio = true;
			break;
		case 's':
			flags->suppress_performance_warnings = true;
			break;
		case 't':
			flags->dump_at = atoi(optarg);
			break;
		case 'u':
			flags->cpu_unbound = true;
			break;
		case 'v':
			flags->verbose = true;
			break;
		case 'w':
			flags->wait_secs = atoi(optarg);
			break;
		default:
			return -1;
		}
	}

}

static int parse_args(int argc, char** argv, struct flags* flags)
{
	const char* exe = argv[0];
	const char* cmd;
	int cmdi;

	flags->max_rbc = DEFAULT_MAX_RBC;
	flags->max_events = DEFAULT_MAX_EVENTS;
	flags->checksum = CHECKSUM_NONE;
	flags->dbgport = -1;
	flags->dump_at = DUMP_AT_NONE;
	flags->dump_on = DUMP_ON_NONE;
	flags->redirect = true;
	flags->use_syscall_buffer = true;
	flags->suppress_performance_warnings = false;

	if (0 > (cmdi = parse_common_args(argc, argv, flags))) {
		return -1;
	}
	if (0 == cmdi || cmdi >= argc) {
		fprintf(stderr, "%s: must specify a command\n", exe);
		return -1;
	}

	cmd = argv[cmdi];
	if (!strcmp("record", cmd)) {
		flags->option = RECORD;
		return parse_record_args(cmdi, argc, argv, flags);
	}
	if (!strcmp("replay", cmd)) {
		flags->option = REPLAY;
		return parse_replay_args(cmdi, argc, argv, flags);
	}
	if (!strcmp("dump", cmd)) {
		flags->option = DUMP_EVENTS;
		return parse_dump_args(cmdi, argc, argv, flags);
	}
	if (!strcmp("help", cmd) || !strcmp("-h", cmd)
	    || !strcmp("--help", cmd)) {
		return -1;
	}
	fprintf(stderr, "%s: unknown command `%s`\n", exe, cmd);
	return -1;
}

static string find_syscall_buffer_library()
{
	char* exe_path = realpath("/proc/self/exe", NULL);
	string lib_path = exe_path;
	free(exe_path);

	int end = lib_path.length();
	// Chop off the filename
	while (end > 0 && lib_path[end - 1] != '/') {
		--end;
	}
	lib_path.erase(end);
	lib_path += "../lib/" SYSCALLBUF_LIB_FILENAME;
	if (access(lib_path.c_str(), F_OK) != 0) {
		// File does not exist. Assume install put it in LD_LIBRARY_PATH.
		lib_path = SYSCALLBUF_LIB_FILENAME;
	}
	return lib_path;
}

int main(int argc, char* argv[])
{
	int argi;		/* index of first positional argument */
	int wait_secs;
	struct flags* flags = rr_flags_for_init();

	if (argc >= 2 && !strcmp("check-preload-lib", argv[1])) {
		// If we reach here and we were checking the preload
		// lib, then it didn't load --- its __constructor__
		// function didn't run.
		_exit(EX_CONFIG);
	}

	if (0 > (argi = parse_args(argc, argv, flags))
	    || argc < argi
	    // |rr replay| is allowed to have no arguments to replay
	    // the most recently saved trace.
	    || (REPLAY != flags->option && argc <= argi)) {
		print_usage();
		return 1;
	}

	assert_prerequisites(flags);
	if (!flags->suppress_performance_warnings) {
		check_performance_settings();
	}

	wait_secs = flags->wait_secs;
	if (wait_secs > 0) {
		struct timespec ts;
		ts.tv_sec = wait_secs;
		ts.tv_nsec = 0;
		LOG(info) <<"Waiting "<< wait_secs <<" seconds before continuing ...";
		if (nanosleep_nointr(&ts)) {
			FATAL() <<"Failed to wait requested duration";
		}
		LOG(info) <<"... continuing.";
	}

	if (!rr_flags()->cpu_unbound) {
		cpu_set_t mask;
		// Pin tracee tasks to logical CPU 0, both in
		// recording and replay.  Tracees can see which HW
		// thread they're running on by asking CPUID, and we
		// don't have a way to emulate it yet.  So if a tracee
		// happens to be scheduled on a different core in
		// recording than replay, it can diverge.  (And
		// indeed, has been observed to diverge in practice,
		// in glibc.)
		//
		// Note that this pins both the tracee processes *and*
		// the tracer procer.  This ends up being a tidy
		// performance win in certain circumstances,
		// presumably due to cheaper context switching and/or
		// better interaction with CPU frequency scaling.
		CPU_ZERO(&mask);
		CPU_SET(0, &mask);
		if (0 > sched_setaffinity(0, sizeof(mask), &mask)) {
			FATAL() <<"Couldn't bind to CPU 0";
		}
	}

	if (RECORD == flags->option) {
		LOG(info) << "Scheduler using max_events="<< flags->max_events
			  <<", max_rbc="<< flags->max_rbc;

		// The syscallbuf library interposes some critical
		// external symbols like XShmQueryExtension(), so we
		// preload it whether or not syscallbuf is enabled.
		if (flags->use_syscall_buffer) {
			setenv(SYSCALLBUF_ENABLED_ENV_VAR, "1", 1);
		} else {
			LOG(info) <<"Syscall buffer disabled by flag";
			unsetenv(SYSCALLBUF_ENABLED_ENV_VAR);
		}
		flags->syscall_buffer_lib_path = find_syscall_buffer_library();
	}

	start(argv[0], argc - argi , argv + argi, environ);

	return 0;

}
