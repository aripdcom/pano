# Packaging

The files here build Pano Terminal packages. The
[Packages workflow](../.github/workflows/packages.yml) runs these steps on
GitHub; pushing a `v*` tag also attaches the packages to a draft release.

Both packages are built without the Nautilus extension, which needs
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

## Versions

When releasing, update the version in `meson.build`, the `Version:` and the
`%changelog` in `fedora/pano-terminal.spec`, and add an entry to
`debian/changelog`.
