Name:           hepo
Version:        1
Release:        1
Summary:        HEPO application
Group:          Utilities
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
%install
mkdir -p $RPM_BUILD_ROOT/usr/share/ia_pkg/tune
install $RPM_BUILD/pkg.OPT.HEPO-0.05.240605-x86_64/* $RPM_BUILD_ROOT/usr/share/ia_pkg/tune

%post
#chmod 755 -R /usr/share/ia_pkg/tune
#tar -xzvf $FILENAME.tar.gz /usr/share/ia_pkg/tune
#rm /usr/share/ia_pkg/tune/*.tar.gz
#/usr/share/ia_pkg/tune/install.sh

%files
#%%license add-license-file-here
#%%doc add-docs-here

%changelog
* Wed Jun 05 2024 vino
- 