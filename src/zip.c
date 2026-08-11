#include "apex-manager.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#define ZIP_LOCAL_SIGNATURE 0x04034b50U
#define ZIP_CENTRAL_SIGNATURE 0x02014b50U
#define ZIP_EOCD_SIGNATURE 0x06054b50U
#define ZIP_STORED 0
#define ZIP_DEFLATED 8

static uint16_t little16(const unsigned char *value)
{
	return (uint16_t)value[0] | (uint16_t)value[1] << 8;
}

static uint32_t little32(const unsigned char *value)
{
	return (uint32_t)value[0] | (uint32_t)value[1] << 8 |
		(uint32_t)value[2] << 16 | (uint32_t)value[3] << 24;
}

static int pread_all(int fd, void *buffer, size_t size, uint64_t offset)
{
	unsigned char *cursor = buffer;

	if (offset > (uint64_t)INT64_MAX) {
		errno = EOVERFLOW;
		return -1;
	}
	while (size) {
		ssize_t amount = pread(fd, cursor, size, (off_t)offset);

		if (amount < 0 && errno == EINTR)
			continue;
		if (amount <= 0) {
			errno = amount == 0 ? EIO : errno;
			return -1;
		}
		cursor += amount;
		size -= (size_t)amount;
		offset += (uint64_t)amount;
	}
	return 0;
}

static int find_central_directory(int fd, uint64_t file_size,
		uint64_t *offset, uint16_t *entries)
{
	size_t tail_size = file_size > 65557 ? 65557 : (size_t)file_size;
	unsigned char *tail;
	ssize_t index;
	int result = -1;

	if (tail_size < 22) {
		errno = EINVAL;
		return -1;
	}
	tail = calloc(1, tail_size);
	if (!tail)
		return -1;
	if (pread_all(fd, tail, tail_size, file_size - tail_size) < 0)
		goto out;
	for (index = (ssize_t)tail_size - 22; index >= 0; index--) {
		if (little32(tail + index) != ZIP_EOCD_SIGNATURE)
			continue;
		if ((size_t)index + 22 + little16(tail + index + 20) > tail_size)
			continue;
		if (little16(tail + index + 4) != 0 ||
		    little16(tail + index + 6) != 0 ||
		    little16(tail + index + 8) != little16(tail + index + 10)) {
			errno = ENOTSUP;
			goto out;
		}
		*entries = little16(tail + index + 10);
		*offset = little32(tail + index + 16);
		if (*entries == UINT16_MAX || *offset == UINT32_MAX) {
			errno = ENOTSUP;
			goto out;
		}
		result = 0;
		goto out;
	}
	errno = EINVAL;
out:
	free(tail);
	return result;
}

static int local_data_offset(int fd, uint64_t local_offset, uint64_t *data_offset)
{
	unsigned char header[30];

	if (pread_all(fd, header, sizeof(header), local_offset) < 0)
		return -1;
	if (little32(header) != ZIP_LOCAL_SIGNATURE) {
		errno = EINVAL;
		return -1;
	}
	*data_offset = local_offset + sizeof(header) + little16(header + 26) +
		little16(header + 28);
	return 0;
}

static int find_entry_fd(int fd, uint64_t file_size, const char *wanted,
		struct zip_entry *entry)
{
	uint64_t cursor;
	uint16_t count;
	uint16_t index;
	unsigned char header[46];
	size_t wanted_size = strlen(wanted);

	if (find_central_directory(fd, file_size, &cursor, &count) < 0)
		return -1;
	for (index = 0; index < count; index++) {
		uint16_t name_size;
		uint16_t extra_size;
		uint16_t comment_size;
		uint32_t local_offset;
		char *name;

		if (pread_all(fd, header, sizeof(header), cursor) < 0)
			return -1;
		if (little32(header) != ZIP_CENTRAL_SIGNATURE) {
			errno = EINVAL;
			return -1;
		}
		name_size = little16(header + 28);
		extra_size = little16(header + 30);
		comment_size = little16(header + 32);
		if (name_size > 4096) {
			errno = EINVAL;
			return -1;
		}
		name = malloc((size_t)name_size + 1);
		if (!name)
			return -1;
		if (pread_all(fd, name, name_size, cursor + sizeof(header)) < 0) {
			free(name);
			return -1;
		}
		name[name_size] = '\0';
		if (name_size == wanted_size && memcmp(name, wanted, wanted_size) == 0) {
			free(name);
			if (little32(header + 20) == UINT32_MAX ||
			    little32(header + 24) == UINT32_MAX ||
			    little32(header + 42) == UINT32_MAX) {
				errno = ENOTSUP;
				return -1;
			}
			entry->flags = little16(header + 8);
			entry->method = little16(header + 10);
			entry->compressed_size = little32(header + 20);
			entry->uncompressed_size = little32(header + 24);
			local_offset = little32(header + 42);
			if (entry->flags & 1) {
				errno = ENOTSUP;
				return -1;
			}
			return local_data_offset(fd, local_offset,
				&entry->data_offset);
		}
		free(name);
		cursor += sizeof(header) + name_size + extra_size + comment_size;
	}
	errno = ENOENT;
	return -1;
}

int zip_find_entry(const char *path, const char *wanted,
		struct zip_entry *entry)
{
	struct stat metadata;
	int fd;
	int result;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	if (fstat(fd, &metadata) < 0 || metadata.st_size < 0) {
		close(fd);
		return -1;
	}
	result = find_entry_fd(fd, (uint64_t)metadata.st_size, wanted, entry);
	close(fd);
	return result;
}

