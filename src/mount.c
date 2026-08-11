#include "apex-manager.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/loop.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

struct payload {
	char path[PATH_MAX];
	uint64_t offset;
	uint64_t size;
	char temporary_apex[PATH_MAX];
	char temporary_payload[PATH_MAX];
};

static int create_temporary(char *path, size_t size, const char *name,
		const char *suffix)
{
	const char *directory = geteuid() == 0 ?
		"/run/droidian-apex-manager" : "/tmp";
	int fd;

	if (geteuid() == 0 && mkdir_p(directory, 0755) < 0)
		return -1;
	if (snprintf(path, size, "%s/%s-XXXXXX%s", directory, name, suffix) >=
			(int)size) {
		errno = ENAMETOOLONG;
		return -1;
	}
	/* mkstemps requires the suffix length, excluding the terminating byte. */
	fd = mkstemps(path, (int)strlen(suffix));
	if (fd < 0)
		return -1;
	if (fchmod(fd, 0600) < 0) {
		int saved_errno = errno;
		close(fd);
		unlink(path);
		errno = saved_errno;
		return -1;
	}
	return fd;
}

static void payload_cleanup(struct payload *payload)
{
	if (payload->temporary_payload[0])
		unlink(payload->temporary_payload);
	if (payload->temporary_apex[0])
		unlink(payload->temporary_apex);
}

static int get_payload(const struct apex_item *item, struct payload *payload)
{
	struct zip_entry entry;
	const char *apex_path = item->source;
	struct stat metadata;
	int fd;

	memset(payload, 0, sizeof(*payload));
	if (item->kind == APEX_COMPRESSED) {
		fd = create_temporary(payload->temporary_apex,
			sizeof(payload->temporary_apex), item->name, ".apex");
		if (fd < 0)
			return -1;
		close(fd);
		if (zip_extract_entry(item->source, "original_apex",
				payload->temporary_apex, 0600) < 0) {
			payload_cleanup(payload);
			return -1;
		}
		apex_path = payload->temporary_apex;
	}
	if (zip_find_entry(apex_path, "apex_payload.img", &entry) < 0) {
		payload_cleanup(payload);
		return -1;
	}
	if (entry.method == 0) {
		if (strlen(apex_path) >= sizeof(payload->path)) {
			errno = ENAMETOOLONG;
			payload_cleanup(payload);
			return -1;
		}
		strcpy(payload->path, apex_path);
		payload->offset = entry.data_offset;
		payload->size = entry.uncompressed_size;
		return 0;
	}
	fd = create_temporary(payload->temporary_payload,
		sizeof(payload->temporary_payload), item->name, ".img");
	if (fd < 0) {
		payload_cleanup(payload);
		return -1;
	}
	close(fd);
	if (zip_extract_entry(apex_path, "apex_payload.img",
			payload->temporary_payload, 0600) < 0 ||
	    stat(payload->temporary_payload, &metadata) < 0) {
		payload_cleanup(payload);
		return -1;
	}
	strcpy(payload->path, payload->temporary_payload);
	payload->size = (uint64_t)metadata.st_size;
	return 0;
}

static const char *payload_filesystem(const struct payload *payload)
{
	unsigned char superblock[64];
	int fd = open(payload->path, O_RDONLY | O_CLOEXEC);
	ssize_t amount;

	if (fd < 0)
		return NULL;
	amount = pread(fd, superblock, sizeof(superblock),
		(off_t)(payload->offset + 1024));
	close(fd);
	if (amount < 58) {
		errno = EINVAL;
		return NULL;
	}
	/* EROFS magic is at byte zero of its 1024-byte superblock. */
	if (superblock[0] == 0xe2 && superblock[1] == 0xe1 &&
	    superblock[2] == 0xf5 && superblock[3] == 0xe0)
		return "erofs";
	/* ext2/3/4 superblock magic is at byte 56. */
	if (superblock[56] == 0x53 && superblock[57] == 0xef)
		return "ext4";
	errno = ENODEV;
	return NULL;
}

static int setup_loop(const struct payload *payload, char *device,
		size_t device_size)
{
	int control = -1;
	int backing = -1;
	int loop = -1;
	int number;
	struct loop_info64 info;
	int result = -1;

	control = open("/dev/loop-control", O_RDWR | O_CLOEXEC);
	if (control < 0)
		goto out;
	number = ioctl(control, LOOP_CTL_GET_FREE);
	if (number < 0)
		goto out;
	if (snprintf(device, device_size, "/dev/loop%d", number) >=
			(int)device_size) {
		errno = ENAMETOOLONG;
		goto out;
	}
	backing = open(payload->path, O_RDONLY | O_CLOEXEC);
	if (backing < 0)
		goto out;
	loop = open(device, O_RDONLY | O_CLOEXEC);
	if (loop < 0)
		goto out;
	if (ioctl(loop, LOOP_SET_FD, backing) < 0)
		goto out;
	memset(&info, 0, sizeof(info));
	info.lo_offset = payload->offset;
	info.lo_sizelimit = payload->size;
	info.lo_flags = LO_FLAGS_READ_ONLY | LO_FLAGS_AUTOCLEAR;
	snprintf((char *)info.lo_file_name, LO_NAME_SIZE, "%.*s",
		LO_NAME_SIZE - 1, payload->path);
	if (ioctl(loop, LOOP_SET_STATUS64, &info) < 0) {
		int saved_errno = errno;
		ioctl(loop, LOOP_CLR_FD, 0);
		errno = saved_errno;
		goto out;
	}
	result = loop;
	loop = -1;
out:
	if (loop >= 0)
		close(loop);
	if (backing >= 0)
		close(backing);
	if (control >= 0)
		close(control);
	return result;
}

