#!/bin/sh
# Build sonyctl (Release) and install it to a local bin directory.
#
# Usage:
#   ./install.sh              # installs to ~/.local/bin
#   ./install.sh /usr/local   # installs to /usr/local/bin
#   PREFIX=~/opt ./install.sh  # installs to ~/opt/bin
#
# The install prefix comes from (in order): $1, $PREFIX, then ~/.local.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
prefix=${1:-${PREFIX:-$HOME/.local}}
build_dir=$here/build

cmake -S "$here" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_dir" --config Release
cmake --install "$build_dir" --prefix "$prefix"

bindir=$prefix/bin
printf '\ninstalled: %s/sonyctl\n' "$bindir"
case ":$PATH:" in
    *":$bindir:"*) ;;
    *) printf 'note: %s is not on your PATH; add it, e.g.\n  export PATH="%s:$PATH"\n' "$bindir" "$bindir" ;;
esac
