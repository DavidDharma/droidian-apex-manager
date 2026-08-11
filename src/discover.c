#include "apex-manager.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void apex_list_init(struct apex_list *list)
{
	memset(list, 0, sizeof(*list));
}

void apex_list_free(struct apex_list *list)
{
	free(list->items);
	memset(list, 0, sizeof(*list));
}

int apex_list_add(struct apex_list *list, const struct apex_item *item)
{
	struct apex_item *items;
	size_t capacity;

	if (list->count == list->capacity) {
		capacity = list->capacity ? list->capacity * 2 : 16;
		if (capacity > SIZE_MAX / sizeof(*items)) {
			errno = ENOMEM;
			return -1;
		}
		items = realloc(list->items, capacity * sizeof(*items));
		if (!items)
			return -1;
		list->items = items;
		list->capacity = capacity;
	}
	list->items[list->count++] = *item;
	return 0;
}

const char *apex_kind_name(enum apex_kind kind)
{
	switch (kind) {
	case APEX_FLATTENED:
		return "flattened";
	case APEX_PACKAGED:
		return "packaged";
	case APEX_COMPRESSED:
		return "compressed";
	}
	return "unknown";
}

static int manifest_from_package(const char *path, char *name,
		size_t name_size, uint64_t *version)
{
	static const struct {
		const char *filename;
		bool json;
	} manifests[] = {
		{ "apex_manifest.pb", false },
		{ "apex_manifest.json", true },
	};
	size_t index;

	for (index = 0; index < sizeof(manifests) / sizeof(manifests[0]); index++) {
		unsigned char *data = NULL;
		size_t size = 0;

		if (zip_read_entry(path, manifests[index].filename, &data, &size) < 0)
			continue;
		if (apex_manifest_parse(data, size, manifests[index].json,
				name, name_size, version) == 0) {
			free(data);
			return 0;
		}
		free(data);
	}
	errno = EINVAL;
	return -1;
}

static void derive_identity(const char *path, char *name, size_t name_size,
		uint64_t *version)
{
	const char *base = strrchr(path, '/');
	char *marker;
	char *suffix;

	base = base ? base + 1 : path;
	snprintf(name, name_size, "%s", base);
	if (has_suffix(name, ".capex"))
		name[strlen(name) - 6] = '\0';
	else if (has_suffix(name, ".apex"))
		name[strlen(name) - 5] = '\0';
	marker = strrchr(name, '@');
	if (marker && marker[1]) {
		char *end = NULL;
		unsigned long long parsed = strtoull(marker + 1, &end, 10);

		if (end && *end == '\0') {
			*version = parsed;
			*marker = '\0';
		}
	}
	for (;;) {
		suffix = strrchr(name, '.');
		if (!suffix)
			break;
		if (strcmp(suffix, ".release") == 0 ||
		    strcmp(suffix, ".debug") == 0 ||
		    strcmp(suffix, ".current") == 0) {
			*suffix = '\0';
			continue;
		}
		if (suffix[1] == 'v' && suffix[2] >= '0' && suffix[2] <= '9') {
			char *cursor;
			for (cursor = suffix + 2; *cursor >= '0' && *cursor <= '9'; cursor++)
				;
			if (*cursor == '\0') {
				*suffix = '\0';
				continue;
			}
		}
		break;
	}
}

