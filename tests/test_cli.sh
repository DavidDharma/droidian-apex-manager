#!/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
temporary_dir=$(mktemp -d)
trap 'rm -rf "$temporary_dir"' EXIT HUP INT TERM

output=$($project_dir/droidian-apex \
	--android-root "$project_dir/tests/fixtures/android" list)
printf '%s\n' "$output" | grep -q 'flattened.*7.*com.example.flattened'

json=$($project_dir/droidian-apex \
	--android-root "$project_dir/tests/fixtures/android" list --json)
printf '%s\n' "$json" | grep -q '"name":"com.example.flattened"'

mkdir -p "$temporary_dir/system/apex"
python3 - "$temporary_dir/system/apex" <<'PY'
import io
import pathlib
import zipfile
import sys

root = pathlib.Path(sys.argv[1])
payload = bytearray(2048)
payload[1024 + 56:1024 + 58] = b"\x53\xef"

def make_apex(path, name, version):
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr("apex_manifest.json",
                         '{"name":"%s","version":%d}' % (name, version),
                         compress_type=zipfile.ZIP_DEFLATED)
        archive.writestr("apex_payload.img", payload,
                         compress_type=zipfile.ZIP_STORED)

make_apex(root / "com.example.packaged.apex", "com.example.packaged", 350090000)
nested = io.BytesIO()
with zipfile.ZipFile(nested, "w") as archive:
    archive.writestr("apex_manifest.json",
                     '{"name":"com.example.compressed","version":12}',
                     compress_type=zipfile.ZIP_STORED)
    archive.writestr("apex_payload.img", payload,
                     compress_type=zipfile.ZIP_STORED)
with zipfile.ZipFile(root / "com.example.compressed.capex", "w") as archive:
    archive.writestr("apex_manifest.json",
                     '{"name":"com.example.compressed","version":12}',
                     compress_type=zipfile.ZIP_STORED)
    archive.writestr("original_apex", nested.getvalue(),
                     compress_type=zipfile.ZIP_DEFLATED)
PY

output=$($project_dir/droidian-apex --android-root "$temporary_dir" list)
printf '%s\n' "$output" | grep -q 'packaged.*350090000.*com.example.packaged'
printf '%s\n' "$output" | grep -q 'compressed.*12.*com.example.compressed'
printf '%s\n' "$output" | awk '
	NR == 1 { name_column = index($0, "NAME"); next }
	{
		if (index($0, $3) != name_column)
			exit 1
	}
	END { if (!name_column) exit 1 }
'

output=$($project_dir/droidian-apex --dry-run --mount-root "$temporary_dir/mnt" \
	mount "$temporary_dir/system/apex/com.example.packaged.apex")
printf '%s\n' "$output" | grep -q 'loop-mount.*type=ext4'
printf '%s\n' "$output" | grep -q 'bind.*com.example.packaged@350090000.*com.example.packaged'

output=$($project_dir/droidian-apex --dry-run --mount-root "$temporary_dir/mnt" \
	mount "$temporary_dir/system/apex/com.example.compressed.capex")
printf '%s\n' "$output" | grep -q 'loop-mount.*type=ext4'

fake_bin="$temporary_dir/bin"
mkdir -p "$fake_bin"
printf '%s\n' \
	'#!/bin/sh' \
	'case " $* " in' \
	'  *" -sH "*) printf "%s\n" RUNNING ;;' \
	'  *" -pH "*) printf "%s\n" "$DROIDIAN_TEST_PID" ;;' \
	'  *) exit 2 ;;' \
	'esac' > "$fake_bin/lxc-info"
printf '%s\n' \
	'#!/bin/sh' \
	'case " $* " in' \
	'  *" /system/bin/getprop apexd.status "*) printf "%s\n" ready ;;' \
	'  *" /system/bin/dumpsys apexservice "*) printf "%s\n" "apexservice dump" ;;' \
	'  *) exit 2 ;;' \
	'esac' > "$fake_bin/lxc-attach"
chmod 0755 "$fake_bin/lxc-info" "$fake_bin/lxc-attach"

DROIDIAN_TEST_PID=$$ PATH="$fake_bin:$PATH" \
	$project_dir/droidian-apex --timeout 1 wait ready
status=$(DROIDIAN_TEST_PID=$$ PATH="$fake_bin:$PATH" \
	$project_dir/droidian-apex status)
printf '%s\n' "$status" | grep -q '^container.state=RUNNING$'
printf '%s\n' "$status" | grep -q "^container.pid=$$\$"
printf '%s\n' "$status" | grep -q '^apexd.status=ready$'
dump=$(DROIDIAN_TEST_PID=$$ PATH="$fake_bin:$PATH" \
	$project_dir/droidian-apex apexd dump --proto)
printf '%s\n' "$dump" | grep -q '^apexservice dump$'

echo "CLI tests passed"
