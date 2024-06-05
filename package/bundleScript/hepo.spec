Name:           hepo
Version:        1.0
Release:        1%{?dist}
Summary:        HEPO application

Group:          Utilities
License:        GPL v3
    
Source0:		pkg.OPT.HEPO-0.04.240604-x86_64.tar.gz

Requires: 		bash

BuildArch:      x86_64

#BuildRequires: 
#Requires:      

%description
HEPO applicatoin


%prep
%setup -q


%build


%install
rm -rf $RPM_BUILD_ROOT
install -d $RPM_BUILD_ROOT/usr/share/ia_pkg/tune
install *.tar.gz $RPM_BUILD_ROOT/o/usr/share/ia_pkg/tune


%clean
rm -rf $RPM_BUILD_ROOT

%files
%dir /usr/share/ia_pkg/tune
%defattr(-,root,root,-)
/usr/share/ia_pkg/tune/install.sh

%post
chmod 755 -R /usr/share/ia_pkg/tune
tar -xzvf $FILENAME.tar.gz /usr/share/ia_pkg/tune
rm /usr/share/ia_pkg/tune/*.tar.gz
/usr/share/ia_pkg/tune/install.sh