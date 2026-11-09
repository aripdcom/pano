# Pano Terminal starts zsh with ZDOTDIR pointing here, see ../README.md.
# Put the user's ZDOTDIR back, so that zsh reads their other files, and
# load their .zshenv, then the integration.

__pano_integration_dir="$ZDOTDIR"
if [[ -n "${PANO_ZDOTDIR+x}" ]]; then
    ZDOTDIR="$PANO_ZDOTDIR"
    unset PANO_ZDOTDIR
else
    unset ZDOTDIR
fi

if [[ -r "${ZDOTDIR:-$HOME}/.zshenv" ]]; then
    source "${ZDOTDIR:-$HOME}/.zshenv"
fi

if [[ -o interactive ]]; then
    source "$__pano_integration_dir/pano.zsh"
fi
unset __pano_integration_dir
