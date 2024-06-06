Name:           hepo
Version:        1
Release:        1
Summary:        HEPO application
License:        GPLv3

%description
HEPO applicatoin
%prep
#nothing to do here
#untars the files to build folder
%build
cat > hello-world.sh <<EOF
#!/usr/bin/bash
echo Hello world
EOF
%install
mkdir -p %{buildroot}/usr/share/ia_pkg/hepo/
#mkdir -p %{buildroot}/usr/share/ia_pkg/hepo/tuned-profile
#install -m 644 tuned-profile/* %{buildroot}/usr/share/ia_pkg/hepo/tuned-profile
#install -m 755 hello-world.sh %{buildroot}/usr/share/ia_pkg/hepo/hello-world.sh
#install -m 755 deploy.sh %{buildroot}/usr/share/ia_pkg/hepo/deploy.sh
#install -m 755 rollback.sh %{buildroot}/usr/share/ia_pkg/hepo/rollback.sh
install -m 644 * %{buildroot}/usr/share/ia_pkg/hepo/
%files
/usr/share/ia_pkg/hepo/Intel_OBL_Internal_Use_License_Agreement_[v2022.12.20].pdf
/usr/share/ia_pkg/hepo/deploy.sh
/usr/share/ia_pkg/hepo/hello-world.sh
/usr/share/ia_pkg/hepo/intel_lpmd
/usr/share/ia_pkg/hepo/intel_lpmd.8
/usr/share/ia_pkg/hepo/intel_lpmd.service
/usr/share/ia_pkg/hepo/intel_lpmd_config.xml
/usr/share/ia_pkg/hepo/intel_lpmd_config.xml.5
/usr/share/ia_pkg/hepo/intel_lpmd_control
/usr/share/ia_pkg/hepo/org.freedesktop.intel_lpmd.conf
/usr/share/ia_pkg/hepo/org.freedesktop.intel_lpmd.service
/usr/share/ia_pkg/hepo/readme-license.md
/usr/share/ia_pkg/hepo/release-notes.txt
/usr/share/ia_pkg/hepo/rollback.sh
/usr/share/ia_pkg/hepo/tuned-profile.tar.gz
/usr/share/ia_pkg/hepo/user-guide.md
%post
/usr/share/ia_pkg/tune/deploy.sh
%changelog
* Wed Jun 05 2024 vino
- 