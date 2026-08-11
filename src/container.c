#include "apex-manager.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int container_get_state(const struct apex_options *options, char *state,
		size_t state_size)
{
	char *arguments[] = {
		"lxc-info", "-n", (char *)options->container, "-sH", NULL
	};
	int exit_status;

	if (run_capture(arguments, state, state_size, &exit_status) < 0)
		return -1;
	if (exit_status != 0) {
		errno = ESRCH;
		return -1;
	}
	return 0;
}

int container_get_pid(const struct apex_options *options, pid_t *pid)
{
	char output[64];
	char *end = NULL;
	long parsed;
	char *arguments[] = {
		"lxc-info", "-n", (char *)options->container, "-pH", NULL
	};
	int exit_status;

	if (run_capture(arguments, output, sizeof(output), &exit_status) < 0)
		return -1;
	if (exit_status != 0) {
		errno = ESRCH;
		return -1;
	}
	errno = 0;
	parsed = strtol(output, &end, 10);
	if (errno || !end || *end || parsed <= 0 || parsed > INT_MAX) {
		errno = EINVAL;
		return -1;
	}
	*pid = (pid_t)parsed;
	return 0;
}

int apexd_get_status(const struct apex_options *options, char *status,
		size_t status_size)
{
	char *arguments[] = {
		"lxc-attach", "-n", (char *)options->container, "--",
		"/system/bin/getprop", "apexd.status", NULL
	};
	int exit_status;

	if (run_capture(arguments, status, status_size, &exit_status) < 0)
		return -1;
	if (exit_status != 0) {
		errno = EIO;
		return -1;
	}
	return 0;
}

static int status_rank(const char *status)
{
	if (strcmp(status, "starting") == 0)
		return 1;
	if (strcmp(status, "activated") == 0)
		return 2;
	if (strcmp(status, "ready") == 0)
		return 3;
	return 0;
}

int apexd_wait(const struct apex_options *options, const char *wanted)
{
	struct timespec start;
	struct timespec now;
	struct timespec pause = { .tv_sec = 0, .tv_nsec = 250000000 };
	int wanted_rank;
	char last_status[64] = "";
	char last_state[64] = "";

	if (strcmp(wanted, "started") == 0 || strcmp(wanted, "starting") == 0)
		wanted_rank = 1;
	else if (strcmp(wanted, "activated") == 0)
		wanted_rank = 2;
	else if (strcmp(wanted, "ready") == 0)
		wanted_rank = 3;
	else {
		apex_log("error", "invalid apexd state '%s'", wanted);
		errno = EINVAL;
		return -1;
	}
	if (clock_gettime(CLOCK_MONOTONIC, &start) < 0)
		return -1;
	for (;;) {
		char state[64];
		char status[64];

		if (container_get_state(options, state, sizeof(state)) == 0) {
			if (strcmp(last_state, state) != 0) {
				apex_debug("container %s state: %s", options->container, state);
				snprintf(last_state, sizeof(last_state), "%s", state);
			}
			if (strcmp(state, "RUNNING") == 0 &&
			    apexd_get_status(options, status, sizeof(status)) == 0) {
				if (strcmp(last_status, status) != 0) {
					apex_debug("apexd.status: %s",
						status[0] ? status : "(unset)");
					snprintf(last_status, sizeof(last_status), "%s", status);
				}
				if (status_rank(status) >= wanted_rank)
					return 0;
			}
		}
		if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
			return -1;
		if ((uint64_t)(now.tv_sec - start.tv_sec) >= options->timeout) {
			apex_log("error", "timed out after %u seconds waiting for "
				"apexd to become %s (container=%s, apexd.status=%s)",
				options->timeout, wanted,
				last_state[0] ? last_state : "unavailable",
				last_status[0] ? last_status : "unset");
			errno = ETIMEDOUT;
			return -1;
		}
		while (nanosleep(&pause, &pause) < 0 && errno == EINTR)
			;
		pause.tv_sec = 0;
		pause.tv_nsec = 250000000;
	}
}

static int run_dumpsys(const struct apex_options *options, int argc, char **argv)
{
	char **arguments;
	int index;
	int result;

	arguments = calloc((size_t)argc + 8, sizeof(*arguments));
	if (!arguments)
		return -1;
	arguments[0] = "lxc-attach";
	arguments[1] = "-n";
	arguments[2] = (char *)options->container;
	arguments[3] = "--";
	arguments[4] = "/system/bin/dumpsys";
	arguments[5] = "apexservice";
	for (index = 0; index < argc; index++)
		arguments[index + 6] = argv[index];
	result = run_passthrough(arguments);
	free(arguments);
	if (result == 126 || result == 127)
		apex_log("error", "could not run dumpsys in container %s",
			options->container);
	return result;
}

static bool dump_has_active_packages(const char *dump)
{
	const char *packages = strstr(dump, "ACTIVE PACKAGES:");

	if (!packages)
		return false;
	packages += strlen("ACTIVE PACKAGES:");
	while (*packages == ' ' || *packages == '\t' ||
	       *packages == '\r' || *packages == '\n')
		packages++;
	return *packages && strncmp(packages, "SESSIONS:", strlen("SESSIONS:")) != 0;
}

