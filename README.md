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

## Release 0.0.1

## Security

See Intel's [Security Center](https://www.intel.com/content/www/us/en/security-center/default.html)
for information on how to report a potential security issue or vulnerability.

See also: [Security Policy](SECURITY.md)
