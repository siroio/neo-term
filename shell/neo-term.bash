if [[ $- == *i* && -z ${__neo_term_installed-} ]]; then
    __neo_term_installed=1
    if [[ -f ~/.bashrc ]]; then
        source ~/.bashrc
    fi
    __neo_term_host=$(cygpath -u "$NEO_TERM_HOST")

    neo-open() {
        if [[ $# -lt 1 || $# -gt 3 ]]; then
            printf 'Usage: neo-open FILE [LINE [COLUMN]]\n' >&2
            return 1
        fi
        "$__neo_term_host" --notify open "$(cygpath -w -- "$1")" "${2:-1}" "${3:-1}"
    }

    __neo_term_cwd() {
        local status=$?
        "$__neo_term_host" --notify cwd
        return "$status"
    }

    if declare -p PROMPT_COMMAND 2>/dev/null | grep -q 'declare -a'; then
        PROMPT_COMMAND+=(__neo_term_cwd)
    elif [[ -n ${PROMPT_COMMAND-} ]]; then
        PROMPT_COMMAND+=(__neo_term_cwd)
    else
        PROMPT_COMMAND=(__neo_term_cwd)
    fi
    PS1='\[\e]133;A\e\\\]'"${PS1-\s-\v\$ }"'\[\e]133;B\e\\\]'
fi
