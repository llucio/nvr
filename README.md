# nvr

Self-hosted, motion/person-activated home surveillance built from a cheap USB
webcam plugged into a travel router, with [Frigate](https://frigate.video) as the NVR.

```
Logitech C920 ──USB──> GL.iNet GL-AXT1800 (stock firmware 4.8.x)
                         uvcstream: userspace UVC -> MJPEG over HTTP :8081
                              │
                              ▼
                        Frigate (Docker) — detection, recording, face recognition
                              │
                              ▼
                        phone notifications · remote access over Tailscale
```

## Why `uvcstream`?

The GL-AXT1800 stock firmware ships a kernel built **without** V4L2/media support
(`CONFIG_MEDIA_SUPPORT` is not set), so `kmod-video-uvc`, `mjpg-streamer` and friends
can't see a webcam. GL.iNet's SDK for this firmware is closed, so building matching
kernel modules is fragile.

`uvcstream` sidesteps the kernel entirely: it talks to the camera through
[libuvc](https://github.com/libuvc/libuvc) on top of libusb/usbfs, which the stock
kernel does provide, and serves the camera's native MJPEG frames over HTTP — no
re-encoding, ~0–6% CPU on the router, single 170 KB static binary.

Tested: C920 (046d:08e5) at 1280×720@30 and 1920×1080@30 on GL-AXT1800 4.8.3.

| Endpoint | |
|---|---|
| `GET /stream` | `multipart/x-mixed-replace` MJPEG |
| `GET /snapshot` | latest frame, `image/jpeg` |
| `GET /` | minimal viewer |

## Build and install

Requires [zig](https://ziglang.org) (used as a cross C compiler), `curl`, `git`, `make`.

```sh
cd router/uvcstream
make                          # -> build/uvcstream (static aarch64-musl)
make install ROUTER=192.168.8.1
```

Settings live in `/etc/config/uvcstream` on the router (port, width, height, fps,
device). Port 8080 is taken by the router's LuCI web server, so the default is 8081.

## Roadmap

- [x] Webcam streaming from the router on stock firmware
- [ ] Frigate (Docker) consuming the stream; person detection + recording
- [ ] Phone notifications
- [ ] Remote access over Tailscale
- [ ] Face recognition ("your wife arrived home")

## License

MIT
