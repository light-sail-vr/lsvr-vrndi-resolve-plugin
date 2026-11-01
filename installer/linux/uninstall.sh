#!/usr/bin/env bash
# VR.NDI Sender for DaVinci Resolve: Linux uninstaller (ticket #78).
#
#   sudo ./uninstall.sh   remove /usr/OFX/Plugins/NDIOutput.ofx.bundle
#
# One implementation for both directions: this runs install.sh --uninstall
# from the same folder. Arguments are never passed on: --help prints this and
# anything else is refused, so neither removes anything.
set -euo pipefail
case "$#:${1:-}" in
    0:) ;;
    1:-h|1:--help) sed -n '2,4p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "usage: sudo $0" >&2; exit 2 ;;
esac
exec bash "$(dirname "$0")/install.sh" --uninstall
