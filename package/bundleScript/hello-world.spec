Name:       hello-world
Version:    1
Release:    1
Summary:    Most simple RPM package
License:    FIXME

%description
This is my first RPM package, which does nothing.
%prep
# we have no source, so nothing here
%build
cat > hello-world.sh <<EOF
#!/usr/bin/bash
echo Hello world
EOF
%install
mkdir -p %{buildroot}/usr/bin/
install -m 755 hello-world.sh %{buildroot}/usr/bin/hello-world.sh
%files
/usr/bin/hello-world.sh
%changelog
* Wed Jun 05 2024 firstcut
-