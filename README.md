# Rootless_v2ci — Rootless Continuous Integration for Virtualsquare Projects

Rootless_v2ci is a continuous integration system designed to run time-based cross-compilations on Debian hosts without root privileges. It targets unofficial Virtualsquare projects, available at [https://github.com/virtualsquare](https://github.com/virtualsquare).

## Build Workflow Overview

The rootless_v2ci engine starts by loading a configuration file where the user specifies the projects to cross-compile together with several options for each project, such as:

- APT packages to install;
- Sources that must be built manually (and their sub-dependencies);
- Target architectures;
- Build refresh interval;
- Update detection strategy (on the main project repository or on those of its dependencies).

Then rootless_v2ci creates chroot environments through debootstrap for all requested architectures, merging architecture declarations across projects, and spawns a builder daemon for each project. Every builder daemon manages one thread per architecture; each thread produces the static binaries for its `<project, architecture>` pair, compiling the project and its dependencies inside the corresponding chroot environment, thanks to the `qemu-user-static` emulation (that must be installed on the host).

## Quickstart

### Prerequisites

#### System Dependencies

Install the minimal required dependencies:

```bash
sudo apt update
sudo apt upgrade
sudo apt install debootstrap qemu-user-static binfmt-support build-essential cmake git libexecs-dev libyaml-dev
```

#### Engine Configuration

The engine is controlled by directives defined in `config.yml`. Edit this file before the first run while keeping the structure demonstrated in the bundled example.

#### Update Default Paths

Edit `src/include/types/types.h` to match your environment:

```c
#define DEFAULT_CONFIG_PATH "~/.config/v2ci/config.yml"         // Replace with the absolute path to config.yml
#define SCRIPTS_DIR_PATH "/usr/lib/v2ci/scripts"                // Replace with the absolute path to the scripts directory
```

#### AppArmor restrictions on Ubuntu >= 24.04
If the user wishes to run rootless_v2ci on a host with `Ubuntu 24.04` or later, a kernel configuration related to AppArmor must be changed. In recent Ubuntu versions, AppArmor enforces stricter security profiles, adding (by default) checks on user namespace creation by unprivileged users/processes. Since rootless_v2ci aims to complete the debootstrap phase for cross‑compilation without elevated privileges, the program, instead of using chroot, adopts a user‑namespace‑based approach. Therefore, before launching the program, you must change the AppArmor configuration (only on Ubuntu hosts >= 24.04) by executing:
```bash
sudo sysctl -w kernel.apparmor_restrict_unprivileged_userns=0
```

### Build from Source

To build rootless_v2ci locally instead of using the pre-built static binaries:

```bash
cd rootless_V2CI
mkdir build
cd build
cmake ..
make
```

> **Note:** If you encounter linking issues with `libyaml`, adjust the following line inside `CMakeLists.txt` so that it points to the correct location of `libyaml.a` on your host:
> ```cmake
> set(LIBYAML "/usr/lib/x86_64-linux-gnu/libyaml.a")
> ```
> If you also encounter linking issues with the execs library, ensure the `libexecs-dev` package is properly installed (or build it from source: https://salsa.debian.org/virtualsquare-team/s2argv-execs.git and install it manually). Then edit the `target_link_libraries` entries in `CMakeLists.txt`, replacing `execs` with the absolute path to `libexecs.a` (e.g. `/usr/local/lib/libexecs.a`).

### Run

After compilation, two statically linked binaries are available in `build/`.

Start the daemons:

```bash
./v2ci_start
```

Stop the daemons:

```bash
./v2ci_stop
```

#### Do I Need `sudo`?

No. rootless_v2ci leverages an `_enter` script generated inside each rootfs environment to perform a chroot-like operation through user namespaces without requiring root privileges.

### Log Monitoring

rootless_v2ci starts by printing to `stdout` and announces the log file path before switching output destinations. Logs are written to:

1. `<build_dir>/logs/main.log` — setup of chroot environments and daemon startup.
2. `<build_dir>/<project_name>/logs/worker.log` — main builder daemon logs for the project.
3. `<build_dir>/<project_name>/logs/<arch>-worker.log` — outer execution logs for the architecture-specific thread.
4. `/home/<project_name>/logs/worker.log` — inner execution logs inside the chroot for that thread.

Here, `<build_dir>` is the build directory defined in `config.yml` and `<project_name>` is the project name being cross-compiled.

### Final Binaries

Upon completion, static binaries are stored in the directory specified by each project’s `target_dir`. For a project named `<project_name>`, the static binary for architecture `<arch>` resides at:

```
<target_dir>/<release>/<project_name>-<arch>
```

`<release>` corresponds to the latest GitHub release tag or defaults to `unstable` when no tag exists.