static int copy_stored(int source, uint64_t offset, uint64_t size, int destination)
{
	unsigned char buffer[65536];

	while (size) {
		size_t wanted = size > sizeof(buffer) ? sizeof(buffer) : (size_t)size;
		ssize_t amount;
		size_t written = 0;

		if (pread_all(source, buffer, wanted, offset) < 0)
			return -1;
		while (written < wanted) {
			amount = write(destination, buffer + written, wanted - written);
			if (amount < 0 && errno == EINTR)
				continue;
			if (amount <= 0)
				return -1;
			written += (size_t)amount;
		}
		offset += wanted;
		size -= wanted;
	}
	return 0;
}

static int copy_deflated(int source, const struct zip_entry *entry,
		int destination)
{
	unsigned char input[65536];
	unsigned char output[65536];
	uint64_t remaining = entry->compressed_size;
	uint64_t offset = entry->data_offset;
	z_stream stream = { 0 };
	int code;
	int result = -1;

	if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
		errno = EINVAL;
		return -1;
	}
	code = Z_OK;
	while (code != Z_STREAM_END) {
		if (stream.avail_in == 0 && remaining) {
			size_t amount = remaining > sizeof(input) ?
				sizeof(input) : (size_t)remaining;
			if (pread_all(source, input, amount, offset) < 0)
				goto out;
			stream.next_in = input;
			stream.avail_in = (uInt)amount;
			offset += amount;
			remaining -= amount;
		}
		stream.next_out = output;
		stream.avail_out = sizeof(output);
		code = inflate(&stream, Z_NO_FLUSH);
		if (code != Z_OK && code != Z_STREAM_END) {
			errno = EINVAL;
			goto out;
		}
		if (stream.avail_out != sizeof(output)) {
			size_t have = sizeof(output) - stream.avail_out;
			size_t written = 0;
			while (written < have) {
				ssize_t amount = write(destination, output + written,
					have - written);
				if (amount < 0 && errno == EINTR)
					continue;
				if (amount <= 0)
					goto out;
				written += (size_t)amount;
			}
		}
		if (!remaining && stream.avail_in == 0 && code != Z_STREAM_END) {
			errno = EINVAL;
			goto out;
		}
	}
	if (stream.total_out != entry->uncompressed_size) {
		errno = EINVAL;
		goto out;
	}
	result = 0;
out:
	inflateEnd(&stream);
	return result;
}

static int extract_fd(int source, const struct zip_entry *entry, int destination)
{
	if (entry->method == ZIP_STORED)
		return copy_stored(source, entry->data_offset,
			entry->compressed_size, destination);
	if (entry->method == ZIP_DEFLATED)
		return copy_deflated(source, entry, destination);
	errno = ENOTSUP;
	return -1;
}

int zip_extract_entry(const char *path, const char *wanted,
		const char *destination, mode_t mode)
{
	struct zip_entry entry;
	struct stat metadata;
	int source = -1;
	int output = -1;
	int result = -1;

	source = open(path, O_RDONLY | O_CLOEXEC);
	if (source < 0)
		goto out;
	if (fstat(source, &metadata) < 0 || metadata.st_size < 0)
		goto out;
	if (find_entry_fd(source, (uint64_t)metadata.st_size, wanted, &entry) < 0)
		goto out;
	output = open(destination, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
	if (output < 0)
		goto out;
	if (extract_fd(source, &entry, output) < 0)
		goto out;
	if (fsync(output) < 0)
		goto out;
	result = 0;
out:
	if (output >= 0)
		close(output);
	if (source >= 0)
		close(source);
	if (result < 0)
		unlink(destination);
	return result;
}

int zip_read_entry(const char *path, const char *wanted,
		unsigned char **data, size_t *size)
{
	struct zip_entry entry;
	char temporary[] = "/tmp/droidian-apex-entry-XXXXXX";
	unsigned char *buffer = NULL;
	int temporary_fd = -1;
	int source = -1;
	ssize_t amount;
	size_t used = 0;
	int result = -1;

	if (zip_find_entry(path, wanted, &entry) < 0)
		return -1;
	if (entry.uncompressed_size > 1024 * 1024 ||
	    entry.uncompressed_size > SIZE_MAX - 1) {
		errno = EFBIG;
		return -1;
	}
	temporary_fd = mkstemp(temporary);
	if (temporary_fd < 0)
		return -1;
	unlink(temporary);
	source = open(path, O_RDONLY | O_CLOEXEC);
	if (source < 0)
		goto out;
	if (extract_fd(source, &entry, temporary_fd) < 0)
		goto out;
	if (lseek(temporary_fd, 0, SEEK_SET) < 0)
		goto out;
	buffer = malloc((size_t)entry.uncompressed_size + 1);
	if (!buffer)
		goto out;
	while (used < (size_t)entry.uncompressed_size) {
		amount = read(temporary_fd, buffer + used,
			(size_t)entry.uncompressed_size - used);
		if (amount < 0 && errno == EINTR)
			continue;
		if (amount <= 0)
			goto out;
		used += (size_t)amount;
	}
	buffer[used] = '\0';
	*data = buffer;
	*size = used;
	buffer = NULL;
	result = 0;
out:
	free(buffer);
	if (source >= 0)
		close(source);
	if (temporary_fd >= 0)
		close(temporary_fd);
	return result;
}
