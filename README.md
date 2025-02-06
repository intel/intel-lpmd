# Intel Low Power Mode Daemon

Intel Low Power Model Daemon is a Linux daemon used to optimize active idle power.
It selects a set of most power efficient CPUs based on configuration file or CPU topology. Based on system utilization and other hints, it puts the system into Low Power Mode by activate the power efficient CPUs and disable the rest, and restore the system from Low Power Mode by activating all CPUs.

<p>Use man pages to check command line arguments and xml configurations</p>
<pre><code>man intel_lpmd
man intel_lpmd_config.xml</code></pre></p>

<p>Prerequisites: Prefers kernel start with TBD</p>

### Fedora

<strong>intel-lpmd</strong> is available on Fedora. To install it, run

<pre><code>sudo dnf install intel-lpmd</code></pre>

<p>To start the service</p>
<pre><code>sudo systemctl start intel_lpmd</code></pre>
<p>To check the status</p>
<pre><code>sudo systemctl status intel_lpmd</code></pre>
<p>To stop the service</p>
<pre><code>sudo systemctl stop intel_lpmd</code></pre>

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

Release 0.0.8
- Introduce workload type proxy support.
- Add support for model/sku specific config file.
- Add detection for AC/DC status.
- Honor power profile daemon default EPP when restoring.
- Introduce MeteorLake-P platform specific config file.
- Minor fixes and cleanups.

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