static int bind_read_only(const char *source, const char *target, bool dry_run)
{
	if (path_is_mountpoint(target)) {
		apex_debug("already mounted: %s", target);
		return 0;
	}
	if (dry_run) {
		printf("bind %s -> %s\n", source, target);
		return 0;
	}
	if (mount(source, target, NULL, MS_BIND | MS_REC, NULL) < 0)
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

static int mount_packaged(const struct apex_item *item, const char *target,
		bool dry_run)
{
	struct payload payload;
	const char *filesystem;
	char device[64];
	int loop = -1;
	int result = -1;

	if (path_is_mountpoint(target))
		return 0;
	if (get_payload(item, &payload) < 0)
		return -1;
	filesystem = payload_filesystem(&payload);
	if (!filesystem)
		goto out;
	if (dry_run) {
		printf("loop-mount %s offset=%" PRIu64 " size=%" PRIu64
		       " type=%s -> %s\n", payload.path, payload.offset,
		       payload.size, filesystem, target);
		result = 0;
		goto out;
	}
	loop = setup_loop(&payload, device, sizeof(device));
	if (loop < 0)
		goto out;
	if (mount(device, target, filesystem,
			MS_RDONLY | MS_NODEV | MS_NOSUID,
			strcmp(filesystem, "ext4") == 0 ? "errors=remount-ro" : NULL) < 0)
		goto out;
	result = 0;
out:
	if (result < 0 && loop >= 0)
		ioctl(loop, LOOP_CLR_FD, 0);
	if (loop >= 0)
		close(loop);
	payload_cleanup(&payload);
	return result;
}

static int mount_one(const struct apex_options *options,
		const struct apex_item *item)
{
	char versioned[PATH_MAX];
	char canonical[PATH_MAX];
	char versioned_name[APEX_NAME_MAX + 32];
	int result;

	if (!item->name[0] || item->error[0]) {
		apex_log("error", "refusing to mount %s: %s", item->source,
			item->error[0] ? item->error : "invalid APEX name");
		errno = EINVAL;
		return -1;
	}
	if (snprintf(versioned_name, sizeof(versioned_name), "%s@%" PRIu64,
			item->name, item->version) >= (int)sizeof(versioned_name) ||
	    !path_join(versioned, sizeof(versioned), options->mount_root,
			versioned_name) ||
	    !path_join(canonical, sizeof(canonical), options->mount_root,
			item->name)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	if (!options->dry_run &&
	    (mkdir_p(versioned, 0755) < 0 || mkdir_p(canonical, 0755) < 0))
		return -1;
	if (item->kind == APEX_FLATTENED)
		result = bind_read_only(item->source, versioned, options->dry_run);
	else
		result = mount_packaged(item, versioned, options->dry_run);
	if (result < 0)
		return -1;
	return bind_read_only(versioned, canonical, options->dry_run);
}

static bool source_is_data_update(const char *source)
{
	return strstr(source, "/data/apex/active/") != NULL;
}

static const struct apex_item *selected_item(const struct apex_list *list,
		const char *name)
{
	const struct apex_item *selected = NULL;
	size_t index;

	for (index = 0; index < list->count; index++) {
		const struct apex_item *item = &list->items[index];

		if (strcmp(item->name, name) != 0)
			continue;
		if (!selected || item->version > selected->version ||
		    (item->version == selected->version &&
		     source_is_data_update(item->source) &&
		     !source_is_data_update(selected->source)))
			selected = item;
	}
	return selected;
}

int apex_mount_all(const struct apex_options *options,
		const struct apex_list *list)
{
	size_t index;
	int failures = 0;

	if (geteuid() != 0 && !options->dry_run) {
		apex_log("error", "mount requires root (or use --dry-run)");
		errno = EPERM;
		return -1;
	}
	if (!options->dry_run && mkdir_p(options->mount_root, 0755) < 0)
		return -1;
	for (index = 0; index < list->count; index++) {
		const struct apex_item *item = &list->items[index];
		const struct apex_item *selected;

		if (!item->name[0]) {
			failures++;
			continue;
		}
		if (index && strcmp(item->name, list->items[index - 1].name) == 0)
			continue;
		selected = selected_item(list, item->name);
		if (!selected) {
			apex_log("error", "internal error selecting APEX %s", item->name);
			failures++;
			continue;
		}
		apex_debug("selected %s version %" PRIu64 " from %s",
			selected->name, selected->version, selected->source);
		if (mount_one(options, selected) < 0) {
			apex_log("error", "cannot mount %s: %s", selected->source,
				strerror(errno));
			failures++;
		}
	}
	return failures ? -1 : 0;
}

int apex_mount_paths(const struct apex_options *options, int count,
		char **paths)
{
	struct apex_list list;
	int result;

	apex_list_init(&list);
	if (apex_discover(options, count, paths, &list) < 0) {
		apex_list_free(&list);
		return -1;
	}
	if (!list.count) {
		apex_log("error", "no APEXs found in the requested paths");
		apex_list_free(&list);
		errno = ENOENT;
		return -1;
	}
	result = apex_mount_all(options, &list);
	apex_list_free(&list);
	return result;
}
