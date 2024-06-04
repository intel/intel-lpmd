Name:           hepo
Version:        1.0
Release:        1%{?dist}
Summary:        HEPO application

Group:          Utilities
License:        GPL
URL:            
Source0:        
BuildArch:      noarch
BuildRoot:      %{_tmppath}/%{name}-%{version}-%{release}-root-%(%{__id_u} -n)


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