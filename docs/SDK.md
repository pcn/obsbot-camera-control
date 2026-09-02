# OBSBOT SDK Analysis

The OBSBOT SDK (`sdk/`) is a closed-source library distributed as precompiled binaries and C++ headers. This document records what we know about it from inspecting the public headers and the unstripped binary.

## What's Distributed

- Three headers: `dev.hpp` (~4000 lines of declarations), `devs.hpp`, `comm.hpp`
- Compiled shared library: `libdev.so` (24MB, x86_64, ELF, debug symbols included, not stripped)
- One sample application: `sample_main.cpp`
- No license file, no copyright headers, no attribution notices

The library version reported in `comm.hpp` is 1.3.0. OBSBOT's download labeled "v2.1.0" contains byte-identical files (verified by SHA256).

## Internal Source Structure

Debug symbols in the unstripped binary reveal the original source tree:

```
libdev/
├── base64/          Base64 encode/decode
├── bsdiff/          Binary diff/patch (BSD 2-clause)
├── bzip2/           Compression (BSD-style license)
├── dev/
│   ├── dev.cpp, dev-info.cpp, dev-linux.cpp
│   ├── dev-mtp.cpp, dev-stream-linux.cpp, dev-upgrade.cpp
│   ├── dev-protocol-ai.cpp         # AI/tracking commands
│   ├── dev-protocol-camera.cpp     # Camera control commands
│   ├── dev-protocol-rm.cpp         # Remote management
│   ├── dev-protocol-sysmg.cpp      # System management
│   ├── dev-protocol-uvc-linux.cpp  # UVC extension unit protocol
│   ├── devs.cpp, devs-linux.cpp, devs-mdns.cpp
│   ├── av-capture-linux.cpp, bt-protocol-rm.cpp
│   └── uvc-protocol.cpp
├── ftp/             FTP client
├── kcp/             KCP reliable UDP transport
├── lzma/            Compression (public domain)
├── mdns/            mDNS/Zeroconf discovery
├── mtp/linux/       Full MTP/PTP/USB stack
├── package/         Firmware packaging and upgrade
├── protobuf/        Protobuf serialization (camera.pb.c, dev.pb.c)
└── util/            CRC, utilities
```

## What the SDK Actually Does

The library is a unified control interface for the entire OBSBOT product line — USB webcams, WiFi PTZ cameras, network converters. Most of its bulk is irrelevant for USB camera control:

| Component | Used for USB webcams? | Purpose |
|-----------|----------------------|---------|
| UVC XU protocol | **Yes** | Extension unit commands for AI tracking, camera settings |
| Device enumeration | **Yes** (but V4L2/udev does this too) | Find connected cameras |
| Protobuf serialization | **Yes** | Message encoding for XU commands |
| MTP stack | No | File transfer (backgrounds, firmware) |
| FTP client | No | Network camera file access |
| KCP/UDP | No | Reliable transport for WiFi cameras |
| mDNS | No | WiFi camera discovery |
| bsdiff/bzip2/lzma | No | Firmware delta updates |
| Bluetooth | No | BT device pairing |

For controlling a USB webcam, the SDK's job reduces to: write structured commands to UVC Extension Unit selectors and read status back. Everything else is overhead from the multi-product design.

## USB Work Modes

The binary references these camera USB modes, switchable via the SDK:

| Mode | Description |
|------|-------------|
| `UVC_UAC` | Normal webcam (video + audio) |
| `UVC_RNDIS` | Video + USB network interface |
| `RNDIS_ONLY` | Camera as pure network device |
| `MASS_STORAGE` | Camera as USB storage device |
| `MTP` | Media Transfer Protocol |
| `HOST` | Camera as USB host |
| `IDLE` | Idle state |

The RNDIS modes strongly suggest the camera runs an embedded Linux stack with USB gadget framework support.

## UVC Extension Unit (Direct Hardware Probe)

The OBSBOT Meet 2 exposes a vendor-specific UVC Extension Unit accessible without the SDK:

```
GUID: {9a1e7291-6843-4683-6d92-39bc7906ee49}
Unit ID: 2
Controls: 7 selectors, each 60 bytes, all GET+SET capable
```

Probed via standard `UVCIOC_CTRL_QUERY` ioctl:

| Selector | Idle State | Notes |
|----------|-----------|-------|
| 1 | zeros | Command channel |
| 2 | zeros | Command channel |
| 3 | zeros | Command channel |
| 4 | `01 00...` | State flag |
| 5 | zeros | Command channel |
| 6 | `24 00 00 02 02 00 01 01...` | Camera status (richest data) |
| 7 | `06 00...` | Product type or mode |

Selector 6 returns a packed struct with live camera state — AI mode, tracking status, feature enables, image settings. This is readable from any language that can do a V4L2 ioctl, no SDK required.

All standard camera controls (brightness, contrast, zoom, pan, tilt, exposure, white balance, focus) are exposed as normal V4L2 controls by the kernel `uvcvideo` driver and work without the SDK.

### Tiny 3 Extension Unit

The Tiny 3 exposes the **same GUID at the same unit ID**, but a larger control
surface:

```
GUID: {9a1e7291-6843-4683-6d92-39bc7906ee49}
Unit ID: 2
Controls: 19 selectors (Meet 2: 7), each 60 bytes, all GET+SET capable
```

A read-only `GET_CUR` sweep of all 19 selectors succeeds without the SDK, even
though the SDK cannot claim the device at all:

