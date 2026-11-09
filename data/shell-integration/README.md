# Shell integration

Pano Terminal's command notifications, prompt navigation, copying command
output and scrollbar marks need a shell that reports its prompts and
commands. VTE's `vte.sh` does that for bash and zsh, but not every
distribution loads it, and it does not cover fish.

So when Pano Terminal starts the default shell of a profile, it adds these
scripts, unless *Shell Integration* is turned off in the preferences, the
profile runs a custom command, or the terminal is in a container:

- **bash** is started with `--rcfile bash/pano.bashrc`, which loads
  `~/.bashrc` as bash would, then `bash/pano.bash`. Login shells read other
  files and are left alone.
- **zsh** is started with `ZDOTDIR` pointing to `zsh/`, whose `.zshenv`
  puts your own `ZDOTDIR` back, loads your `.zshenv`, and then
  `zsh/pano.zsh`; zsh then reads the rest of your files as usual.
- **fish** finds `fish/vendor_conf.d/pano.fish` through `XDG_DATA_DIRS`,
  which the script puts back as it was.

The scripts report, like `vte.sh`:

- the working directory, with OSC 7;
- `vte.shell.precmd` before each prompt, `vte.shell.preexec` when a
  command starts, and `vte.shell.postexec` with its exit status, with
  VTE's termprops (OSC 666).

If `vte.sh` has already been loaded, the bash script does nothing, and
the zsh script only adds the `vte.shell.preexec` signal that `vte.sh`
lacks for zsh.
