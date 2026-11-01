#!/usr/bin/env bash
# VR.NDI Sender for DaVinci Resolve: Linux uninstaller (ticket #78).
#
#   sudo ./uninstall.sh   remove /usr/OFX/Plugins/NDIOutput.ofx.bundle
#
# One implementation for both directions: this runs install.sh --uninstall
# from the same folder.
exec bash "$(dirname "$0")/install.sh" --uninstall "$@"
