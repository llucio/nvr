# Pausing, resuming and restoring

## Pause / resume

```sh
# camera router
ssh root@<router> '/etc/init.d/uvcstream stop; /etc/init.d/uvcstream disable'   # pause
ssh root@<router> '/etc/init.d/uvcstream enable; /etc/init.d/uvcstream start'    # resume

# NVR host
cd frigate && docker compose stop      # pause (stays stopped across reboots)
cd frigate && docker compose start     # resume
```

## Undo everything this project changed on a GL.iNet router

Nothing here touches the firmware or kernel, so reverting is a handful of config changes.

1. Remove uvcstream: `make -C router/uvcstream uninstall ROUTER=<router>`
2. Remove the firewall rule that exposes port 8081:
   ```sh
   ssh root@<router> 'i=$(uci show firewall | grep "name=.Allow-uvcstream." | cut -d. -f2);
     [ -n "$i" ] && uci delete firewall.$i && uci commit firewall && /etc/init.d/firewall reload'
   ```
3. Revert any network changes you made for the camera router (LAN IP, hostname,
   repeater network, Tailscale) from the GL.iNet admin panel, or restore a backup
   taken before the change: **System → Backup / Restore** (or `sysupgrade -r <file>`).

## Full factory reset (last resort)

GL.iNet admin panel → **System → Reset Firmware**, or hold the reset button for ~10 s.
This wipes all settings (Wi-Fi names, passwords, Tailscale binding) back to the sticker defaults.

Take a config backup before experimenting: `ssh root@<router> 'sysupgrade -b /tmp/b.tar.gz'`
then copy it off the router. Backups contain Wi-Fi keys — keep them out of git.
