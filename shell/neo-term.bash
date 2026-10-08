if [[ $- == *i* && -z ${__neo_term_installed-} ]]; then
    __neo_term_installed=1
    if [[ -f ~/.bashrc ]]; then
        source ~/.bashrc
    fi
    neo-open() {
        if [[ $# -lt 1 || $# -gt 3 ]]; then
            printf 'Usage: neo-open FILE [LINE [COLUMN]]\n' >&2
            return 1
        fi
        local file
        file=$(cygpath -aw -- "$1") || return
        file=${file//\\/\\\\}
        file=${file//\"/\\\"}
        __neo_term_notify "$(printf '51;neo-term;{"file":"%s","line":%s,"column":%s}' "$file" "${2:-1}" "${3:-1}")"
    }

    __neo_term_notify() {
        local connection
        exec {connection}>"/dev/tcp/127.0.0.1/$NEO_TERM_PORT" || return
        printf '%s\n%s' "$NEO_TERM_TOKEN" "$1" >&"$connection"
        exec {connection}>&-
    }

    __neo_term_cwd() {
        local status=$?
        __neo_term_notify "cwd;$(cygpath -aw -- "$PWD")"
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