| Selector | Idle State | Notes |
|----------|-----------|-------|
| 6 | `2e 01 01 02 00 00 00 01 00 03 78 00 00 01 01 ...` | live status, richest data |
| 7 | `06 00...` | see caveat below |
| 10 | `01 01 00...` | |
| 14 | `3e a0 06 00...` | |
| 1–5, 8, 9, 11–13, 15–19 | zeros | likely command channels |

Caveat on selector 7: the Meet 2 table above guesses "product type or mode", but
the Tiny 3 returns `06 00` there **as well**, so selector 7 does not
discriminate between these two models and that reading is unsupported.

This is the only route to tracking and auto-framing on the Tiny 3, because V4L2
exposes no AI, tracking, framing, or FOV control (a full
`v4l2-ctl --list-ctrls-menus` dump shows brightness, contrast, saturation, white
balance, exposure, focus, pan, tilt, zoom, and pan/tilt speed, and nothing else).

### Tiny 3 V4L2 control ranges (measured)

| Control | Range | Step | In degrees |
|---------|-------|------|-----------|
| `zoom_absolute` | 0–100 | 1 | — |
| `pan_absolute` | ±468000 | 3600 | ±130° in 1° steps |
| `tilt_absolute` | ±324000 | 3600 | ±90° in 1° steps |

Units are arc-seconds. Readback is live rather than an echo of the last write:
`tilt_absolute` was observed reporting `-298800` (−83°, the parked position) with
nothing having written it.

Two nodes share the card name `OBSBOT Tiny 3: OBSBOT Tiny 3 St` — `/dev/video2`
(`device_caps 0x04200001`, takes camera controls) and `/dev/video3`
(`0x04a00000`, metadata only, no `zoom_absolute`). Device detection must probe a
control rather than trust the card name.

The SDK reaches PTZ the same way: the `UVCControllerControls` table in
`libdev.so` decodes as the standard UVC control set (unit 1 Camera Terminal,
unit 2 Processing Unit), not as vendor commands.

## Bundled Open Source Components

The binary contains code from several open source projects. No attribution is provided in the SDK distribution.

| Library | License | Attribution required? |
|---------|---------|----------------------|
| bsdiff | BSD 2-clause | Yes |
| bzip2 | BSD-style | Yes |
| LZMA SDK | Public domain | No |
| nanopb (protobuf) | zlib license | Yes |
| Boost | BSL-1.0 | Yes |

## Supported Devices

### Enumeration can fail before product type is ever read

The `ObsbotProductType` table below is **not** the device support story. It lists
the types the SDK can *report* for a device it already owns — `productType()` is
a method on `Device`, so the enum is only reachable after `getDevList()` has
successfully claimed the camera.

A device can fail earlier than that. The **OBSBOT Tiny 3** (USB `3564:ff02`) is
opened by SDK v1.0.2, fails the UUID handshake, and is deleted before any
product-type branch runs:

```
d-d: sendMsgSync, cmd_id = 4; cmd_set = 5
d-d: try to resend packet: seq = 0, flag = 1
d-e: error occurred in getDevUuidR, ret_val = -3
d-e: uvc device init failed
```

`getDevList()` then returns empty. Consequently **adding a Tiny 3 value to
`ObsbotProductType`, or aliasing the Tiny 3 to a similar supported model, cannot
work** — there is no `Device` object on which to report a type. See
[ADR-002](adr/002-tiny3-control-path.md).

**This is resolved.** SDK **v2.1.0_8** does support the Tiny 3 and is now the
vendored SDK (`sdk/v2.1.0_8`, soname `libdev.so.1.0.0`); v1.0.2 is retained only
for rollback. The new `dev.hpp` declares `ObsbotProdTiny3 = 18` and
`ObsbotProdTiny3Lite = 19`, and the binary carries `kPidTiny3UvcUac`,
`kPidTiny3UvcRndis`, `ObsbotProdTiny3`, and `ObsbotProdTiny3Lite` — none of
which exist in v1.0.2. Tracking is Tiny-3-aware too:
`AiWorkModePortraitTrack = 14` is commented "Portrait tracking for tiny3", and
the status-code comments tag 11 and 12 as `[Tiny3]` human single and group
tracking.

The SDK is obtained from https://www.obsbot.com/sdk, which gates the download
behind a form rather than a password. Verify any new SDK by SHA256 and by
grepping for the product you need, not by its version label: an OBSBOT download
labeled "v2.1.0" was once byte-identical to v1.0.2. Note also that v2.1.0_8
ships two distinct x86_64 builds whose version numbers and dates disagree —
`libdev.so.1.0.0` (756 exported symbols) is a strict superset of
`libdev.so.1.0.3` (734) and is what `libdev.so` resolves to, so it is the one
vendored here despite the lower version number.

### Product types reported by the vendored SDK

The `ObsbotProductType` enum in `dev.hpp`:

| Enum Value | Device |
|-----------|--------|
| `ObsbotProdTiny` | Tiny (original) |
| `ObsbotProdTiny4k` | Tiny 4K |
| `ObsbotProdTiny2` | Tiny 2 |
| `ObsbotProdTiny2Lite` | Tiny 2 Lite |
| `ObsbotProdTinySE` | Tiny SE |
| `ObsbotProdMeet` | Meet (original) |
| `ObsbotProdMeet4k` | Meet 4K |
| `ObsbotProdMeet2` | Meet 2 |
| `ObsbotProdMeetSE` | Meet SE |
| `ObsbotProdMe` | Me |
| `ObsbotProdTailAir` | Tail Air |
| `ObsbotProdTail2` | Tail 2 |
| `ObsbotProdTail2S` | Tail 2S |
| `ObsbotProdHDMIBox` | HDMI Box |
| `ObsbotProdNDIBox` | NDI Box |
