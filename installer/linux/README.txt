VR.NDI Sender for DaVinci Resolve (Linux)
=========================================

An OpenFX plugin that streams the frame DaVinci Resolve renders as an NDI(R)
source on your local network.

Installs to:
  /usr/OFX/Plugins/NDIOutput.ofx.bundle


Install
-------
From the folder this README is in:

  sudo ./install.sh

The installer replaces any earlier version of the plugin (running it again is
safe), then checks the prerequisites below and warns about anything missing.
None of those warnings stop the install.

Then:
1. Fully restart DaVinci Resolve: OpenFX plugins are only scanned at startup.
2. Color page -> OpenFX -> LSVR -> VR.NDI Sender; drag it onto a clip.
3. Start playback and open the stream in any NDI receiver, for example Studio
   Monitor or NDI Video Monitor from the free NDI Tools
   (https://ndi.video/tools/), on another machine.


Uninstall
---------
  sudo ./uninstall.sh

This removes /usr/OFX/Plugins/NDIOutput.ofx.bundle and nothing else. Restart
Resolve afterwards. Running it again when nothing is installed is safe.


Support statement
-----------------
Tested on Ubuntu 24.04 with DaVinci Resolve Studio 21.1 and an NVIDIA RTX GPU.
Built for Rocky Linux 8 and later, on glibc 2.28 (the platform Blackmagic
certifies Resolve on): the binary is checked to load on Rocky 8 and Ubuntu,
but only Ubuntu is tested end to end. Other x86-64 distributions with glibc
2.28 or newer are expected to work but are unsupported. Linux ARM is not
supported (Resolve has no Linux ARM build).

The plugin ships its own NDI runtime (libndi.so.6, inside the bundle): you
don't need the NDI SDK or NDI Tools on this machine. On Linux the plugin runs
its CPU path today, which has a lower resolution ceiling (use the Resolution
control to stream Half or Quarter); the GPU-native CUDA path for NVIDIA GPUs
arrives in a later release.


Prerequisites: Avahi
--------------------
NDI on Linux discovers and advertises through Avahi, so it needs two things:

* The Avahi client libraries. The NDI runtime links them. Without them the
  plugin loads and passes frames through, but can't stream, and Stream Status
  reads "NDI off - Avahi client library missing".
    Ubuntu/Debian:      sudo apt install libavahi-client3
    Rocky/RHEL/Alma:    sudo dnf install avahi-libs

* A running avahi-daemon. Without it the plugin streams, but no receiver can
  discover the source (one that already knows the address, or an NDI
  Discovery Server, still can).
    Ubuntu/Debian:      sudo apt install avahi-daemon
    Rocky/RHEL/Alma:    sudo dnf install avahi
    then:               sudo systemctl enable --now avahi-daemon
  Check it with:        systemctl is-active avahi-daemon

NEVER STOP avahi-daemon WHILE RESOLVE IS RUNNING. Resolve's own control-panel
discovery uses it and Resolve crashes when it goes away, with or without this
plugin. Quit Resolve first if you need to stop or restart the daemon.


Firewall
--------
Allow mDNS (5353/udp) and NDI's ports (5959-5999, 6960-6999 and 7960-7999,
both tcp and udp) from your LAN AND from IPv6 link-local addresses
(fe80::/10). Avahi advertises the machine's fe80:: addresses and receivers
such as macOS connect over them: with IPv4-only rules the source shows up on
other machines but no video arrives.

ufw (Ubuntu); replace 192.168.1.0/24 with your LAN subnet(s):

  for net in 192.168.1.0/24 fe80::/10; do
    sudo ufw allow from "$net" to any port 5353 proto udp comment 'mDNS (NDI discovery)'
    for ports in 5959:5999 6960:6999 7960:7999; do
      sudo ufw allow from "$net" to any port "$ports" proto tcp comment 'NDI'
      sudo ufw allow from "$net" to any port "$ports" proto udp comment 'NDI'
    done
  done

firewalld (Rocky/RHEL, on by default), for the zone your LAN interface is in:

  sudo firewall-cmd --permanent --add-service=mdns
  for ports in 5959-5999 6960-6999 7960-7999; do
    sudo firewall-cmd --permanent --add-port=$ports/tcp --add-port=$ports/udp
  done
  sudo firewall-cmd --reload

(firewalld zone rules apply to IPv4 and IPv6 alike, link-local included.)


Display: Resolve needs real NVIDIA OpenGL
-----------------------------------------
On a headless or remote-desktop machine, run X on the NVIDIA driver with a
virtual monitor (a fake EDID, e.g. ConnectedMonitor + CustomEDID in
xorg.conf), not X's "dummy" driver or software GL. With a dummy display
Resolve's CUDA-OpenGL interop fails as soon as the Color page opens, and
Resolve reports it as "GPU out of memory". This is a Resolve requirement,
independent of this plugin.


Plugin missing from the Effects Library (cache reset)
-----------------------------------------------------
Resolve caches what it learns about each plugin. If VR.NDI Sender doesn't
appear after a restart, or an old version lingers:
1. Quit Resolve fully (it rewrites the cache on exit).
2. Delete  ~/.local/share/DaVinciResolve/configs/OFXPluginCacheV2.xml
   (in the home folder of the user who runs Resolve, not root's).
3. Start Resolve again; it rescans every OpenFX plugin.


Logs
----
The plugin logs to the system journal, tagged NDIOutput:

  journalctl -t NDIOutput          # everything so far
  journalctl -f -t NDIOutput       # follow live

Attach that output when reporting a problem. The Stream Status line in the
plugin's controls also says why a stream isn't running.


Attribution and licensing
-------------------------
NDI(R) is a registered trademark of Vizrt NDI AB - https://ndi.video/

This product uses the NDI SDK. The NDI runtime's third-party license notices
are in libndi_licenses.txt in this folder and beside the runtime inside the
bundle (NDIOutput.ofx.bundle/Contents/Linux-x86-64/).

Plugin license: LICENSE.txt (BSD 3-Clause). Source and documentation:
https://github.com/light-sail-vr/lsvr-vrndi-resolve-plugin
