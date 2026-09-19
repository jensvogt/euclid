# Tab completion for euclid-cli, for bash.
#
# Install by copying to /usr/share/bash-completion/completions/euclid-cli, or source it from
# ~/.bashrc:
#
#     source /usr/local/euclid/etc/euclid-cli.bash
#
# There is deliberately nothing in here about modules, actions or options. The binary answers
# every completion question from the same tables it prints help from, so this file does not go
# stale when a command is added - which is exactly what a hand-written list of actions does, the
# first time somebody adds one and does not think to come here.
#
# -C hands the command COMP_LINE and COMP_POINT and reads candidates from its standard output,
# one per line, so no shell function is needed at all. -o default falls back to file names where
# the binary offers nothing, which is what an option taking a path wants.
complete -o default -C 'euclid-cli __complete' euclid-cli
