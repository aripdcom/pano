# Packaging

The files here build Pano Terminal packages. The
[Packages workflow](../.github/workflows/packages.yml) runs these steps on
GitHub; pushing a `v*` tag also attaches the packages to a draft release.

The packages are built without the Nautilus extension, which needs
`libnautilus-extension-4`, and with the GNOME Shell search provider.

## Fedora

Install the build dependencies listed in the main README, plus `rpm-build`
and `systemd-rpm-macros`, then make the source tarball and build the RPM:

```
meson setup _build -Dnautilus_extension=false
meson dist -C _build --no-tests --formats xztar
mkdir -p ~/rpmbuild/SOURCES
cp _build/meson-dist/pano-terminal-*.tar.xz ~/rpmbuild/SOURCES/
rpmbuild -bb packaging/fedora/pano-terminal.spec
```

`meson dist` packs the last commit, so commit your changes first. The RPM is
written to `~/rpmbuild/RPMS/`.

## Debian

On Debian 13 or newer, with `devscripts`, `equivs` and `debhelper`
installed:

```
cp -a packaging/debian debian
sudo mk-build-deps -i -r debian/control
dpkg-buildpackage -us -uc -b
```

The `.deb` files are written to the parent folder.

## Flatpak

[`flatpak/com.aripd.Pano.json`](flatpak/com.aripd.Pano.json) builds Pano
Terminal and VTE on the GNOME 51 runtime. With `flatpak-builder` and the
`org.gnome.Sdk//51` runtime installed:

```
flatpak-builder --user --install --force-clean _flatpak packaging/flatpak/com.aripd.Pano.json
flatpak run com.aripd.Pano
```

To make a bundle that installs elsewhere, build into a repository with
`--repo=repo` and run `flatpak build-bundle repo pano-terminal.flatpak com.aripd.Pano`.

In the sandbox, Pano Terminal starts the shells and other commands on the
host through Flatpak's `HostCommand` (as `flatpak-spawn --host` does), and
looks at the processes there for the working directory, the root and SSH
highlights and the close confirmation; see `src/terminal-flatpak.cc`. The
meson option `-Dflatpak=true` leaves out the systemd user unit, which
Flatpak cannot export, and names the search provider after the
application. What does not work in the Flatpak:

- the Nautilus extension, which has to be installed on the host;
- importing the GNOME Terminal settings: the settings are kept in the
  sandbox (`~/.var/app/com.aripd.Pano`), not in dconf, where GNOME Terminal
  keeps its own.

## Versions

When releasing, update the version in `meson.build`, the `Version:` and the
`%changelog` in `fedora/pano-terminal.spec`, and add an entry to
`debian/changelog`.
