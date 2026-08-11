#include "apex-manager.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int read_file(const char *path, unsigned char **data, size_t *size)
{
	struct stat metadata;
	unsigned char *buffer;
	ssize_t amount;
	size_t used = 0;
	int fd;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	if (fstat(fd, &metadata) < 0 || metadata.st_size < 0 ||
	    metadata.st_size > 1024 * 1024) {
		close(fd);
		errno = EFBIG;
		return -1;
	}
	buffer = malloc((size_t)metadata.st_size + 1);
	if (!buffer) {
		close(fd);
		return -1;
	}
	while (used < (size_t)metadata.st_size) {
		amount = read(fd, buffer + used, (size_t)metadata.st_size - used);
		if (amount < 0 && errno == EINTR)
			continue;
		if (amount <= 0)
			break;
		used += (size_t)amount;
	}
	close(fd);
	buffer[used] = '\0';
	*data = buffer;
	*size = used;
	return 0;
}

static const unsigned char *json_value(const unsigned char *data, size_t size,
		const char *key)
{
	char pattern[80];
	const unsigned char *cursor = data;
	size_t pattern_size;

	if (snprintf(pattern, sizeof(pattern), "\"%s\"", key) < 0)
		return NULL;
	pattern_size = strlen(pattern);
	while ((size_t)(cursor - data) + pattern_size <= size) {
		if (memcmp(cursor, pattern, pattern_size) != 0) {
			cursor++;
			continue;
		}
		cursor += pattern_size;
		while ((size_t)(cursor - data) < size && isspace(*cursor))
			cursor++;
		if ((size_t)(cursor - data) >= size || *cursor++ != ':')
			continue;
		while ((size_t)(cursor - data) < size && isspace(*cursor))
			cursor++;
		return cursor;
	}
	return NULL;
}

static int parse_json(const unsigned char *data, size_t size, char *name,
		size_t name_size, uint64_t *version)
{
	const unsigned char *cursor;
	size_t length = 0;
	char number[32];
	size_t number_size = 0;

	cursor = json_value(data, size, "name");
	if (!cursor || *cursor++ != '"')
		return -1;
	while ((size_t)(cursor - data) < size && *cursor != '"') {
		if (*cursor == '\\' || *cursor < 0x20 || length + 1 >= name_size)
			return -1;
		name[length++] = (char)*cursor++;
	}
	if ((size_t)(cursor - data) >= size || *cursor != '"')
		return -1;
	name[length] = '\0';
	if (!valid_apex_name(name))
		return -1;

	cursor = json_value(data, size, "version");
	if (!cursor)
		cursor = json_value(data, size, "versionCode");
	if (!cursor) {
		*version = 0;
		return 0;
	}
	if (*cursor == '"')
		cursor++;
	while ((size_t)(cursor - data) < size && isdigit(*cursor) &&
	       number_size + 1 < sizeof(number))
		number[number_size++] = (char)*cursor++;
	if (!number_size)
		return -1;
	number[number_size] = '\0';
	errno = 0;
	*version = strtoull(number, NULL, 10);
	return errno == ERANGE ? -1 : 0;
}

static int read_varint(const unsigned char **cursor,
		const unsigned char *end, uint64_t *value)
{
	unsigned int shift = 0;
	uint64_t result = 0;

	while (*cursor < end && shift < 64) {
		unsigned char byte = *(*cursor)++;
		result |= (uint64_t)(byte & 0x7f) << shift;
		if (!(byte & 0x80)) {
			*value = result;
			return 0;
		}
		shift += 7;
	}
	return -1;
}

static int parse_protobuf(const unsigned char *data, size_t size, char *name,
		size_t name_size, uint64_t *version)
{
	const unsigned char *cursor = data;
	const unsigned char *end = data + size;
	bool got_name = false;

	*version = 0;
	while (cursor < end) {
		uint64_t tag;
		uint64_t value;
		unsigned int field;
		unsigned int wire;

		if (read_varint(&cursor, end, &tag) < 0)
			return -1;
		field = (unsigned int)(tag >> 3);
		wire = (unsigned int)(tag & 7);
		if (wire == 0) {
			if (read_varint(&cursor, end, &value) < 0)
				return -1;
			if (field == 2)
				*version = value;
		} else if (wire == 1) {
			if ((size_t)(end - cursor) < 8)
				return -1;
			cursor += 8;
		} else if (wire == 2) {
			if (read_varint(&cursor, end, &value) < 0 ||
			    value > (uint64_t)(end - cursor))
				return -1;
			if (field == 1) {
				if (value == 0 || value >= name_size)
					return -1;
				memcpy(name, cursor, (size_t)value);
				name[value] = '\0';
				got_name = valid_apex_name(name);
			}
			cursor += (size_t)value;
		} else if (wire == 5) {
			if ((size_t)(end - cursor) < 4)
				return -1;
			cursor += 4;
		} else {
			return -1;
		}
	}
	return got_name ? 0 : -1;
}

int apex_manifest_parse(const unsigned char *data, size_t size,
		bool json, char *name, size_t name_size, uint64_t *version)
{
	if (!data || !name || !name_size || !version) {
		errno = EINVAL;
		return -1;
	}
	return json ? parse_json(data, size, name, name_size, version) :
		parse_protobuf(data, size, name, name_size, version);
}

int apex_manifest_from_dir(const char *directory, char *name,
		size_t name_size, uint64_t *version)
{
	static const struct {
		const char *filename;
		bool json;
	} manifests[] = {
		{ "apex_manifest.pb", false },
		{ "apex_manifest.json", true },
	};
	unsigned char *data = NULL;
	size_t size;
	char path[PATH_MAX];
	size_t index;

	for (index = 0; index < sizeof(manifests) / sizeof(manifests[0]); index++) {
		if (!path_join(path, sizeof(path), directory,
				manifests[index].filename)) {
			errno = ENAMETOOLONG;
			return -1;
		}
		if (read_file(path, &data, &size) < 0)
			continue;
		if (apex_manifest_parse(data, size, manifests[index].json,
				name, name_size, version) == 0) {
			free(data);
			return 0;
		}
		free(data);
		data = NULL;
	}
	errno = EINVAL;
	return -1;
}
