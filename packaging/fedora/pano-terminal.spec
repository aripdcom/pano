Name:           pano-terminal
Version:        0.1.1
Release:        1%{?dist}
Summary:        Terminal emulator for GNOME with split panes and tabs on any side

# The program is GPL-3.0-or-later, the help GPL-3.0-only or CC-BY-SA-3.0,
# the AppStream data GFDL-1.3-only, the color schemes MIT or Apache-2.0.
License:        GPL-3.0-or-later AND (GPL-3.0-only OR CC-BY-SA-3.0) AND GFDL-1.3-only AND (MIT OR Apache-2.0)
URL:            https://pano.aripd.com
Source0:        %{name}-%{version}.tar.xz

BuildRequires:  meson
BuildRequires:  gcc-c++
BuildRequires:  pkgconfig(gtk4) >= 4.12
BuildRequires:  pkgconfig(libadwaita-1) >= 1.4
BuildRequires:  pkgconfig(vte-2.91-gtk4) >= 0.80
BuildRequires:  pkgconfig(libpcre2-8)
BuildRequires:  pkgconfig(uuid)
BuildRequires:  pkgconfig(gsettings-desktop-schemas)
BuildRequires:  gettext
BuildRequires:  itstool
BuildRequires:  desktop-file-utils
BuildRequires:  libxslt
BuildRequires:  docbook-style-xsl
BuildRequires:  systemd-rpm-macros

Requires:       gsettings-desktop-schemas

%description
Pano Terminal is a terminal emulator for GNOME, built with GTK 4, libadwaita
and VTE. It is a fork of GNOME Terminal with split panes, tabs at the top,
bottom, left or right, session restore, color schemes that follow the light
and dark style, background transparency, command notifications and more.
It can be installed next to GNOME Terminal.

%prep
%autosetup -p1

%build
%meson -Dsearch_provider=true -Dnautilus_extension=false
%meson_build

%install
%meson_install
%find_lang %{name} --with-gnome

%check
%meson_test

%files -f %{name}.lang
%license COPYING COPYING.GFDL
%doc README.md
%{_bindir}/pano-terminal
%{_libexecdir}/pano-terminal-preferences
%{_libexecdir}/pano-terminal-server
%{_libdir}/pano-terminal/
%{_datadir}/applications/com.aripd.Pano.desktop
%{_datadir}/applications/com.aripd.Pano.Preferences.desktop
%{_datadir}/applications/com.aripd.Pano.QuickTerminal.desktop
%{_datadir}/dbus-1/services/com.aripd.Pano.service
%{_datadir}/glib-2.0/schemas/com.aripd.Pano.gschema.xml
%{_datadir}/gnome-shell/extensions/quick-terminal@pano.aripd.com/
%{_datadir}/gnome-shell/search-providers/pano-terminal-search-provider.ini
%{_datadir}/icons/hicolor/scalable/apps/com.aripd.Pano*.svg
%{_datadir}/icons/hicolor/symbolic/apps/com.aripd.Pano*-symbolic.svg
%{_datadir}/metainfo/com.aripd.Pano.metainfo.xml
%{_datadir}/pano-terminal/
%{_datadir}/xdg-terminals/com.aripd.Pano.desktop
%{_mandir}/man1/pano-terminal.1*
%{_userunitdir}/pano-terminal-server.service

%changelog
* Tue Nov 10 2026 aripdcem <cem@aripd.com> - 0.1.1-1
- Install the systemd user unit where systemd finds it
- Complete the Turkish translation and update the others

* Tue Nov 10 2026 aripdcem <cem@aripd.com> - 0.1.0-1
- First release of Pano Terminal
