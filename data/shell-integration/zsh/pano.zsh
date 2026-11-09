# Pano Terminal shell integration for zsh, see ../README.md

[[ -o interactive ]] || return 0
(( ${+functions[__pano_preexec]} )) && return 0

# Signals that a command starts. vte.sh does not do that for zsh, so this
# is added even when vte.sh is loaded later.
__pano_preexec() {
    printf '\e]666;vte.shell.preexec!\e\\'
}
preexec_functions+=(__pano_preexec)

__pano_urlencode() {
    local LC_ALL=C str="$1" out="" c i
    for (( i = 1; i <= ${#str}; i++ )); do
        c="${str[i]}"
        case "$c" in
            [a-zA-Z0-9/._~-]) out+="$c" ;;
            *) printf -v c '%%%02X' "'$c"; out+="$c" ;;
        esac
    done
    printf '%s' "$out"
}

# Before each prompt; does nothing once vte.sh is loaded, which does the same
__pano_precmd() {
    local exit_status="$?"
    (( ${+functions[__vte_precmd]} )) && return $exit_status
    printf '\e]666;vte.shell.postexec=%s\e\\' "$exit_status"
    printf '\e]7;file://%s%s\e\\' "${HOST}" "$(__pano_urlencode "$PWD")"
    printf '\e]666;vte.shell.precmd!\e\\'
    return $exit_status
}
precmd_functions=(__pano_precmd $precmd_functions)
