# How MtkWlan works

This is the theory of operation, written so I can answer "why is it done this way" for any part of it. File names refer to `src/`. Where a function is ported from Linux mt76 or from itlmt7925, the source function is named in a comment next to it and `NOTICE.md` lists it per file.

## What macOS sees

The MT7925 is a PCIe device (`14C3:7925`) with one 64-bit memory BAR. Apple's Wi-Fi stack (IO80211Family) is private and tied to Broadcom hardware, so MtkWlan does not use it. Instead the driver does all of 802.11 itself and presents an `IOEthernetController` to macOS. The system sees an Ethernet card named `en1`, runs DHCP on it, and knows nothing about networks, passphrases or signal strength. That is why there is no Wi-Fi menu and why `tools/mtkwifi` exists.

Two classes:

| Class | Base | Files | Owns |
|---|---|---|---|
| `MtkWlan` (the core) | `IOService` | MtkWlan, MtkRegs, MtkDma, MtkFw, MtkScan, MtkJoin, MtkSec, MtkNet, MtkBa, MtkPower, MtkUserClient | PCI attach, BAR0, MSI, DMA rings, the MCU, firmware, scan, join, WPA2, 802.11 TX/RX, block ack, power tables, the work loop and its 1 ms timer, all `Mtk*` registry properties |
| `MtkWlanEth` (the front end) | `IOEthernetController` | MtkWlanEth | the `IOEthernetInterface` (`en1`), a gated output queue of 64 packets, the medium, link status, the MAC address |

The personality matches `IOClass MtkWlan` on `IOPCIPrimaryMatch 0x792514c3`. After bring-up the core creates the front end by class name, attaches it as its child and starts it. The two talk through a plain C table (`MtkCoreApi.h`), fetched with `callPlatformFunction("mtk-core-ops-v2", ...)`. The table is versioned both ways (`version`, `size`) so that a different front end, built as a separate kext, can attach later without a C++ link against the core. The core offers `tx`, `set_enabled`, `get_mac`, `get_link_info`, `scan`, `results`, `join`, `disconnect`, `status`; the front end gives back `rx_alloc`, `rx`, `rx_flush`, `link`, `tx_space`, `scan_done`, `join_result`.

## Bring-up

Everything runs from `MtkWlan::start` on the provider's start thread, then hands over to the 1 ms timer. Each step publishes a registry property (`MtkState` and one `MtkW<n>...` value per step) because the early work was done in macOS Recovery, where the kernel log is tiny and the release kernel ignores `msgbuf=`. `ioreg -r -c MtkWlan -l -w0` still is the main diagnostic.

1. **PCI and registers** (`MtkWlan.cpp`, `MtkRegs.cpp`). Enable memory and bus master, map BAR0. The chip's register space is reached through two remap windows (L1 and L2) exactly as `mt7925/pci.c` does; `MtkRegs.cpp` keeps the same helper shape. Then the driver ownership handshake from `mt792x_core.c`: the firmware owns the chip at power-on and the host has to claim it before touching WFDMA.
2. **WFDMA rings and MSI** (`MtkFw.cpp`, `MtkDma.cpp`). A WFSYS reset, then the TX and RX rings (`mt792x_dma.c`, `dma.c`): descriptors in IOKit DMA memory (`IODMACommand`, 32-bit addressable), one MCU command ring, one data ring, the MCU event ring and the data RX ring. MSI is enabled and the handler only counts and acknowledges; all real work happens in the timer. This is the one place the driver is deliberately unlike Linux: RX is polled every millisecond from the work loop, which costs a little latency and keeps every path under one gate.
3. **Firmware** (`MtkFw.cpp`). Two blobs from linux-firmware, embedded at build time by `tools/gen_firmware.py` with their SHA-256 (checked against `firmware/SHA256SUMS`, and published in the registry): the patch (`WIFI_MT7925_PATCH_MCU_1_1_hdr.bin`) through the patch semaphore, then the RAM code (`WIFI_RAM_CODE_MT7925_1_1.bin`) region by region, then the "firmware start" command, as in `mt7925/mcu.c` and `mt76_connac_mcu.c`. Done when the firmware reports ready in the MCU state register.
4. **Station setup** (`MtkScan.cpp`, `hwInit`). UNI commands over the MCU ring: NIC capability (which tells the firmware version, the MAC address from EEPROM and the HW type), the EEPROM read, the regulatory domain (`US`), DEV_INFO and the first BSS_INFO for a station interface. The short command sequence after firmware load follows itlmt7925, which was the first driver to get this card up on macOS; every value was checked against Linux and where the two differ the driver follows Linux.
5. **Power tables** (`MtkPower.cpp`, 0.7.0). The RAM firmware carries country limit tables (CLC). The driver finds the region matching the EEPROM HW type and sends the `US` rules with UNI SET_POWER_LIMIT, then per-rate limits (SKU) for every channel, three channels per message, as `mt7925_mcu_set_clc` and `mt7925_mcu_rate_txpower_band` do. Without this the firmware transmits at a conservative default and upload speeds stay low. `MtkPower = false` in `Info.plist` skips it.
6. **Front end** (`MtkWlanEth.cpp`). Started right after bring-up, before any network exists, with the link published as valid and down. configd then sees "no carrier" and waits, instead of starting DHCP on a dead interface. The MAC address is the firmware's with the locally administered bit set, so the interface address and the on-air address match.

