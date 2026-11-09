# Color schemes

Pano Terminal reads color schemes from `.palette` files in these folders,
in this order (a later file with the same name replaces an earlier one):

1. `$prefix/share/pano-terminal/schemes` (the schemes in this folder)
2. `pano-terminal/schemes` in the other system data folders (`$XDG_DATA_DIRS`)
3. `~/.local/share/pano-terminal/schemes` (your own schemes)

The file name without `.palette` is the scheme's identifier, which profiles
store in their `color-scheme` setting.

## File format

A scheme is an INI file. It either has the colors directly in `[Palette]`,
which are then used for both the light and the dark style, or it has a
`[Light]` and a `[Dark]` section, and Pano Terminal uses the one that fits
the current style:

```ini
[Palette]
Name=My Scheme

[Light]
Background=#FBF1C7
Foreground=#3C3836
Cursor=#3C3836
Color0=#FBF1C7
...
Color15=#3C3836

[Dark]
Background=#282828
Foreground=#EBDBB2
Cursor=#EBDBB2
Color0=#282828
...
Color15=#EBDBB2
```

`Background`, `Foreground` and `Color0` to `Color15` are required;
`Cursor` is optional. Colors can be written in any form that
`gdk_rgba_parse()` understands, such as `#RRGGBB` or `rgb(…)`.

## Origin and license

The schemes here come from [Ptyxis](https://gitlab.gnome.org/chergert/ptyxis)
(GPL-3.0-or-later), which generated most of them from the themes of
[Gogh](https://github.com/Gogh-Co/Gogh). Gogh is available under the MIT
license, see `LICENSE-MIT`, or the Apache License 2.0.