static int item_from_path(const char *path, struct apex_item *item,
		bool *is_apex)
{
	struct stat metadata;
	int saved_errno;

	memset(item, 0, sizeof(*item));
	*is_apex = false;
	if (strlen(path) >= sizeof(item->source)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	if (stat(path, &metadata) < 0)
		return -1;
	strcpy(item->source, path);
	if (S_ISDIR(metadata.st_mode)) {
		char manifest[PATH_MAX];

		if (!path_join(manifest, sizeof(manifest), path, "apex_manifest.pb"))
			return -1;
		if (access(manifest, F_OK) < 0) {
			if (!path_join(manifest, sizeof(manifest), path,
					"apex_manifest.json"))
				return -1;
			if (access(manifest, F_OK) < 0)
				return 0;
		}
		*is_apex = true;
		item->kind = APEX_FLATTENED;
		if (apex_manifest_from_dir(path, item->name,
				sizeof(item->name), &item->version) == 0)
			return 0;
	} else if (S_ISREG(metadata.st_mode) && has_suffix(path, ".apex")) {
		*is_apex = true;
		item->kind = APEX_PACKAGED;
		if (manifest_from_package(path, item->name,
				sizeof(item->name), &item->version) == 0)
			return 0;
	} else if (S_ISREG(metadata.st_mode) && has_suffix(path, ".capex")) {
		*is_apex = true;
		item->kind = APEX_COMPRESSED;
		if (manifest_from_package(path, item->name,
				sizeof(item->name), &item->version) == 0)
			return 0;
	} else {
		return 0;
	}

	saved_errno = errno;
	derive_identity(path, item->name, sizeof(item->name), &item->version);
	snprintf(item->error, sizeof(item->error), "manifest unreadable: %s",
		strerror(saved_errno));
	if (!valid_apex_name(item->name)) {
		snprintf(item->error, sizeof(item->error),
			"manifest unreadable and filename is not a valid APEX name");
		item->name[0] = '\0';
	}
	return 0;
}

static int compare_items(const void *left_pointer, const void *right_pointer)
{
	const struct apex_item *left = left_pointer;
	const struct apex_item *right = right_pointer;
	int comparison = strcmp(left->name, right->name);

	if (comparison)
		return comparison;
	if (left->version < right->version)
		return -1;
	if (left->version > right->version)
		return 1;
	return strcmp(left->source, right->source);
}

static int scan_root(const char *root, struct apex_list *list)
{
	struct stat metadata;
	struct apex_item root_item;
	bool root_is_apex;
	DIR *directory;
	struct dirent *entry;
	int result = 0;

	if (stat(root, &metadata) < 0) {
		if (errno == ENOENT || errno == ENOTDIR) {
			apex_debug("source directory does not exist: %s", root);
			return 0;
		}
		return -1;
	}
	if (item_from_path(root, &root_item, &root_is_apex) < 0)
		return -1;
	if (root_is_apex)
		return apex_list_add(list, &root_item);
	if (!S_ISDIR(metadata.st_mode))
		return 0;
	directory = opendir(root);
	if (!directory)
		return -1;
	while ((entry = readdir(directory))) {
		char path[PATH_MAX];
		struct apex_item item;
		bool is_apex;

		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;
		if (!path_join(path, sizeof(path), root, entry->d_name)) {
			errno = ENAMETOOLONG;
			result = -1;
			break;
		}
		if (item_from_path(path, &item, &is_apex) < 0) {
			apex_log("warning", "cannot inspect %s: %s", path, strerror(errno));
			continue;
		}
		if (is_apex && apex_list_add(list, &item) < 0) {
			result = -1;
			break;
		}
	}
	closedir(directory);
	return result;
}

int apex_discover(const struct apex_options *options, int root_count,
		char **roots, struct apex_list *list)
{
	static const char *relative_roots[] = {
		"system/apex",
		"system/system_ext/apex",
		"system/product/apex",
		"system/vendor/apex",
		"system/odm/apex",
		"system_ext/apex",
		"product/apex",
		"vendor/apex",
		"odm/apex",
		"data/apex/active",
	};
	size_t index;

	if (root_count) {
		int argument;

		for (argument = 0; argument < root_count; argument++) {
			if (scan_root(roots[argument], list) < 0)
				return -1;
		}
	} else {
		for (index = 0; index < sizeof(relative_roots) /
					     sizeof(relative_roots[0]); index++) {
			char path[PATH_MAX];

			if (!path_join(path, sizeof(path), options->android_root,
					relative_roots[index])) {
				errno = ENAMETOOLONG;
				return -1;
			}
			if (scan_root(path, list) < 0)
				return -1;
		}
	}
	qsort(list->items, list->count, sizeof(*list->items), compare_items);
	return 0;
}

static void print_json_string(const char *value)
{
	const unsigned char *cursor = (const unsigned char *)value;

	putchar('"');
	for (; *cursor; cursor++) {
		switch (*cursor) {
		case '"': fputs("\\\"", stdout); break;
		case '\\': fputs("\\\\", stdout); break;
		case '\b': fputs("\\b", stdout); break;
		case '\f': fputs("\\f", stdout); break;
		case '\n': fputs("\\n", stdout); break;
		case '\r': fputs("\\r", stdout); break;
		case '\t': fputs("\\t", stdout); break;
		default:
			if (*cursor < 0x20)
				printf("\\u%04x", *cursor);
			else
				putchar(*cursor);
		}
	}
	putchar('"');
}

int apex_print_list(const struct apex_list *list, bool json)
{
	size_t index;

	if (json) {
		fputs("[\n", stdout);
		for (index = 0; index < list->count; index++) {
			const struct apex_item *item = &list->items[index];

			fputs("  {\"name\":", stdout);
			print_json_string(item->name);
			printf(",\"version\":%" PRIu64 ",\"type\":", item->version);
			print_json_string(apex_kind_name(item->kind));
			fputs(",\"source\":", stdout);
			print_json_string(item->source);
			if (item->error[0]) {
				fputs(",\"warning\":", stdout);
				print_json_string(item->error);
			}
			printf("}%s\n", index + 1 == list->count ? "" : ",");
		}
		fputs("]\n", stdout);
		return 0;
	}
	printf("%-11s %-8s %-36s %s\n", "TYPE", "VERSION", "NAME", "SOURCE");
	for (index = 0; index < list->count; index++) {
		const struct apex_item *item = &list->items[index];

		printf("%-11s %-8" PRIu64 " %-36s %s%s%s%s\n",
			apex_kind_name(item->kind), item->version, item->name,
			item->source, item->error[0] ? "  [" : "",
			item->error[0] ? item->error : "",
			item->error[0] ? "]" : "");
	}
	return 0;
}
