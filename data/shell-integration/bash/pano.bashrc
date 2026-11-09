# Pano Terminal starts bash with this file instead of ~/.bashrc, see
# ../README.md. Load the user's file as bash would, then the integration.

if [ -r ~/.bashrc ]; then
    . ~/.bashrc
fi

. "${BASH_SOURCE[0]%/*}/pano.bash"
