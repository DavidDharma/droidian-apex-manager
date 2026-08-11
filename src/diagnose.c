#include "apex-manager.h"

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool filesystem_available(const char *wanted)
{
	FILE *filesystems = fopen("/proc/filesystems", "re");
	char line[256];
	bool found = false;

	if (!filesystems)
		return false;
	while (fgets(line, sizeof(line), filesystems)) {
		char *name = strrchr(line, '\t');
		char *newline;

		name = name ? name + 1 : line;
		while (*name == ' ' || *name == '\t')
			name++;
		newline = strchr(name, '\n');
		if (newline)
			*newline = '\0';
		if (strcmp(name, wanted) == 0) {
			found = true;
			break;
		}
	}
	fclose(filesystems);
	return found;
}

static const char *yes_no(bool value)
{
	return value ? "yes" : "no";
}

int apex_diagnose(const struct apex_options *options)
{
	struct apex_list list;
	char state[64];
	char status[64];
	pid_t pid;
	size_t flattened = 0;
	size_t packaged = 0;
	size_t compressed = 0;
	size_t warnings = 0;
	size_t index;
	struct stat metadata;

	printf("Droidian APEX manager %s diagnostics\n\n", APEX_VERSION);
	printf("Configuration:\n");
	printf("  Android root:  %s\n", options->android_root);
	printf("  host APEX root: %s\n", options->mount_root);
	printf("  LXC container:  %s\n", options->container);
	printf("  timeout:        %u seconds\n", options->timeout);
	printf("  effective UID:  %ld%s\n\n", (long)geteuid(),
		geteuid() == 0 ? " (mount capable)" : " (mount commands need root)");

	printf("Kernel and host:\n");
	printf("  /dev/loop-control: %s\n", yes_no(access("/dev/loop-control", R_OK | W_OK) == 0));
	printf("  /dev/mapper/control: %s\n", yes_no(access("/dev/mapper/control", R_OK | W_OK) == 0));
	printf("  ext4 filesystem: %s\n", yes_no(filesystem_available("ext4")));
	printf("  EROFS filesystem: %s\n", yes_no(filesystem_available("erofs")));
	printf("  %s exists: %s\n", options->mount_root,
		yes_no(stat(options->mount_root, &metadata) == 0));
	printf("  %s is a mount point: %s\n\n", options->mount_root,
		yes_no(path_is_mountpoint(options->mount_root)));

	printf("Container and apexd:\n");
	if (container_get_state(options, state, sizeof(state)) == 0)
		printf("  container state: %s\n", state);
	else
		printf("  container state: unavailable (%s)\n", strerror(errno));
	if (container_get_pid(options, &pid) == 0)
		printf("  init PID: %ld\n", (long)pid);
	else
		printf("  init PID: unavailable\n");
	if (apexd_get_status(options, status, sizeof(status)) == 0)
		printf("  apexd.status: %s\n\n", status[0] ? status : "unset");
	else
		printf("  apexd.status: unavailable (%s)\n\n", strerror(errno));

	apex_list_init(&list);
	if (apex_discover(options, 0, NULL, &list) < 0) {
		printf("Pre-start discovery: failed (%s)\n", strerror(errno));
		apex_list_free(&list);
		return 0;
	}
	for (index = 0; index < list.count; index++) {
		switch (list.items[index].kind) {
		case APEX_FLATTENED: flattened++; break;
		case APEX_PACKAGED: packaged++; break;
		case APEX_COMPRESSED: compressed++; break;
		}
		if (list.items[index].error[0])
			warnings++;
	}
	printf("Pre-start discovery:\n");
	printf("  total:      %zu\n", list.count);
	printf("  flattened:  %zu\n", flattened);
	printf("  packaged:   %zu\n", packaged);
	printf("  compressed: %zu\n", compressed);
	printf("  warnings:   %zu\n", warnings);
	for (index = 0; index < list.count; index++) {
		const struct apex_item *item = &list.items[index];

		printf("  - %-10s %-32s version=%" PRIu64 " source=%s",
			apex_kind_name(item->kind), item->name[0] ? item->name : "(unknown)",
			item->version, item->source);
		if (item->error[0])
			printf(" warning=%s", item->error);
		putchar('\n');
	}
	if (!list.count)
		printf("\nHint: verify --android-root and that Android partitions are mounted.\n");
	if ((packaged || compressed) && access("/dev/loop-control", F_OK) < 0)
		printf("\nHint: packaged APEXs require CONFIG_BLK_DEV_LOOP and /dev/loop-control.\n");
	apex_list_free(&list);
	return 0;
}
