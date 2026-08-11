#include "apex-manager.h"

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *stream)
{
	fprintf(stream,
		"Usage: droidian-apex [OPTIONS] COMMAND [ARGS...]\n"
		"\n"
		"Manage Android APEXs for Droidian's Halium container.\n"
		"\n"
		"Commands:\n"
		"  list [--json] [ROOT...]       Find APEXs without contacting apexd\n"
		"  mount [--all | PATH...]       Mount selected or all discovered APEXs\n"
		"  prepare                       Alias for 'mount --all'\n"
		"  wait [started|activated|ready] Wait for apexd (default: ready)\n"
		"  status                        Show container and apexd state\n"
		"  sync                          Mirror apexd's live mounts to the host\n"
		"  apexd status|wait|dump        Interact with apexd in the container\n"
		"  diagnose                      Print porting and boot diagnostics\n"
		"\n"
		"Options:\n"
		"  -a, --android-root PATH  Android partition root (default: /android)\n"
		"  -m, --mount-root PATH    Host APEX mount root (default: /apex)\n"
		"  -c, --container NAME     LXC container name (default: android)\n"
		"  -t, --timeout SECONDS    Wait timeout (default: 60)\n"
		"  -n, --dry-run            Report mount operations without performing them\n"
		"  -v, --verbose            Print progress details\n"
		"  -V, --version            Print version\n"
		"  -h, --help               Show this help\n");
}

static int operation_error(const char *operation)
{
	apex_log("error", "%s failed: %s", operation, strerror(errno));
	return 1;
}

static int command_list(const struct apex_options *options, int argc, char **argv)
{
	struct apex_list list;
	bool json = false;
	int result;

	if (argc && strcmp(argv[0], "--json") == 0) {
		json = true;
		argc--;
		argv++;
	}
	apex_list_init(&list);
	result = apex_discover(options, argc, argv, &list);
	if (result == 0)
		result = apex_print_list(&list, json);
	apex_list_free(&list);
	return result < 0 ? operation_error("APEX discovery") : 0;
}

static int command_mount(const struct apex_options *options, int argc, char **argv)
{
	if (argc && strcmp(argv[0], "--all") == 0) {
		argc--;
		argv++;
	}
	return apex_mount_paths(options, argc, argv) < 0 ?
		operation_error("APEX mount") : 0;
}

static int command_status(const struct apex_options *options)
{
	char state[64];
	char status[64];
	pid_t pid;
	bool failed = false;

	if (container_get_state(options, state, sizeof(state)) == 0)
		printf("container.state=%s\n", state);
	else {
		printf("container.state=unavailable\n");
		failed = true;
	}
	if (container_get_pid(options, &pid) == 0)
		printf("container.pid=%ld\n", (long)pid);
	else {
		printf("container.pid=unavailable\n");
		failed = true;
	}
	if (apexd_get_status(options, status, sizeof(status)) == 0)
		printf("apexd.status=%s\n", status[0] ? status : "unset");
	else {
		printf("apexd.status=unavailable\n");
		failed = true;
	}
	return failed ? 1 : 0;
}

static int command_apexd(const struct apex_options *options, int argc, char **argv)
{
	if (!argc) {
		apex_log("error", "apexd requires status, wait, or dump");
		return 2;
	}
	if (strcmp(argv[0], "status") == 0)
		return command_status(options);
	if (strcmp(argv[0], "wait") == 0) {
		const char *wanted = argc > 1 ? argv[1] : "ready";

		return apexd_wait(options, wanted) < 0 ? 1 : 0;
	}
	if (strcmp(argv[0], "dump") == 0)
		return apexd_dump(options, argc - 1, argv + 1);
	apex_log("error", "unknown apexd operation '%s'", argv[0]);
	return 2;
}

int main(int argc, char **argv)
{
	static const struct option long_options[] = {
		{ "android-root", required_argument, NULL, 'a' },
		{ "mount-root", required_argument, NULL, 'm' },
		{ "container", required_argument, NULL, 'c' },
		{ "timeout", required_argument, NULL, 't' },
		{ "dry-run", no_argument, NULL, 'n' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "version", no_argument, NULL, 'V' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	struct apex_options options = {
		.android_root = getenv("DROIDIAN_APEX_ANDROID_ROOT"),
		.mount_root = getenv("DROIDIAN_APEX_MOUNT_ROOT"),
		.container = getenv("DROIDIAN_APEX_CONTAINER"),
		.timeout = 60,
	};
	const char *timeout_environment = getenv("DROIDIAN_APEX_TIMEOUT");
	char *end;
	unsigned long parsed;
	int option;
	const char *command;

	if (!options.android_root || !options.android_root[0])
		options.android_root = "/android";
	if (!options.mount_root || !options.mount_root[0])
		options.mount_root = "/apex";
	if (!options.container || !options.container[0])
		options.container = "android";
	if (timeout_environment && timeout_environment[0]) {
		errno = 0;
		parsed = strtoul(timeout_environment, &end, 10);
		if (!errno && end && !*end && parsed > 0 && parsed <= 3600)
			options.timeout = (unsigned int)parsed;
	}
	while ((option = getopt_long(argc, argv, "+a:m:c:t:nvVh",
			long_options, NULL)) != -1) {
		switch (option) {
		case 'a': options.android_root = optarg; break;
		case 'm': options.mount_root = optarg; break;
		case 'c': options.container = optarg; break;
		case 't':
			errno = 0;
			parsed = strtoul(optarg, &end, 10);
			if (errno || !end || *end || parsed == 0 || parsed > 3600) {
				apex_log("error", "timeout must be between 1 and 3600 seconds");
				return 2;
			}
			options.timeout = (unsigned int)parsed;
			break;
		case 'n': options.dry_run = true; break;
		case 'v': options.verbose = true; break;
		case 'V': printf("droidian-apex %s\n", APEX_VERSION); return 0;
		case 'h': usage(stdout); return 0;
		default: usage(stderr); return 2;
		}
	}
	apex_verbose = options.verbose;
	if (optind >= argc) {
		usage(stderr);
		return 2;
	}
	command = argv[optind++];
	argc -= optind;
	argv += optind;
	if (strcmp(command, "list") == 0 || strcmp(command, "scan") == 0)
		return command_list(&options, argc, argv);
	if (strcmp(command, "mount") == 0 || strcmp(command, "prepare") == 0)
		return command_mount(&options, argc, argv);
	if (strcmp(command, "wait") == 0) {
		const char *wanted = argc ? argv[0] : "ready";

		return apexd_wait(&options, wanted) < 0 ? 1 : 0;
	}
	if (strcmp(command, "status") == 0)
		return command_status(&options);
	if (strcmp(command, "sync") == 0)
		return apex_sync(&options) < 0 ? operation_error("APEX sync") : 0;
	if (strcmp(command, "apexd") == 0)
		return command_apexd(&options, argc, argv);
	if (strcmp(command, "diagnose") == 0 || strcmp(command, "diagnostics") == 0)
		return apex_diagnose(&options);
	apex_log("error", "unknown command '%s'", command);
	usage(stderr);
	return 2;
}
