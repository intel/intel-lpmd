Name:           hepo
Version:        1
Release:        1
Summary:        HEPO application
License:        GPLv3

Source0:        pkg.OPT.HEPO-0.05.240605-x86_64.tar.gz
BuildArch:      noarch

%description
HEPO applicatoin
%prep
#nothing to do here
%setup -q
#untars the files to build folder
%build
cat > hello-world.sh <<EOF
#!/usr/bin/bash
echo Hello world
EOF
%install
mkdir -p %{buildroot}/usr/share/ia_pkg/tune/
#install -m 755 pkg.OPT.HEPO-0.05.240605-x86_64/* %{buildroot}/usr/share/ia_pkg/tune/
install -m 755 hello-world.sh %{buildroot}/usr/share/ia_pkg/tune/hello-world.sh
%files
/usr/bin/hello-world.sh
%changelog
* Wed Jun 05 2024 vino
- 