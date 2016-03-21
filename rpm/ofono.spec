Name:       ofono

Summary:    Open Source Telephony
Version:    1.17
Release:    1
Group:      Communications/Connectivity Adaptation
License:    GPLv2
URL:        https://git.merproject.org/mer-core/ofono
Source:     %{name}-%{version}.tar.bz2
Requires:   dbus
Requires:   systemd
Requires:   ofono-configs
Requires:   libgrilio >= 1.0.8
Requires:   libglibutil >= 1.0.6
Requires(preun): systemd
Requires(post): systemd
Requires(postun): systemd
BuildRequires:  pkgconfig(glib-2.0)
BuildRequires:  pkgconfig(dbus-1)
BuildRequires:  pkgconfig(libudev) >= 145
BuildRequires:  pkgconfig(bluez) >= 4.85
BuildRequires:  pkgconfig(mobile-broadband-provider-info)
BuildRequires:  pkgconfig(libwspcodec) >= 2.0
BuildRequires:  pkgconfig(libgrilio) >= 1.0.8
BuildRequires:  pkgconfig(libglibutil) >= 1.0.6
BuildRequires:  libtool
BuildRequires:  automake
BuildRequires:  autoconf

%description
Telephony stack

%package devel
Summary:    Headers for oFono
Group:      Development/Libraries
Requires:   %{name} = %{version}-%{release}

%description devel
Development headers and libraries for oFono

%package tests
Summary:    Test Scripts for oFono
Group:      Development/Libraries
Requires:   %{name} = %{version}-%{release}
Requires:   dbus-python3
Requires:   python3-gobject
Provides:   ofono-test >= 1.0
Obsoletes:  ofono-test < 1.0

%description tests
Scripts for testing oFono and its functionality

%package configs-mer
Summary:    Package to provide default configs for ofono
Group:      Development/Tools
Requires:   %{name} = %{version}-%{release}
Provides:   ofono-configs

%description configs-mer
This package provides default configs for ofono

%prep
%setup -q -n %{name}-%{version}/%{name}

./bootstrap

%build
autoreconf --force --install

%configure --disable-static \
    --enable-test \
    --enable-logcontrol \
    --enable-jolla-rilmodem \
    --with-systemdunitdir="/%{_lib}/systemd/system"

make %{?jobs:-j%jobs}


%check
# run unit tests
make check

%install
rm -rf %{buildroot}
%make_install

mkdir -p %{buildroot}/%{_sysconfdir}/ofono/push_forwarder.d
mkdir -p %{buildroot}/%{_lib}/systemd/system/network.target.wants
ln -s ../ofono.service %{buildroot}/%{_lib}/systemd/system/network.target.wants/ofono.service

%preun
if [ "$1" -eq 0 ]; then
systemctl stop ofono.service ||:
fi

%post
systemctl daemon-reload ||:
# Do not restart during update
# We don't want to break anything during update
# New daemon is taken in use after reboot
# systemctl reload-or-try-restart ofono.service ||:

%postun
systemctl daemon-reload ||:

%files
%defattr(-,root,root,-)
%doc COPYING ChangeLog AUTHORS README
%config %{_sysconfdir}/dbus-1/system.d/*.conf
%{_sbindir}/*
/%{_lib}/systemd/system/network.target.wants/ofono.service
/%{_lib}/systemd/system/ofono.service
%dir %{_sysconfdir}/ofono/
%dir %{_sysconfdir}/ofono/push_forwarder.d
# This file is part of phonesim and not needed with ofono.
%exclude %{_sysconfdir}/ofono/phonesim.conf
%doc /usr/share/man/man8/ofonod.8.gz
%dir %attr(775,radio,radio) /var/lib/ofono

%files devel
%defattr(-,root,root,-)
%{_includedir}/ofono/
%{_libdir}/pkgconfig/ofono.pc

%files tests
%defattr(-,root,root,-)
%{_libdir}/%{name}/test/*

%files configs-mer
%defattr(-,root,root,-)
%config /etc/ofono/ril_subscription.conf
