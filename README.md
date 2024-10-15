# Intel Low Power Mode Daemon

Intel Low Power Model Daemon is a Linux daemon used to optimize active idle power.
It selects a set of most power efficient CPUs based on configuration file or CPU topology. Based on system utilization and other hints, it puts the system into Low Power Mode by activate the power efficient CPUs and disable the rest, and restore the system from Low Power Mode by activating all CPUs.

<p>Use man pages to check command line arguments and xml configurations</p>
<pre><code>man intel_lpmd
man intel_lpmd_config.xml</code></pre></p>

<p>Prerequisites: Prefers kernel start with TBD</p>

### Building and executing on Fedora
<p>1. Install</p>

<pre><code>dnf install automake
dnf install autoconf-archive
dnf install gcc
dnf install glib-devel
dnf install dbus-glib-devel
dnf install libxml2-devel
dnf install libnl3-devel
dnf install systemd-devel
dnf install gtk-doc
dnf install upower-devel
TBD
</code></pre>

<p>2. Build</p>

<pre><code>./autogen.sh prefix=/
make
sudo make install
</code></pre>

<p>The prefix value depends on the distribution version.
This can be "/" or "/usr". So please check existing
path of intel_lpmd install, if present to update and
add appropriate prefix.</p>

<p>3. Run</p>
<p>- start service</p>

<pre><code>sudo systemctl start intel_lpmd.service</code></pre>
<p>- Get status</p>
<pre><code>sudo systemctl status intel_lpmd.service</code></pre>
<p>- Stop service</p>
<pre><code>sudo systemctl stop intel_lpmd.service</code></pre>

<p>- Terminate using DBUS I/F</p>
<pre><code>sudo test/lpm_test_interface.sh 1</code></pre></p>

### Building on Ubuntu
<p>1. Install</p>
<pre><code>sudo apt install autoconf
sudo apt install autoconf-archive
sudo apt install gcc
sudo apt install libglib2.0-dev
sudo apt install libdbus-1-dev
sudo apt install libdbus-glib-1-dev
sudo apt install libxml2-dev
sudo apt install libnl-3-dev libnl-genl-3-dev
sudo apt install libsystemd-dev
sudo apt install gtk-doc-tools
sudo apt install libupower-glib-dev
</code></pre></p>

<p>For build and run, follow the same procedure as Fedora.</p>

### Building and executing on openSUSE
<p>1. Install</p>
<pre><code>
    zypper in automake
    zypper in gcc
    TBD
</code></pre>

<p>For build and run, follow the same procedure as Fedora.</p>

<hr />

<p>Releases</p>

Release 0.0.7
- Change lpmd description from "Low Power Mode Daemon" to "Energy
  Optimizer (lpmd)" because it covers more scenarios.
- Fix invalid cgroup setting during probe, in case lpmd doesn't quit
  smoothly and cleanups are not done properly in the previous run.
- Introduce a new parameter "--ignore-platform-check".
- Provide more detailed information when lpmd failed to probe on an
  unvalidated platform.
- Various of fixes for array bound check, potential memory leak, etc.
- Autotool improvements.

Release 0.0.6
- removes the automake and autoconf improvements because a regression is
  found.
- deprecates the dbus-glib dependency.

Release 0.0.5
- Fix compiling errors with -Wall.
- Remove unintended default config file change to keep it unchanged
  since v0.0.3.

Release 0.0.4
- Enhance HFI monitor so that it can handle back-to-back HFI LPM
  hints.
- Enhance HFI monitor to handle HFI hints for banned CPUs.
- Introduce support for multiple Low Power states.
- Introduce support for work load type hint.
- Allow change EPP during Low Power modes transition.
- Minor fixes and cleanups.

Release 0.0.3
- Conversion from glib-dbus to GDBus.
- Add handling for CPU hotplug.
- Use strict CPU model check to allow intel_lpmd running on
  - validated platforms only. Including ADL/RPL/MTL for now.
  - CPUID.7 Hybrid bit is set
  - /sys/firmware/acpi/pm_profile returns 2 (mobile platform)
- Use cpuid() to detect Lcores instead of using cache sysfs.
- Enhance Ecore module detection
- Fix pthread error handling, suggested by ColinIanKing.
- Werror Fixes from aekoroglu.

Release 0.0.2
- Various of fixes and cleanups.

Release 0.0.1
- Add initial lpmd support.

## Security

See Intel's [Security Center](https://www.intel.com/content/www/us/en/security-center/default.html)
for information on how to report a potential security issue or vulnerability.

See also: [Security Policy](SECURITY.md)