static size_t print_live_packages(const struct apex_list *list)
{
	size_t index;
	size_t printed = 0;
	int version_width = 8;
	char last_name[APEX_NAME_MAX + 1] = "";
	uint64_t last_version = 0;
	bool have_last = false;

	for (index = 0; index < list->count; index++) {
		int width;

		if (!list->items[index].name[0] || list->items[index].error[0])
			continue;
		width = snprintf(NULL, 0, "%" PRIu64, list->items[index].version);
		if (width > version_width)
			version_width = width;
	}
	printf("%-*s %-36s %s\n", version_width, "VERSION", "NAME", "MOUNT");
	for (index = 0; index < list->count; index++) {
		const struct apex_item *item = &list->items[index];
		const char *base;

		if (!item->name[0] || item->error[0])
			continue;
		if (have_last && item->version == last_version &&
		    strcmp(item->name, last_name) == 0)
			continue;
		base = strrchr(item->source, '/');
		base = base ? base + 1 : item->source;
		printf("%-*" PRIu64 " %-36s /apex/%s\n", version_width,
			item->version, item->name, base);
		snprintf(last_name, sizeof(last_name), "%s", item->name);
		last_version = item->version;
		have_last = true;
		printed++;
	}
	return printed;
}

static int dump_live_packages(const struct apex_options *options)
{
	struct apex_list list;
	char container_apex[PATH_MAX];
	char *root;
	pid_t pid;
	int result;
	int path_size = -1;

	apex_list_init(&list);
	if (container_get_pid(options, &pid) == 0)
		path_size = snprintf(container_apex, sizeof(container_apex),
			"/proc/%ld/root/apex", (long)pid);
	if (path_size >= 0 && path_size < (int)sizeof(container_apex))
		root = container_apex;
	else
		root = (char *)options->mount_root;
	result = apex_discover(options, 1, &root, &list);
	if ((result < 0 || !list.count) && root == container_apex) {
		apex_list_free(&list);
		apex_list_init(&list);
		root = (char *)options->mount_root;
		result = apex_discover(options, 1, &root, &list);
	}
	if (result < 0) {
		apex_list_free(&list);
		return -1;
	}
	puts("\nMOUNTED PACKAGES (live /apex fallback):");
	if (!print_live_packages(&list))
		puts("(none found)");
	apex_list_free(&list);
	return 0;
}

int apexd_dump(const struct apex_options *options, int argc, char **argv)
{
	char *arguments[] = {
		"lxc-attach", "-n", (char *)options->container, "--",
		"/system/bin/dumpsys", "apexservice", NULL
	};
	char *dump;
	int exit_status;
	int result;
	size_t dump_size = 1024 * 1024;

	if (argc)
		return run_dumpsys(options, argc, argv);
	dump = malloc(dump_size);
	if (!dump)
		return -1;
	result = run_capture(arguments, dump, dump_size, &exit_status);
	if (result < 0) {
		free(dump);
		return -1;
	}
	if (dump[0]) {
		fputs(dump, stdout);
		if (dump[strlen(dump) - 1] != '\n')
			putchar('\n');
	}
	if (exit_status != 0) {
		free(dump);
		errno = EIO;
		return exit_status;
	}
	if (!dump_has_active_packages(dump) && dump_live_packages(options) < 0)
		apex_log("warning", "cannot inspect live APEX mounts: %s",
			strerror(errno));
	free(dump);
	return 0;
}

static int sync_bind(const char *source, const char *target, bool file,
		bool dry_run)
{
	if (path_is_mountpoint(target)) {
		apex_debug("already mirrored: %s", target);
		return 0;
	}
	if (dry_run) {
		printf("mirror %s -> %s\n", source, target);
		return 0;
	}
	if (file) {
		if (ensure_empty_file(target, 0644) < 0)
			return -1;
	} else if (mkdir_p(target, 0755) < 0) {
		return -1;
	}
	if (mount(source, target, NULL, MS_BIND | (file ? 0 : MS_REC), NULL) < 0)
		return -1;
	if (mount(NULL, target, NULL,
			MS_BIND | MS_REMOUNT | MS_RDONLY | MS_NODEV | MS_NOSUID,
			NULL) < 0) {
		int saved_errno = errno;
		umount2(target, MNT_DETACH);
		errno = saved_errno;
		return -1;
	}
	return 0;
}

int apex_sync(const struct apex_options *options)
{
	char source_root[PATH_MAX];
	DIR *directory = NULL;
	struct dirent *entry;
	pid_t pid;
	int failures = 0;

	if (geteuid() != 0 && !options->dry_run) {
		apex_log("error", "sync requires root (or use --dry-run)");
		errno = EPERM;
		return -1;
	}
	if (apexd_wait(options, "ready") < 0)
		return -1;
	if (container_get_pid(options, &pid) < 0)
		return -1;
	if (snprintf(source_root, sizeof(source_root), "/proc/%ld/root/apex",
			(long)pid) >= (int)sizeof(source_root)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	directory = opendir(source_root);
	if (!directory)
		return -1;
	if (!options->dry_run && mkdir_p(options->mount_root, 0755) < 0) {
		closedir(directory);
		return -1;
	}
	while ((entry = readdir(directory))) {
		char source[PATH_MAX];
		char target[PATH_MAX];
		struct stat metadata;
		bool file;

		if (entry->d_name[0] == '.' || strchr(entry->d_name, '@'))
			continue;
		if (strcmp(entry->d_name, "apex-info-list.xml") != 0 &&
		    !valid_apex_name(entry->d_name)) {
			apex_log("warning", "ignoring unexpected /apex entry: %s",
				entry->d_name);
			continue;
		}
		if (!path_join(source, sizeof(source), source_root, entry->d_name) ||
		    !path_join(target, sizeof(target), options->mount_root,
			entry->d_name)) {
			failures++;
			continue;
		}
		if (stat(source, &metadata) < 0) {
			failures++;
			continue;
		}
		file = S_ISREG(metadata.st_mode);
		if (!file && !S_ISDIR(metadata.st_mode))
			continue;
		if (sync_bind(source, target, file, options->dry_run) < 0) {
			apex_log("error", "cannot mirror %s: %s", source,
				strerror(errno));
			failures++;
		}
	}
	closedir(directory);
	return failures ? -1 : 0;
}
