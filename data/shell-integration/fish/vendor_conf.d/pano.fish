# Pano Terminal shell integration for fish, see ../../README.md

# Put XDG_DATA_DIRS back as it was, without this folder
if set -q PANO_SHELL_INTEGRATION
    if set -q PANO_XDG_DATA_DIRS
        set -gx XDG_DATA_DIRS $PANO_XDG_DATA_DIRS
        set -e PANO_XDG_DATA_DIRS
    else
        set -e XDG_DATA_DIRS
    end
    set -e PANO_SHELL_INTEGRATION
end

status is-interactive; or exit 0
set -q __pano_integration; and exit 0
set -g __pano_integration 1

function __pano_preexec --on-event fish_preexec
    printf '\e]666;vte.shell.preexec!\e\\'
end

function __pano_postexec --on-event fish_postexec
    printf '\e]666;vte.shell.postexec=%s\e\\' $status
end

function __pano_prompt --on-event fish_prompt
    set -l dir (string escape --style=url -- $PWD | string replace -a '%2F' /)
    printf '\e]7;file://%s%s\e\\' $hostname $dir
    printf '\e]666;vte.shell.precmd!\e\\'
end
