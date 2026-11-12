# OTA Phase 2: firmware download from the server

Status: draft for review, 2026-11-11.

Phase 1 (done) installs `/sdcard/firmware.bin` at boot, with rollback if the new
firmware fails before it is marked valid. Phase 2 adds a way for the unit to fetch
that file from the server. The download only ever writes to the SD card; flash is
written by the Phase 1 code, so a dropped connection costs time, never a broken unit.

## 1. Overview

```
 unit                                   server (socksrvr, port 3590)
 ----                                   ------
 connect, send FW signature   ───────►
 HELLO: id, version, battery,
        last rolled-back version ───►   log it; look up the offered firmware
                              ◄───────  OFFER: none | version, size, SHA-256
 (skip if same version, the
  rolled-back version, or battery low)
 GET: offset                  ───────►
                              ◄───────  raw image bytes from offset to end
 append to /sdcard/fw_dl.bin
 (drop?  resume from file size next time)
 complete: check SHA-256, rename to
 /sdcard/firmware.bin, restart
 boot: Phase 1 installs it (signature checked), trial, valid after contact or 1 h
```

## 2. Protocol

Same port and server as clip uploads (3590), so no new router port forward or
service. The first 8 bytes choose the conversation:

| Signature | Conversation |
|---|---|
| `19 57 55 aa c0 03 f0 0f` | clip upload (unchanged) |
| `19 57 55 aa c0 03 f0 1f` | firmware check / download (new) |

All fields are fixed size and little-endian (both ends are little-endian).
Strings are NUL-padded.

**HELLO** (unit → server, 64 bytes)

| Bytes | Field | Notes |
|---|---|---|
| 0–3 | `"FWH1"` | message and protocol version |
| 4–11 | device id | the 6 MAC hex digits used in clip names, e.g. `f30f1c` |
| 12–43 | running version | from `esp_app_get_description()` |
| 44–45 | battery mV | `uint16`, reading taken before the modem was powered |
| 46–47 | flags | bit 0: running firmware is still on trial; bits 8–15: why this boot started (`esp_reset_reason_t`, from 1.4.5; 0 = unknown) |
| 48–63 | rolled-back version | first 16 bytes, empty if none |

The server logs every HELLO, so it also serves as the version and rollback report
the plan asked for. It keeps the latest one per unit in `~/firmware/status/<id>`.

**OFFER** (server → unit, 72 bytes)

| Bytes | Field |
|---|---|
| 0–3 | `"NONE"` (nothing more follows; both sides close) or `"OFFR"` |
| 4–35 | version |
| 36–39 | size in bytes (`uint32`) |
| 40–71 | SHA-256 of the whole image |

**GET** (unit → server, 8 bytes): `"GET1"`, then the start offset (`uint32`).
The server replies with the bytes from that offset to the end of the image, then
closes. TCP already guarantees order and integrity in transit; the SHA-256 check
at the end catches anything else, including a server-side file change mid-download.

## 3. Server

- Firmware lives in `~/firmware/bird_watch.bin` (the signed build output, copied as is).
- The server reads the version from the image's own app description (offset 32,
  magic `0xABCD5432`) and computes its SHA-256 with OpenSSL, which it already links.
  There is no separate version file to get out of step.
- **Rollout list:** if `~/firmware/devices.txt` exists, only the ids listed in it are
  offered the image. Proposed use: list only the bench unit until a release is proven,
  then add the field units.
- **Clip filename check (security fix, done first):** upload names may contain only
  `A–Z a–z 0–9 _ . -`, may not start with `.`, and are at most 22 bytes; anything else
  is refused and logged. This stops writes outside `~/bin`, without rejecting the old
  `D_xxxx_...` names that unit D_4c30 still sends.

## 4. Unit

New function `ota_check_server()` in `ota_update.c`, called while PPP is up:
at the startup time-sync connection now, and in the reporting cycle once it is enabled.

1. Send HELLO; read OFFER. `NONE` → done.
2. Skip the offer if its version is the running one, or the last rolled-back one
   (otherwise a bad release would be downloaded again on every connection), or the
   battery is below **3600 mV**.
3. Staging files:
   - `/sdcard/fw_dl.inf` holds the offered version, size and SHA-256.
   - `/sdcard/fw_dl.bin` holds the bytes received so far.
   - If the `.inf` matches the offer, resume from the size of `.bin`; otherwise
     delete both and start again.
4. Send GET with that offset; append to `.bin`, feeding the watchdog. A timeout or
   drop leaves the partial file for next time.
5. When the size is reached, check the SHA-256 (mbedTLS). Mismatch → delete and
   start again next time. Match → rename to `/sdcard/firmware.bin`, delete `.inf`.
6. Restart so Phase 1 installs it:
   - startup connection: restart right away (the sampler has not started);
   - reporting cycle: set a flag that the sampler checks between samples, so a
     clip being written is never cut short.

Battery is also checked in `ota_install_from_sd()`: below 3600 mV the install waits
for a later boot.

## 5. Signing

`CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT` with the RSA-3072 scheme. Checked in
the ESP-IDF v5.3.1 source:

- The build signs `bird_watch.bin` automatically with the key file named in
  `CONFIG_SECURE_BOOT_SIGNING_KEY`.
- `esp_ota_end()` refuses an image that isn't signed by a trusted key, so the
  Phase 1 code already enforces it (the file becomes `fw_bad.bin`).
- Without hardware secure boot, the trusted key is **the key the running firmware
  was signed with** (`secure_boot_signatures_app.c`). So:
  - The first signed release can be installed by any route, because the current
    unsigned firmware has no checking code.
  - From then on, only images signed with the same key are accepted over the air
    and from the SD card.
  - USB flashing is never blocked (no eFuses are burned), so a lost key or a mistake
    costs a USB flash of each unit, not a dead unit.
- The bootloader is unchanged (boot-time checking isn't available for the RSA scheme
  without hardware secure boot, and adds little without it).
- The key is generated once:
  `espsecure.py generate_signing_key --version 2 --scheme rsa3072 <file>.pem`.
  It must never be committed; keep it outside the repo and keep an offline backup.

## 6. Order of work

1. Server: clip filename check; first real use of `deploy.sh`.
2. Signing: generate the key, enable it, build 1.1.0. On the bench unit, check that a
   signed image installs from SD and an unsigned one becomes `fw_bad.bin`.
3. Server: firmware HELLO/OFFER/GET, rollout list, status files.
4. Unit: `ota_check_server()`, staging and resume, battery checks, restart flag.
5. End to end on the bench unit: normal update, a dropped download that resumes,
   a corrupted image, a rolled-back release that is not downloaded again.
6. Field units. Units still on pre-Phase-1 firmware (D_4c30) need one USB or SD-card
   update before they can update over the air.

## 7. Decisions (agreed 2026-11-11)

1. Same port 3590 with a new signature.
2. Rollout list: yes.
3. Signing key in `C:\Users\gardn\keys\`, outside the repo; backup on OneDrive,
   encrypted (password-protected archive or Personal Vault).
4. Restart straight after a completed download.
