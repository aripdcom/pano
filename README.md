Pano Terminal
=============

Pano Terminal is a terminal emulator for the GNOME desktop, built with GTK 4,
libadwaita and VTE.

Website: https://pano.aripd.com

Pano is a fork of [GNOME Terminal](https://gitlab.gnome.org/GNOME/gnome-terminal).
It focuses on power-user features that GNOME Terminal does not offer, such
as flexible tab placement and, on the roadmap, split panes, session restore
and background transparency.

> **Status:** early development. There are no packages yet; build it from
> source as described below.

Pano Terminal can be installed next to GNOME Terminal. It uses its own
programs (`pano-terminal`, `pano-terminal-server`), application ID
(`com.aripd.Pano`) and settings (`/com/aripd/pano/`), so the two do not share
profiles or preferences.

Features
--------

Everything GNOME Terminal provides, plus:

* **Tab bar position**: place the tabs at the top, bottom, left or right of
  the window (*Preferences → Appearance → Tab Bar Position*). Left and right
  show the tabs as a vertical list.

See the [issue tracker](https://github.com/aripdcom/pano/issues) for the
roadmap.

Building from source
--------------------

Pano needs recent versions of its dependencies. It is currently tested with
GTK 4.24, libadwaita 1.10 and VTE 0.84.

On Fedora:
```
sudo dnf install meson ninja-build gcc-c++ gtk4-devel libadwaita-devel \
    vte291-gtk4-devel pcre2-devel libuuid-devel gsettings-desktop-schemas-devel \
    gettext itstool desktop-file-utils libxslt docbook-style-xsl
```

On Debian and Ubuntu:
```
sudo apt install meson ninja-build g++ libgtk-4-dev libadwaita-1-dev \
    libvte-2.91-gtk4-dev libpcre2-dev uuid-dev gsettings-desktop-schemas-dev \
    gettext itstool desktop-file-utils xsltproc docbook-xsl
```

If your distribution ships an older VTE, let Meson build VTE as a subproject
by adding `--force-fallback-for=vte-2.91-gtk4` to the `meson setup` command.

Then build and install:
```
git clone https://github.com/aripdcom/pano.git
cd pano
meson setup _build --prefix=$HOME/.local -Dsearch_provider=false -Dnautilus_extension=false
ninja -C _build
ninja -C _build install
```

Pano uses a D-Bus activated server, so the client cannot simply be run from
the build directory. Start the server with a test application ID, then open a
window in it within 10 seconds:
```
./_build/src/pano-terminal-server --app-id test.Pano &
./_build/src/pano-terminal --app-id test.Pano
```

Debugging
---------

Configure with `-Ddbg=true` and set `PANO_DEBUG` to the debug categories
you want, for example:
```
PANO_DEBUG=selection,draw ./_build/src/pano-terminal-server [...]
PANO_DEBUG=all ./_build/src/pano-terminal-server [...]
```
The categories are listed in [`TerminalDebugFlags`](src/terminal-debug.hh).

Contributing
------------

Report bugs and request features in the
[issue tracker](https://github.com/aripdcom/pano/issues). Please do not
report Pano problems to the GNOME Terminal project.

License
-------

Pano is free software, released under the
[GNU General Public License, version 3 or later](COPYING). The documentation
is released under the [GNU Free Documentation License](COPYING.GFDL).

GNOME Terminal is developed by the GNOME project and its contributors. Pano
is not affiliated with or endorsed by the GNOME Foundation.
