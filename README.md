Pano Terminal
=============

Pano Terminal is a terminal emulator for the GNOME desktop, built with GTK 4,
libadwaita and VTE.

Website: https://pano.aripd.com

Pano is a fork of [GNOME Terminal](https://gitlab.gnome.org/GNOME/gnome-terminal).
It focuses on power-user features that GNOME Terminal does not offer, such
as flexible tab placement, split panes, session restore and background
transparency.

> **Status:** early development. There are no packages yet; build it from
> source as described below.

Pano Terminal can be installed next to GNOME Terminal. It uses its own
programs (`pano-terminal`, `pano-terminal-server`), application ID
(`com.aripd.Pano`) and settings (`/com/aripd/pano/`), so the two do not share
profiles or preferences. If you have used GNOME Terminal, Pano Terminal offers
to copy its profiles, keyboard shortcuts and preferences on its first start;
you can also do that later in *Preferences → Application*.

Features
--------

Everything GNOME Terminal provides, plus:

* **Tab bar position**: place the tabs at the top, bottom, left or right of
  the window (*Preferences → Appearance → Tab Bar Position*). Left and right
  show the tabs as a vertical list: drag tabs to reorder them, onto another
  window or out of the window; drag its inner edge to resize it; pinned tabs
  stay on top. Ctrl+Shift+L moves the focus to the list, where the arrow
  keys switch tabs, the menu key opens the tab menu, Delete closes the tab,
  and Return or Escape go back to the terminal.
* **Split panes**: split a tab into several terminals, side by side or one
  above the other. New panes start in the working directory of the active
  one. Use the context menu, the main menu or these shortcuts (all can be
  changed in *Preferences → Shortcuts*):

  | Action                              | Shortcut            |
  |-------------------------------------|---------------------|
  | Split right / split down            | Ctrl+Shift+R / Ctrl+Shift+D |
  | Move the focus to a neighbouring pane | Alt+Arrow keys    |
  | Move the divider                    | Alt+Shift+Arrow keys |
  | Zoom the active pane (and back)     | Ctrl+Shift+Z        |
  | Close the active pane               | Ctrl+Shift+X        |

  The focus, divider, zoom and close shortcuts only take effect while a tab
  is split, so programs in the terminal still get Alt+Arrow keys otherwise.

  The context menu of a split tab can also make all panes the same size and
  move the active pane, with what runs in it, to a new tab or window. The
  panes you are not typing in are dimmed, and *Preferences → Appearance* can
  show a title above each pane.
* **Session restore**: when started without arguments, for example from the
  application menu, Pano Terminal reopens the windows, tabs and panes that
  were open when it was last closed, each with a new shell in its previous
  folder. Running programs are not restored. Turn it off with
  *Preferences → Behavior → Restore Session*. The session is kept in
  `~/.local/state/pano-terminal/`.
* **Remember window size** (optional): open new windows with the size of the
  last used terminal instead of the initial size of the profile.
* **Color schemes**: choose one of about 240 color schemes (Gruvbox, Nord,
  Dracula, Solarized, Tango, …) in *Preferences → Profile → Colors → Color
  Scheme*. Schemes with a light and a dark variant switch with the system
  style. Add your own `.palette` files to `~/.local/share/pano-terminal/schemes`
  (*Open Folder* in the same place); the format is described in
  [data/schemes/README.md](data/schemes/README.md), and changes apply at once.
* **Profile import and export**: save a profile to a `.pano-profile` file
  with *Export…* in its menu in *Preferences*, and load it on another
  computer with *Import…* above the list of profiles. The file holds the
  settings that differ from the defaults, in the format `dconf dump` uses.
* **Visual bell**: turn on *Flash the Terminal* in *Preferences → Profile →
  Bell* to see the bell instead of, or as well as, hearing it. A tab in the
  background is marked instead.
* **Space around the text**: choose none, small or large padding between
  the terminal text and its edges in *Preferences → Appearance*.
* **Background transparency**: set the background opacity of each profile in
  *Preferences → Profile → Colors → Background Opacity*. Only the terminal
  background lets the desktop through; text, header bar and tab bars stay
  opaque.
* **Command notifications**: when a command that ran for a while (10 seconds
  by default) finishes in a terminal you are not looking at, Pano Terminal
  sends a desktop notification with the command, its result and how long it
  took, and marks its tab. Click the notification to go to the terminal.
  Change it in *Preferences → Behavior*. This needs a shell that reports its
  commands: bash or zsh with VTE's shell integration. Fedora loads it by
  default; on Debian and Ubuntu add `. /etc/profile.d/vte-2.91.sh` to
  `~/.bashrc`.
* **Prompt navigation**: with the same shell integration, jump between the
  prompts of earlier commands with Ctrl+Shift+Up and Ctrl+Shift+Down, and
  copy the output of the last command with Ctrl+Shift+O or from the context
  menu. Without shell integration these keys keep their usual meaning.
* **Better search**: the find bar highlights every match, shows which match
  is current out of how many ("3 of 17"), and says when a regular expression
  is invalid.
* **Broadcast input**: type into several terminals at once, for example to
  run the same command on several servers. Choose *Broadcast Input → To All
  Panes in This Tab* or *To All Terminals in This Window* in the main or
  context menu. A banner shows how many terminals get the input, and they
  are framed; leave single terminals out with *Include This Terminal*.
* **Root and remote sessions stand out**: while the active terminal runs a
  program as root (sudo, su, a root shell), the header bar turns red; while
  it is connected to another computer with ssh or mosh, purple. Such tabs get
  a warning or server icon. Turn it off in *Preferences → Appearance*.
* **Containers**: open a tab in a Toolbox or Distrobox container from the
  main menu (*New Tab in Container*). New tabs and panes opened from a
  terminal in a container start in the same container; turn this off in
  *Preferences → Behavior*. Containers are found with podman.
* **Tab titles**: *Set Title…* in a tab's menu gives the terminal a title
  that programs cannot change, which is kept with the session. To ignore
  the titles that programs set everywhere, turn on *Ignore Titles Set by
  Programs* in *Preferences → Behavior*. A terminal without a title shows
  its working directory.
* **Save contents**: *Save Contents…* in the main menu saves the terminal's
  text with all its scrollback to a file, without freezing the window.
* **Copy notice**: a short "Copied to clipboard" notice confirms copying;
  turn it off with *Confirm Copying* in *Preferences → Behavior*.
* **Middle-click on a tab**: choose in *Preferences → Behavior* whether a
  middle click on a tab, in the tab bar or the tab list, closes it, pastes
  the selection into it, or does nothing (the default).
* **Close other tabs**: *Close Other Terminals* in a tab's menu closes every
  other tab except pinned ones, asking once if something still runs in
  them. Assign a shortcut in *Preferences → Keyboard Shortcuts*.
* **Reopen closed tabs**: Ctrl+Shift+Alt+T, the main menu or the Undo button
  of the "Tab closed" notice bring back the last closed tab, with its panes,
  working directories and profiles, where it was. The last ten closed tabs
  are kept, also from windows that were closed.
* **Quick terminal**: a terminal that you show and hide with one key. Run
  `pano-terminal --quick` to toggle it; to use a key such as F12, add that
  command in *Settings → Keyboard → View and Customize Shortcuts → Custom
  Shortcuts*. Set its width and height in *Preferences → Quick Terminal*. On
  X11 it opens at the top of the screen; on Wayland the desktop places it.

See the [issue tracker](https://github.com/aripdcom/pano/issues) for the
roadmap.

Packages
--------

The [Packages workflow](https://github.com/aripdcom/pano/actions/workflows/packages.yml)
builds a source tarball, a Fedora RPM and a Debian 13 package; download them
from the artifacts of a run. To build them yourself, see
[`packaging/`](packaging/README.md).

Building from source
--------------------

Pano Terminal needs GTK 4.12, libadwaita 1.4 and VTE 0.80 or newer. It is
tested on Fedora 45 (GTK 4.24, libadwaita 1.10, VTE 0.84) and Debian 13
(GTK 4.18, libadwaita 1.7, VTE 0.80).

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

If your distribution ships an older VTE (for example Ubuntu 24.04 with 0.76),
let Meson build VTE as a subproject by adding
`--force-fallback-for=vte-2.91-gtk4` to the `meson setup` command.

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
