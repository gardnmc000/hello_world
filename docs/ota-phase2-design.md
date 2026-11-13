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

## 6a. Releasing a version (as built and tested, 1.3.0 to 1.4.5)

`server/deploy/release.sh` does steps 3 and 4 with checks (app image, size, signed with
our key, no version number reused for different contents, upload SHA-256), and asks
before changing anything:

```
bash server/deploy/release.sh status
bash server/deploy/release.sh publish build/bird_watch.bin f30f1c      # or --all
bash server/deploy/release.sh units f30f1c <id>...                     # or --all
bash server/deploy/release.sh withdraw
```

The steps by hand:

1. Set `version.txt` to a version **no unit runs and no test image used**. The server
   offers its image whenever the unit's version string differs, and a unit skips a
   version it has blocked, so a reused number can be silently skipped or blocked.
2. Build with **IDF: Build** (or `tools\idf.ps1 build`). The build signs
   `build\bird_watch.bin` with `C:\Users\gardn\keys\bird_watch_signing.pem`.
3. Copy it to the server under a temporary name, then rename it, so a unit is never
   offered a half-copied file:
   ```
   scp build/bird_watch.bin mike@10.147.20.210:firmware/bird_watch.new
   ssh mike@10.147.20.210 "mv ~/firmware/bird_watch.new ~/firmware/bird_watch.bin"
   ```
4. Choose who gets it in `~/firmware/devices.txt`: one unit id per line (the 6 MAC hex
   digits in its clip names). Start with the bench unit; add field units once it has run
   through a trial. Deleting the file offers the image to every unit.
5. Units pick it up at their next connection: at boot, after sending clips, or at the
   daily check. Watch progress with `journalctl --user -t socksrvr -f` and
   `cat ~/firmware/status/<id>` (version, battery, trial, rollbacks, `last_reset`).

Notes:
- Only units on 1.3.0 or later can update over the air. Older units need one USB or
  SD-card update first.
- The server offers by difference, not by "newer": putting an older signed image up makes
  units install it. That is a deliberate way back from a bad release, but it means the
  file on the server must always be the one intended.
- To withdraw a release, delete `~/firmware/bird_watch.bin`; units are then told `NONE`.
  A unit already part-way through a download resumes it only if the same image returns.

## 6c. Unit settings (from 1.5.0)

One file for every unit, of `key=value` lines. It is edited in one place only,
`server/config.cfg` in the repository, which lists every setting with its default and its
range (commented out = firmware default). `bash server/deploy/release.sh config` checks
each value against the firmware's own table (`main/bw_settings.c`), lists every value
beside the one on the server now, and if anything differs asks to publish. The server
copy, `~/firmware/config.cfg`, gets only the settings, without comments.

- Units on 1.5.0+ send `FWH2`: the HELLO gains the CRC-32 of the settings text they last
  applied and how many values they refused. When that CRC differs from `config.cfg`'s, the
  server sends the file (`CFG1`, length, text) before the firmware offer; otherwise `CFG0`.
  Units on 1.4.x send `FWH1` and see no change.
- The unit checks each value against its range (out of range: the default is kept and
  the refusal counted), stores the text in NVS and applies it at once; it is applied
  again at every boot. Settings missing from the file take the compiled defaults.
- `status/<id>` shows `config` (the unit's CRC), `config_refused` and `config_current`.
- Clip length is limited to 10 s by the upload: the server's `OK###` acknowledgement
  allows at most 999 packets of 1024 bytes.
- Active hours are GMT, the unit's clock. Outside them the sampler does not listen;
  reporting and firmware checks carry on.

**Status report (from 1.5.2).** The HELLO stays FWH2 (battery and last reset are in it).
Everything else goes in a status report, a short `key=value` text file the unit writes and
uploads on every connection (startup and reporting), before any clips, through the clip
upload. Its name is a clip name with `S` for `D` and `.txt` for `.wav`, for example
`Sf30f1c26317171010.txt`. The server keeps reports in `~/clips/<id>/reports/`, beside that unit's
clips in `~/clips/<id>/` (a resent report replaces, never appends), and the unit deletes its copy whether or
not it was sent; the next connection makes a fresh one. The report holds: time, version,
partition, trial, rolled-back version, last reset, uptime, battery at rest and under load,
free memory, the network (operator from `AT+COPS?`; MCC+MNC, technology and band from
`AT+QNWINFO`; signal from `AT+CSQ`), the raw `AT+QENG="servingcell"` reply (on trial
until its format is confirmed, then parsed), and every setting, marked where it came from
the server. See it with `release.sh report <id>` (the latest) or `release.sh report <id> 10`
(a list); `release.sh status` names each unit's latest report.

From 1.5.3 the serving cell is also decoded (confirmed format on this EG915, LTE:
state, technology, duplex, MCC, MNC, cell id (hex), PCI, EARFCN, band, UL/DL bandwidth,
TAC (hex), RSRP, RSRQ, RSSI, SINR, then one more) into `cell_...` lines, with the cell id
and TAC also in decimal for cell databases, and `AT+QENG="neighbourcell"` is added as raw
`neighbour=` lines (on trial). esp_modem keeps only the last line of a reply and cuts
every reply at `CONFIG_ESP_MODEM_C_API_STR_MAX` bytes (raised from 128 to 1024 in
`sdkconfig`); every AT call in `mgnetwork.c` goes through `modem_at()`, which receives
into a buffer of that size, and multi-line replies use `modem_at_all_lines()`.

1.5.1 (bench unit only) sent the network in a 120-byte `FWH3` HELLO instead; the server
still accepts it until no unit runs 1.5.1.

## 6b. Testing a dropped download (test switch)

`~/firmware/test_cutoff` holding a byte count (for example `300000`) makes the server
stop sending at that byte, as if the connection had dropped. The unit logs
`Download stopped at ... it resumes next time`; delete the file and the next connection
resumes from that point. **Delete it after testing**: while it exists, no unit can
complete a download.

## 7. Decisions (agreed 2026-11-11)

1. Same port 3590 with a new signature.
2. Rollout list: yes.
3. Signing key in `C:\Users\gardn\keys\`, outside the repo; backup on OneDrive,
   encrypted (password-protected archive or Personal Vault).
4. Restart straight after a completed download.

Later decisions (2026-11-12):
5. The reporting task checks for firmware after sending all clips on every connection,
   and connects once a day for the check even with no clips (`BW_OTA_CHECK_INTERVAL_MS`).
   From 1.5.1, a connection with clips to send checks first too, without downloading, so
   new settings are never held up by a backlog. It comes back after the clips only if an
   offer is worth downloading (or the first check failed). The server logs that first
   check as `declined the offer` when there is one.
6. The startup connection only syncs the time and checks for firmware; it does not send
   clips, in testing or production.
7. Clip names are exactly 22 bytes, checked by the unit (others are ignored) and by the
   server. The server's `D_` exception stays until unit D_4c30, in a remote field, is
   replaced.
