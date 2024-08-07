Name:           ileo
Version:        1
Release:        1
Summary:        ILEO application
License:        GPLv3

%description
ILEO applicatoin
%prep
#nothing to do here
#untars the files to build folder
%build
cat > hello-world.sh <<EOF
#!/usr/bin/bash
echo Hello world
EOF
%install
mkdir -p %{buildroot}/usr/share/ia_pkg/ileo/
#mkdir -p %{buildroot}/usr/share/ia_pkg/ileo/tuned-profile
#install -m 644 tuned-profile/* %{buildroot}/usr/share/ia_pkg/ileo/tuned-profile
#install -m 755 hello-world.sh %{buildroot}/usr/share/ia_pkg/ileo/hello-world.sh
#install -m 755 deploy.sh %{buildroot}/usr/share/ia_pkg/ileo/deploy.sh
#install -m 755 rollback.sh %{buildroot}/usr/share/ia_pkg/ileo/rollback.sh
install -m 644 * %{buildroot}/usr/share/ia_pkg/ileo/
%files
/usr/share/ia_pkg/ileo/Intel_OBL_Internal_Use_License_Agreement_[v2022.12.20].pdf
/usr/share/ia_pkg/ileo/deploy.sh
/usr/share/ia_pkg/ileo/hello-world.sh
/usr/share/ia_pkg/ileo/intel_lpmd
/usr/share/ia_pkg/ileo/intel_lpmd.8
/usr/share/ia_pkg/ileo/intel_lpmd.service
/usr/share/ia_pkg/ileo/intel_lpmd_config.xml
/usr/share/ia_pkg/ileo/intel_lpmd_config.xml.5
/usr/share/ia_pkg/ileo/intel_lpmd_control
/usr/share/ia_pkg/ileo/org.freedesktop.intel_lpmd.conf
/usr/share/ia_pkg/ileo/org.freedesktop.intel_lpmd.service
/usr/share/ia_pkg/ileo/readme-license.md
/usr/share/ia_pkg/ileo/release-notes.txt
/usr/share/ia_pkg/ileo/rollback.sh
/usr/share/ia_pkg/ileo/tuned-profile.tar.gz
/usr/share/ia_pkg/ileo/user-guide.md
%post
/usr/share/ia_pkg/tune/deploy.sh
%changelog
* Wed Jun 05 2024 vino
- 