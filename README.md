# Intel Low Power Mode Daemon

Intel Low Power Mode Daemon (lpmd) is a Linux daemon designed to optimize active idle power. It selects the most power-efficient CPUs based on a configuration file or CPU topology. Depending on system utilization and other hints, it puts the system into Low Power Mode by activating the power-efficient CPUs and disabling the rest, and restores the system from Low Power Mode by activating all CPUs.

## Usage

Refer to the man pages for command line arguments and XML configurations:

```sh
man intel_lpmd
man intel_lpmd_control
man intel_lpmd_config.xml
```

## Install Dependencies

### Fedora

```sh
dnf install automake autoconf-archive gcc glib2-devel dbus-glib-devel libxml2-devel libnl3-devel systemd-devel gtk-doc upower-devel
```

### Ubuntu

```sh
sudo apt install autoconf autoconf-archive gcc libglib2.0-dev libdbus-1-dev libdbus-glib-1-dev libxml2-dev libnl-3-dev libnl-genl-3-dev libsystemd-dev gtk-doc-tools libupower-glib-dev
```

### OpenSUSE

```sh
zypper in automake gcc
```

## Build and Install

```sh
./autogen.sh
make
sudo make install
```

The generated artifacts are copied to respective directories under `/usr/local`. If a custom install path is preferred other than system default,  make sure `--localstatedir` and `--sysconfdir` are set to the right path that the system can understand. If installed via RPM then artifacts would be under `/usr`.

Example command for installation using prefix under `/opt/lpmd_install` dir with `--localstatedir` and `--sysconfdir` set to system default


```sh
./autogen.sh prefix=/opt/lpmd_install --localstatedir=/var --sysconfdir=/etc
```

## Run

### Start Service

```sh
sudo systemctl start intel_lpmd.service
```

### Get Status

```sh
sudo systemctl status intel_lpmd.service
```

### Stop Service

```sh
sudo systemctl stop intel_lpmd.service
```

### Terminate using DBUS Interface

```sh
sudo tests/lpm_test_interface.sh 1
```

## Testing Installation from Source

Launch `lpmd` in no-daemon mode:
```sh
./intel_lpmd --no-daemon --dbus-enable --loglevel=debug
```

Start `lpmd` using:
```sh
sudo sh tests/lpm_test_interface.sh 4
```

Run a workload and monitor `lpmd` to ensure it puts the system in the appropriate state based on the load.


## Releases

### Release 0.0.8

- Introduce workload type proxy support.
- Add support for model/sku specific config file.
- Add detection for AC/DC status.
- Honor power profile daemon default EPP when restoring.
- Introduce MeteorLake-P platform specific config file.
- Minor fixes and cleanups.

### Release 0.0.7

- Change lpmd description from "Low Power Mode Daemon" to "Energy Optimizer (lpmd)" because it covers more scenarios.
- Fix invalid cgroup setting during probe, in case lpmd doesn't quit smoothly and cleanups are not done properly in the previous run.
- Introduce a new parameter `--ignore-platform-check`.
- Provide more detailed information when lpmd fails to probe on an unvalidated platform.
- Various fixes for array bound check, potential memory leak, etc.
- Autotool improvements.

### Release 0.0.6

- Remove automake and autoconf improvements due to a regression.
- Deprecate the dbus-glib dependency.

### Release 0.0.5

- Fix compiling errors with `-Wall`.
- Remove unintended default config file change to keep it unchanged since v0.0.3.

### Release 0.0.4

- Enhance HFI monitor to handle back-to-back HFI LPM hints.
- Enhance HFI monitor to handle HFI hints for banned CPUs.
- Introduce support for multiple Low Power states.
- Introduce support for workload type hint.
- Allow change EPP during Low Power modes transition.
- Minor fixes and cleanups.

### Release 0.0.3

- Convert from glib-dbus to GDBus.
- Add handling for CPU hotplug.
- Use strict CPU model check to allow intel_lpmd to run on validated platforms only, including ADL/RPL/MTL for now.
- CPUID.7 Hybrid bit is set
- /sys/firmware/acpi/pm_profile returns 2 (mobile platform)
- Use `cpuid()` to detect Lcores instead of using cache sysfs.
- Enhance Ecore module detection.
- Fix pthread error handling, suggested by ColinIanKing.
- Werror fixes from aekoroglu.

### Release 0.0.2

- Various fixes and cleanups.

### Release 0.0.1

- Add initial lpmd support.

## Security

See Intel's [Security Center](https://www.intel.com/content/www/us/en/security-center/default.html) for information on how to report a potential security issue or vulnerability.

See also: [Security Policy](security.md)
