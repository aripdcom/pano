# Pano Terminal shell integration for bash, see ../README.md

# Only for interactive shells, and only once; vte.sh does the same
[[ $- == *i* ]] || return 0
declare -F __vte_prompt_command >/dev/null && return 0
declare -F __vte_precmd >/dev/null && return 0
declare -F __pano_prompt_command >/dev/null && return 0

__pano_urlencode() {
    local LC_ALL=C str="$1" out="" c i
    for (( i = 0; i < ${#str}; i++ )); do
        c="${str:i:1}"
        case "$c" in
            [a-zA-Z0-9/._~-]) out+="$c" ;;
            *) printf -v c '%%%02X' "'$c"; out+="$c" ;;
        esac
    done
    printf '%s' "$out"
}

# Runs first before each prompt, so that it sees the exit status of the
# command, which it keeps for the rest of PROMPT_COMMAND
__pano_prompt_command() {
    local status="$?"
    printf '\e]666;vte.shell.postexec=%s\e\\' "$status"
    printf '\e]7;file://%s%s\e\\' "${HOSTNAME}" "$(__pano_urlencode "$PWD")"
    printf '\e]666;vte.shell.precmd!\e\\'
    return $status
}

if [[ "$(declare -p PROMPT_COMMAND 2>/dev/null)" == "declare -a"* ]]; then
    PROMPT_COMMAND=(__pano_prompt_command "${PROMPT_COMMAND[@]}")
else
    PROMPT_COMMAND="__pano_prompt_command${PROMPT_COMMAND:+;$PROMPT_COMMAND}"
fi

# PS0 is printed when a command starts (bash 4.4 and newer). The '\r' keeps
# the string from ending in a backslash.
PS0='\e]666;vte.shell.preexec!\e\\\r'"${PS0:-}"