From here on the timer runs: every millisecond it drains the RX rings, dispatches MCU events, and services the scan, join, security, connection manager and block-ack machines in turn.

Steps 2 and 3 live in `hwStart` so they can run again (0.12.0). If they fail at attach, usually because a warm restart left the old firmware running with its DMA pointed at freed memory, `bringUp` does one more WFSYS reset and a second try, then a PCIe function level reset (`pciFlr`, only if the card advertises FLR) and a third. On restart and power off the core is in the power plane and `systemWillShutdown` quiesces the chip on the work-loop gate: interrupts off, driver own, DMA off, WFSYS reset, bus master off, as `mt7925_pci_shutdown` does. `MtkAttachRecover`, `MtkFlr` and `MtkPmJoined` show what happened.

## The link supervisor

`stallService` runs once a second and feeds counters to `MtkSuper.c`, a small state machine with no kernel calls (host-tested in `tests/mtk_super_test.c`). While the port is open:

- No frame received for `MtkStallSec` seconds (default 8): send one null frame with Tx status. Acknowledged means a quiet link and nothing happens. Not acknowledged means the AP dropped us without a deauth, and the link goes down the same way as after a deauth, so the manager rejoins. No status at all means the chip is stuck.
- Frames queued and no Tx token released for `MtkStallSec` seconds, or `fwDead` set: the chip is stuck.

A stuck chip gets `reload`: the link goes down (a native front end sees a link loss), WFSYS reset, the rings reset in place, the firmware loaded again, `hwInit` with the current station address kept, then a scan and a rejoin of the wanted network. The front end and its interface stay attached. At most three reloads per ten minutes. `MtkStall` has the counters and `MtkStallPostMortem` the ring indexes and PCI state from just before the last reload. `MtkStallSec = 0` or boot-arg `mtkstall=0` turns it off, and `kMtkUcTune` key 4 freezes the data ring on purpose to test the path. The idea of a supervisor that resets and reloads comes from thanhnndev's ga403wm-hackintosh [5f00557](https://github.com/thanhnndev/ga403wm-hackintosh/commit/5f00557b5ed4377b6cb0f45a4e1c9cab5503163f).

## Scanning

`scanStart` sends a hardware scan (UNI SCAN, all channels including DFS on 5 GHz, with the SSIDs of saved networks as directed probes). The firmware returns beacons and probe responses as frames on the MCU ring; `rxFrame` classifies them, `mtkParseFrame` pulls out SSID, channel, capability, RSN or WPA element and the HT/VHT elements. Each network goes into a cache of 48 entries keyed by BSSID with its security type (`mtk_rsn_parse` in `MtkSupp.c` decides whether the driver can join it: AKM PSK with pairwise CCMP, group CCMP or TKIP), RSSI from the RCPI in the RX descriptor, and a last-seen time. Entries unseen for 60 s are dropped. Beacons heard while associated keep the cache fresh. Scans start only while idle or connected, never during an authentication, and no join starts while a scan runs.

