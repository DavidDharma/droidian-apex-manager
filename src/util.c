#include "apex-manager.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

bool apex_verbose;

void apex_log(const char *level, const char *fmt, ...)
{
	va_list arguments;

	fprintf(stderr, "%s: ", level);
	va_start(arguments, fmt);
	vfprintf(stderr, fmt, arguments);
	va_end(arguments);
	fputc('\n', stderr);
}

void apex_debug(const char *fmt, ...)
{
	va_list arguments;

	if (!apex_verbose)
		return;
	fputs("debug: ", stderr);
	va_start(arguments, fmt);
	vfprintf(stderr, fmt, arguments);
	va_end(arguments);
	fputc('\n', stderr);
}

bool path_join(char *out, size_t size, const char *left, const char *right)
{
	int written;

	while (*right == '/')
		right++;
	written = snprintf(out, size, "%s%s%s", left,
		left[0] && left[strlen(left) - 1] == '/' ? "" : "/", right);
	return written >= 0 && (size_t)written < size;
}

bool has_suffix(const char *value, const char *suffix)
{
	size_t value_len = strlen(value);
	size_t suffix_len = strlen(suffix);

	return value_len >= suffix_len &&
		strcmp(value + value_len - suffix_len, suffix) == 0;
}

bool valid_apex_name(const char *name)
{
	const unsigned char *cursor = (const unsigned char *)name;

	if (!*cursor || strlen(name) > APEX_NAME_MAX || name[0] == '.')
		return false;
	for (; *cursor; cursor++) {
		if ((*cursor >= 'a' && *cursor <= 'z') ||
		    (*cursor >= 'A' && *cursor <= 'Z') ||
		    (*cursor >= '0' && *cursor <= '9') ||
		    *cursor == '.' || *cursor == '_' || *cursor == '-')
			continue;
		return false;
	}
	return strstr(name, "..") == NULL;
}

int mkdir_p(const char *path, mode_t mode)
{
	char copy[PATH_MAX];
	char *cursor;

	if (strlen(path) >= sizeof(copy)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	strcpy(copy, path);
	for (cursor = copy + 1; *cursor; cursor++) {
		if (*cursor != '/')
			continue;
		*cursor = '\0';
		if (mkdir(copy, mode) < 0 && errno != EEXIST)
			return -1;
		*cursor = '/';
	}
	if (mkdir(copy, mode) < 0 && errno != EEXIST)
		return -1;
	return 0;
}

int ensure_empty_file(const char *path, mode_t mode)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC, mode);

	if (fd < 0)
		return -1;
	return close(fd);
}

static void unescape_mount_path(char *path)
{
	char *source = path;
	char *destination = path;

	while (*source) {
		if (source[0] == '\\' && source[1] >= '0' && source[1] <= '7' &&
		    source[2] >= '0' && source[2] <= '7' &&
		    source[3] >= '0' && source[3] <= '7') {
			*destination++ = (char)((source[1] - '0') * 64 +
				(source[2] - '0') * 8 + source[3] - '0');
			source += 4;
		} else {
			*destination++ = *source++;
		}
	}
	*destination = '\0';
}

bool path_is_mountpoint(const char *path)
{
	FILE *mountinfo;
	char *line = NULL;
	size_t capacity = 0;
	bool result = false;

	mountinfo = fopen("/proc/self/mountinfo", "re");
	if (!mountinfo)
		return false;
	while (getline(&line, &capacity, mountinfo) >= 0) {
		char *save = NULL;
		char *field;
		int index = 0;

		for (field = strtok_r(line, " ", &save); field;
		     field = strtok_r(NULL, " ", &save)) {
			index++;
			if (index != 5)
				continue;
			unescape_mount_path(field);
			if (strcmp(field, path) == 0)
				result = true;
			break;
		}
		if (result)
			break;
	}
	free(line);
	fclose(mountinfo);
	return result;
}

int run_capture(char *const argv[], char *output, size_t output_size,
		int *exit_status)
{
	int pipefd[2];
	pid_t child;
	ssize_t amount;
	size_t used = 0;
	int status;

	if (!output || output_size == 0) {
		errno = EINVAL;
		return -1;
	}
	output[0] = '\0';
	if (pipe2(pipefd, O_CLOEXEC) < 0)
		return -1;
	child = fork();
	if (child < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return -1;
	}
	if (child == 0) {
		close(pipefd[0]);
		if (dup2(pipefd[1], STDOUT_FILENO) < 0)
			_exit(126);
		close(pipefd[1]);
		execvp(argv[0], argv);
		_exit(errno == ENOENT ? 127 : 126);
	}
	close(pipefd[1]);
	while ((amount = read(pipefd[0], output + used,
			output_size - used - 1)) > 0) {
		used += (size_t)amount;
		if (used == output_size - 1)
			break;
	}
	output[used] = '\0';
	close(pipefd[0]);
	for (;;) {
		if (waitpid(child, &status, 0) >= 0)
			break;
		if (errno != EINTR)
			return -1;
	}
	if (exit_status)
		*exit_status = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
	while (used && (output[used - 1] == '\n' || output[used - 1] == '\r' ||
			output[used - 1] == ' ' || output[used - 1] == '\t'))
		output[--used] = '\0';
	return 0;
}

int run_passthrough(char *const argv[])
{
	pid_t child = fork();
	int status;

	if (child < 0)
		return -1;
	if (child == 0) {
		execvp(argv[0], argv);
		_exit(errno == ENOENT ? 127 : 126);
	}
	for (;;) {
		if (waitpid(child, &status, 0) >= 0)
			break;
		if (errno != EINTR)
			return -1;
	}
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	return 128;
}
