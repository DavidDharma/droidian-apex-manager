#include "apex-manager.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void test_json(void)
{
	static const unsigned char manifest[] =
		"{\n  \"version\": 123456789,\n  \"name\": \"com.example.runtime\"\n}";
	char name[256];
	uint64_t version;

	assert(apex_manifest_parse(manifest, sizeof(manifest) - 1, true,
		name, sizeof(name), &version) == 0);
	assert(strcmp(name, "com.example.runtime") == 0);
	assert(version == 123456789);
}

static void test_json_string_version(void)
{
	static const unsigned char manifest[] =
		"{\"name\":\"com.example.i18n\",\"versionCode\":\"42\"}";
	char name[256];
	uint64_t version;

	assert(apex_manifest_parse(manifest, sizeof(manifest) - 1, true,
		name, sizeof(name), &version) == 0);
	assert(strcmp(name, "com.example.i18n") == 0);
	assert(version == 42);
}

static void test_protobuf(void)
{
	/* field 1: name; field 2: version (300) */
	static const unsigned char manifest[] = {
		0x0a, 0x0b, 'c', 'o', 'm', '.', 't', 'e', 's', 't', '.', 'p', 'b',
		0x10, 0xac, 0x02,
	};
	char name[256];
	uint64_t version;

	assert(apex_manifest_parse(manifest, sizeof(manifest), false,
		name, sizeof(name), &version) == 0);
	assert(strcmp(name, "com.test.pb") == 0);
	assert(version == 300);
}

static void test_invalid_name(void)
{
	static const unsigned char manifest[] =
		"{\"name\":\"../../escape\",\"version\":1}";
	char name[256];
	uint64_t version;

	assert(apex_manifest_parse(manifest, sizeof(manifest) - 1, true,
		name, sizeof(name), &version) < 0);
}

int main(void)
{
	test_json();
	test_json_string_version();
	test_protobuf();
	test_invalid_name();
	puts("manifest tests passed");
	return 0;
}