## Joining

`joinStart` / `joinService` is a state machine with deadlines and four tries per phase:

1. **ROC**: ask the firmware to stay on the AP's channel (remain-on-channel).
2. **Authentication**: open-system auth frame, wait for the response.
3. **Association**: association request with the supported rates, HT and VHT capabilities (80 MHz, 2 streams) and, for WPA2, the RSN element (group cipher = the AP's, pairwise CCMP, AKM PSK). Wait for the response, read the AID.
4. **Handshake** (WPA2 only): see below.
5. **Up**: BSS_INFO with the connected state, STA_REC with the HT/VHT/rate TLVs in the order Linux sends them (the firmware rejects other orders), the EDCA parameters from the association response, then the firmware does rate control. The port opens and the front end reports link up at the negotiated speed.

A deauthentication from the AP during the join counts as a failure. Right after our handshake message 2 it is reported as "wrong passphrase" (that is how most APs answer a bad key); otherwise as "AP left". Both back off.

## WPA2-PSK

Three files, with no OS calls in the first two so they can be built and tested on the host:

- `MtkCrypto.c`: SHA-1, HMAC-SHA1, PBKDF2 (passphrase to PMK), the 802.11 PRF (PMK to PTK), AES-128 and RFC 3394 key unwrap (for the GTK). Checked by `tests/mtk_wpa_test.c` against the FIPS, RFC and IEEE test vectors.
- `MtkSupp.c`: the supplicant state machine for the 4-way handshake and the group key handshake. The rules it enforces come from IEEE 802.11-2020 12.7.6 and from reading hostap's `wpa_supplicant`: a frame is dropped unless its replay counter is above the last one that carried a verified MIC; message 3's ANonce must equal message 1's, its MIC must verify, and the RSN element inside it must equal the AP's beacon element (downgrade check); message 4 is sent before the PTK is installed; a key equal to the installed one is never installed again (the KRACK rule); one SNonce per handshake. The host tests run a simulated authenticator through the normal, retransmit, wrong-key and rekey cases and compare every derived value with an independent Python implementation (`tests/oracle.py`).
- `MtkSec.cpp`: the glue. EAPOL frames from the AP are queued by `rxFrame` and fed to the supplicant from the timer. The supplicant's callbacks only queue a frame to send or stash a key; nothing in them touches the rings or the firmware. `secInstallService` then installs the keys from the timer in the order Linux and itlmt7925 use: wait until message 4 is on the air (its TX-free seen, 100 ms bound), send BSS_INFO with the WPA2 security values, the pairwise key (STA_REC_KEY_V3 on WCID 1), the group key (WCID 19; 16 bytes CCMP or a 32-byte TKIP key with the MIC halves swapped), then open the port. A later group-key handshake installs the new GTK the same way without touching the port.

**Port gating** is the driver's, not the firmware's. While the port is closed `txEthernet` drops everything but EAPOL. Once the PTK is installed every data frame carries the Protected bit and TXD3 PROTECT_FRAME, and the firmware adds the CCMP header and packet number. On receive, protected data must have been decrypted by the hardware with the expected cipher (RXD2 SEC_MODE), plaintext data on a WPA2 association is dropped, and the CCMP packet number (from RX descriptor group 1) is checked when the frame leaves the block-ack reorder buffer in order, as `mt76_check_ccmp_pn` does: one counter per key and TID, reset by a PTK install, seeded from the key RSC by a GTK install. Every drop has a counter in `MtkW2Sec`.

The PMK is derived in user space by `mtkwifi add`, stored root-only, and handed to the driver through the user client at join time. The driver never publishes a key, PMK or passphrase anywhere.

## The connection manager

`cmService` decides what to join. A wanted network (from `mtkwifi join` or a front end's `join`) is kept: rejoined 500 ms after a loss, retried with backoff (5, 10, 20, 40, 60 s) after failures, rescanned every 5 s while out of range (15 s after the first 30 s). When nothing is wanted or the wanted network has been out of range for 30 s, the driver may join an open network whose name is listed exactly in `MtkAutoOpenSsids`, but not before 20 s after the first scan (so the autojoin daemon gets its turn first) and not while a user-space scan is pending. While on that fallback it keeps looking for the wanted network after every scan (one every 30 s) and switches back when it reappears. `mtkwifi disconnect` turns the fallback off until the next `join`.

`MtkAutoNetworks` (0.12.0) does the same for WPA2 networks on machines without the native menu: entries of `Ssid` plus `Pmk` (64 hex digits from `mtkwifi pmk`). When one is in range and nothing is wanted, it becomes the wanted network as if it had been joined through the user client. The keys are removed from the registry after start. Neither list is used while the native front end drives the joins.

## The data path

Ethernet frames from the output queue become 802.11 data frames: `txEthernet` builds the 802.11 header (ToDS, our MAC, the BSSID) and the LLC/SNAP header, then the TX descriptor (`write_txwi` and `write_hw_txp` from mt76) and puts the frame on the data ring. TX completions arrive as TX-free events with the sampled rate, power and try count, which feed the `MtkW8*` counters. In flight is capped at 32 frames and the output queue at 64, which is what took upload latency from over a second to tens of milliseconds in 0.5.1.

Received data frames are translated back to Ethernet (`rxFrame`, then `rx` on the front end with packets the front end allocated through `rx_alloc`). EAPOL goes to the supplicant queue, block-ack action frames to the block-ack machine, deauthentication to a pending-teardown flag that the timer acts on, beacons to the cache. A keepalive null frame goes out when nothing has been sent for a while, and a rejoin starts after a link loss.

## Block ack

`MtkBa.cpp`. On the receive side the driver accepts the AP's ADDBA request, gives the firmware a recipient STA_REC_BA, and reorders by sequence number with the algorithm from mt76's `agg-rx.c` (release head, release on timeout at HZ/10, BlockAckReq handling). Frames waiting in the buffer are the front end's mbufs; dropped ones are freed. On the transmit side, once data flows the driver sends an ADDBA request for TID 0 and, on success, an originator STA_REC_BA; the firmware then builds A-MPDUs itself. `MtkRxBa` and `MtkTxBa` in `Info.plist` switch either side off, in which case ADDBA requests are declined.

## The user client

`MtkWlanUserClient` (`MtkUserClient.cpp`) is created per process that opens the service. Its five methods (scan, results, join, disconnect, status) run under the core's work loop gate through `runAction`; they set flags or copy snapshots and never issue a firmware command themselves, so a call waits at most for one command to finish (3 s worst case). Scan, join and disconnect need admin; status and results do not. The data shapes are fixed-size integers in `MtkUserApi.h`, the same in the kernel and in a 64-bit user process.

## Threading

One gate. The core makes the work loop; the front end returns the same loop from `getWorkLoop()`, so its output queue and the IONetworkController command gate serialise with the timer.

| Path | Context |
|---|---|
| `hwInit` (NIC capability, domain, power) | provider start thread, before the timer runs |
| `tick`: ring drains, pending teardown, `secService`, `joinService`, `scanService`, `cmService`, block ack, keepalive, link publication | core work loop, timer action; the timer is re-armed first |
| `uniCmd` (every firmware command) | tick or bring-up thread only; a call from inside `dispatchRx` fails with -13 and is counted; three timeouts in a row set `fwDead` and stop the machines |
| `rxFrame` | classifies, queues and delivers only |
| user client calls | `workLoop->runAction`; set flags or copy snapshots |
| supplicant callbacks | queue an EAPOL frame or stash a key; never poll, never command |
| `tx` from the output queue, `set_enabled` from enable/disable | the core gate |
| MSI handler | primary interrupt: count and acknowledge |

Callbacks must not sleep and must not call back into `tx` or `set_enabled`. The front end's `stop` removes its callbacks under the gate before detaching `en1`, so a running tick never calls into a dying front end.

## Registry properties

`ioreg -r -c MtkWlan -l -w0` shows `MtkState` (the current step as text), `MtkScan` (one line per cached network: SSID, channel, RSSI, security, `!` when not joinable), `MtkW1Bss` / `MtkW1Scan` / `MtkW1Join` (the machines' counters and last results), `MtkW2Sec` (handshake messages seen, bad MICs, replays, reinstall skips, missing PNs, ICV errors, cipher mismatches, unprotected / port / translation drops, key installs), `MtkCm` (connection manager decisions), `MtkW2Cmd` (in-dispatch violations, timeouts, failures, dead), the command results `MtkW2BssSec` / `MtkW2PtkInstall` / `MtkW2GtkInstall`, and the older `MtkW4*` to `MtkW9*` sets (scan, join, data path, rates, block ack). Never a key.

## What is not there

- WPA3-SAE, PSK-SHA256 (EAPOL descriptor version 3), management frame protection, fast transition and enterprise authentication.
- Roaming between BSSIDs of one SSID.
- HE (802.11ax) and EHT; the association is VHT at 80 MHz.
- A native front end that gives macOS a real Wi-Fi interface (menu, AirDrop, Continuity). The ops table is shaped for it, and that kext is now [mt7925-hackintosh-wifi-native](https://github.com/watchthelight-labs/mt7925-hackintosh-wifi-native).
- Sleep and wake: not upstream. The `stealth-a16` fork adds it, see "Sleep and wake (stealth-a16 fork)" below.
- Matching a SCAN_DONE or ROC grant event to the request that caused it; the machines use deadlines instead.
- Interrupt-driven RX.

## Testing

`bash tests/run.sh` builds `MtkCrypto.c`, `MtkSupp.c` and `MtkSuper.c` with the host clang and UBSan, runs the vectors, the simulated handshakes and every supervisor decision, and compares the printed values with `tests/oracle.py`. On the machine, `tools/wifi.sh` records the interface state, runs a router ping, a download and an upload through Cloudflare's speed endpoint with ping alongside, and saves every `Mtk*` property. The numbers in the README come from that script.

## Sleep and wake (stealth-a16 fork)

The core is a child of the PCI device in the power plane (it joined for the restart quiesce in 0.12.0). `setPowerState`
now does real work on the core's gate, unless `MtkSleep` is false or the boot-arg `-mtknosleep` is set:

- State 0 (before the PCI device drops to D3: sleep, hibernation). `sleepDown` in `MtkNet.cpp` does what mac80211 does
  for a driver without WoWLAN: leave the access point while the firmware still answers (deauthentication reason 3,
  STA_REC and BSS_INFO removed), so the front end sees the link go down; a join or scan in flight ends with a result
  (`MTK_JOIN_AP_LEFT`, scan status -1). Then the tick stops, the firmware is marked dead so nothing waits on it, and
  `sleepHw` in `MtkFw.cpp` follows `mt7925_pci_suspend`: driver own, wait for the WFDMA to go idle, DMA off, host
  interrupts off, ownership back to the firmware (`mt792xe_mcu_fw_pmctrl`), bus mastering off. The deep-sleep and
  HIF-suspend MCU commands Linux sends are left out because the wake path reloads the firmware anyway.
- State 1 (after IOPCIFamily restored config space). `wakeUp` / `wakeHw`: memory and bus master on, driver own
  (`mt792xe_mcu_drv_pmctrl`), whether the chip kept power is read as `mt792x_dma_need_reinit` does (the WFDMA dummy
  register loses the NEED_REINIT bit), then the attach bring-up whatever the answer: WFSYS reset, rings reset, patch
  and RAM download, with the same retries as attach (WFSYS reset again, then a function level reset). `hwInit(true)`
  readies the station with the address macOS last set (a `set_mac` that arrives while asleep is only stored) and the
  current country. Then the tick restarts and the manager rejoins the wanted network without backoff; a native front
  end hears the link come back as after a reload, and macOS's own auto-join is answered as usual.
- A wake that fails leaves the firmware marked dead, the tick stopped and the link down; the next sleep and wake try
  again. Nothing panics.

Properties: `MtkSleep`, `MtkSleeps`, `MtkSleepHw`, `MtkWakes`, `MtkWakeMs`, `MtkWakePower` (`kept`, `lost (registers
reset)`, `card not on the bus`), `MtkWakePowerLost`, `MtkWakeFails`, `MtkWakeLastResult`, `MtkWakeConnOnMisc`,
`MtkWakeDrvOwnTries`. None of this has run on hardware yet.
