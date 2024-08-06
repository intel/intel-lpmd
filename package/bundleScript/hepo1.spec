Name:           ileo1
Version:        1
Release:        1%{?dist}
Summary:        ILEO application

License:        
URL:            
Source0:        

BuildRequires:  
Requires:       

%description
ILEO application

%prep
%autosetup


%build
%configure
%make_build


%install
%make_install


%files
%license add-license-file-here
%doc add-docs-here



%changelog
* Wed Jun 05 2024 vino
- 
