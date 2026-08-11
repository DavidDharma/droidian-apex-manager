#ifndef DROIDIAN_APEX_MANAGER_H
#define DROIDIAN_APEX_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define APEX_NAME_MAX 255
#define APEX_VERSION "0.1.0"

enum apex_kind {
	APEX_FLATTENED,
	APEX_PACKAGED,
	APEX_COMPRESSED,
};

struct apex_item {
	enum apex_kind kind;
	char source[PATH_MAX];
	char name[APEX_NAME_MAX + 1];
	uint64_t version;
	char error[256];
};

struct apex_list {
	struct apex_item *items;
	size_t count;
	size_t capacity;
};

struct apex_options {
	const char *android_root;
	const char *mount_root;
	const char *container;
	unsigned int timeout;
	bool verbose;
	bool dry_run;
};

struct zip_entry {
	uint16_t method;
	uint16_t flags;
	uint64_t compressed_size;
	uint64_t uncompressed_size;
	uint64_t data_offset;
};

extern bool apex_verbose;

void apex_log(const char *level, const char *fmt, ...);
void apex_debug(const char *fmt, ...);
bool path_join(char *out, size_t size, const char *left, const char *right);
bool has_suffix(const char *value, const char *suffix);
bool valid_apex_name(const char *name);
int mkdir_p(const char *path, mode_t mode);
int ensure_empty_file(const char *path, mode_t mode);
bool path_is_mountpoint(const char *path);
int run_capture(char *const argv[], char *output, size_t output_size,
		int *exit_status);
int run_passthrough(char *const argv[]);

int apex_manifest_parse(const unsigned char *data, size_t size,
		bool json, char *name, size_t name_size, uint64_t *version);
int apex_manifest_from_dir(const char *directory, char *name,
		size_t name_size, uint64_t *version);

int zip_find_entry(const char *path, const char *wanted,
		struct zip_entry *entry);
int zip_read_entry(const char *path, const char *wanted,
		unsigned char **data, size_t *size);
int zip_extract_entry(const char *path, const char *wanted,
		const char *destination, mode_t mode);

void apex_list_init(struct apex_list *list);
void apex_list_free(struct apex_list *list);
int apex_list_add(struct apex_list *list, const struct apex_item *item);
int apex_discover(const struct apex_options *options, int root_count,
		char **roots, struct apex_list *list);
const char *apex_kind_name(enum apex_kind kind);
int apex_print_list(const struct apex_list *list, bool json);

int apex_mount_all(const struct apex_options *options,
		const struct apex_list *list);
int apex_mount_paths(const struct apex_options *options, int count,
		char **paths);

int container_get_state(const struct apex_options *options, char *state,
		size_t state_size);
int container_get_pid(const struct apex_options *options, pid_t *pid);
int apexd_get_status(const struct apex_options *options, char *status,
		size_t status_size);
int apexd_wait(const struct apex_options *options, const char *wanted);
int apexd_dump(const struct apex_options *options, int argc, char **argv);
int apex_sync(const struct apex_options *options);
int apex_diagnose(const struct apex_options *options);

#endif
