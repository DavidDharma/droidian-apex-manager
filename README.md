# Droidian APEX manager

`droidian-apex` discovers and mounts Android Pony EXpress (APEX) modules for
Droidian systems that run Android in the Halium LXC container. It is designed
primarily for `lxc-android` startup integration, while exposing enough detail
to be useful during device porting.

The installed executable is `/usr/sbin/droidian-apex`.

## Features

- Finds APEXs before the container or `apexd` has started by scanning the
  Android system, system_ext, product, vendor, odm, and data/active locations.
- Reads both protobuf and JSON APEX manifests.
- Recognizes flattened directories, regular `.apex` packages, and compressed
  `.capex` packages.
- Bind-mounts flattened APEXs and loop-mounts ext4 or EROFS payload images.
- Waits for `apexd.status` inside a named Halium LXC container without linking
  against Android or libhybris libraries.
- Mirrors the live, verified APEX mounts from the container mount namespace to
  the host, regardless of whether `apexd` activated flattened or packaged
  modules.
- Reports kernel, loop-device, filesystem, container, property, and discovery
  information for porting diagnostics.

## Build and test

The only library dependency is zlib, used to read deflated ZIP entries and
compressed APEXs.

```sh
make
make check
sudo make install
```

## Commands

List every preinstalled APEX without talking to the container:

```sh
droidian-apex list
droidian-apex list --json
```

Use a nonstandard Android partition root while porting:

```sh
droidian-apex --android-root /mnt/android list
```

Mount all discovered modules directly on the host:

```sh
droidian-apex mount --all
```

`mount` is intended for trusted APEXs from read-only Android partitions and for
recovery/porting environments. It does not reproduce `apexd` signature or
dm-verity validation. For normal container startup, let `apexd` activate the
modules and use `sync` instead.

Wait for Android's APEX manager and mirror its canonical mounts:

```sh
droidian-apex --timeout 90 sync
```

Inspect a port:

```sh
droidian-apex diagnose
droidian-apex status
droidian-apex apexd dump
```

On some Halium ports, `dumpsys apexservice` reaches the service but its
in-memory active-package list is empty. When that happens, `apexd dump` keeps
the native output and appends a `MOUNTED PACKAGES` section derived from the
container's live `/apex` namespace. Additional dumpsys arguments disable this
fallback and are passed through unchanged.

All global settings also have environment forms:
`DROIDIAN_APEX_ANDROID_ROOT`, `DROIDIAN_APEX_MOUNT_ROOT`,
`DROIDIAN_APEX_CONTAINER`, and `DROIDIAN_APEX_TIMEOUT`.

## lxc-android integration

The `sync` command is the normal integration point for
`/usr/lib/lxc-android/lxc-android-notify`. It replaces the old shell loop that
waits with `waitforservice` and guesses a flattened source directory:

```sh
if command -v droidian-apex >/dev/null 2>&1; then
    /usr/sbin/droidian-apex --timeout 90 sync
else
    # Keep the existing compatibility path for installations without this package.
    /usr/bin/waitforservice apexd.status
fi
```

`sync` performs these operations as one idempotent step:

1. Wait for the `android` LXC container to enter `RUNNING` state.
2. Poll `apexd.status` inside the container until it reaches `ready`.
3. Resolve the container init PID and inspect its `/apex` through
   `/proc/PID/root/apex`.
4. Read-only bind-mount every canonical APEX directory and
   `apex-info-list.xml` onto the host `/apex` tree.

This namespace-based approach supports flattened, packaged, compressed, and
future APEX payload filesystems without guessing their source path.

## Mount behavior and security

Direct packaged mounting locates the uncompressed `apex_payload.img` ZIP entry,
configures a read-only autoclearing loop device with the correct offset and size,
detects ext4 versus EROFS from its superblock, and mounts it with `nodev` and
`nosuid`. CAPEX files are decompressed under `/run/droidian-apex-manager` only
for the duration of loop setup.

The manifest-derived package name is strictly validated before it is used as a
mount path. Mount operations require root; discovery, status, dry runs, and most
diagnostics do not.

## License

MIT

## Author

Erik Inkinen <erik.inkinen@erikinkinen.fi>
