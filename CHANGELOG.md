# Changelog

## 0.12.1-stealth.1 (fork `xarmaq/mt7925-hackintosh-wifi`, branch `stealth-a16`, 2026-10-25)

- Sleep and wake. System sleep tears the station down (deauthentication, firmware records removed, link down to the
  front end, join or scan in flight ended with a result), then quiesces the chip as `mt7925_pci_suspend` does (DMA idle
  and off, interrupts off, fw own, bus master off). Wake runs the attach bring-up again (drv own, WFSYS reset, rings,
  firmware download, station with the address macOS set) and the manager rejoins the wanted network at once. Whether
  the chip lost power is detected as Linux does and published (`MtkWakePower`), but the full bring-up runs either way.
  A failed wake leaves Wi-Fi down without touching the chip until the next wake. Details in `docs/HOW-IT-WORKS.md`.
- `MtkSleep` (Info.plist, default true) and the boot-arg `-mtknosleep` turn it off; sleep is then only acknowledged, as
  in 0.12.1.
- CI: `.github/workflows/stealth.yml` builds the kext with `tools/build-mac.sh` and publishes stealth tags as releases.
- Written for the MSI Stealth A16 AI+ A3XWJG (MT7925 subsystem `105B:E138`). Nothing in the driver depends on the
  subsystem id. Not tested on hardware yet.

## 0.12.1 (2026-10-23)

- Core API version 5 for the native front end ([mt7925-hackintosh-wifi-native](https://github.com/watchthelight-labs/mt7925-hackintosh-wifi-native) 0.4.13):
  - `get_rate_info` reports the link's PHY (legacy, HT, VHT), MCS, streams, width, guard interval and rate. It uses the firmware's TX status records when there are any, and the best rate the association allows otherwise.
  - `set_country` takes the country macOS asks for. The CLC power table for that country is used, and the channel domain is sent again between scans.
  - Outside the US and its territories, channels 12 and 13 are added as passive, and 149 to 165 become passive too. Passive means listen only until an access point is heard, so a country can only make the radio quieter than the US set, never louder.
- `MtkCountry` property: the current country, its channel count and the firmware's answer.
- Tested on the author's Legion 5 (N-261): joins at boot. macOS shows 11ac, 80 MHz, MCS 9, 2 streams, short GI, 866 Mbit/s for a VHT80 access point. That is the negotiated maximum, because this link produced no TX status records.

## 0.12.0 (built 2026-10-22; first boot 2026-10-23 together with 0.12.1)

- Restart and power off quiesce the chip (interrupts off, driver own, DMA off, WFSYS reset, bus master off). Before this a warm restart could leave the firmware running and the next boot failed with `W3 fail: patch download`, sometimes with the card gone from the bus until a full power off ([#1](https://github.com/watchthelight-labs/mt7925-hackintosh-wifi/issues/1)).
- Attach recovers a wedged chip: one more WFSYS reset and a second try, then a PCIe function level reset and a third. The FLR idea comes from flxxxxddd's [SimpleMtkWlan PR #7](https://github.com/laobamac/SimpleMtkWlan/pull/7); the code here is our own.
- Link supervisor: a link that stays associated but stops working is noticed within about 8 s. An AP that no longer acknowledges us gets a rejoin; a chip whose firmware or DMA stopped gets a reset, a firmware reload and a rejoin without detaching the interface. Inspired by thanhnndev's link-loss recovery ([ga403wm-hackintosh 5f00557](https://github.com/thanhnndev/ga403wm-hackintosh/commit/5f00557b5ed4377b6cb0f45a4e1c9cab5503163f)). `MtkStallSec` (default 8, 0 = off), boot-arg `mtkstall=`, properties `MtkStall` and `MtkStallPostMortem`.
- `MtkAutoNetworks`: WPA2 networks the driver joins by itself when the native menu is not there (a USB installer, Recovery on another macOS build). `mtkwifi pmk <ssid>` prints an entry. Asked for in [#3](https://github.com/watchthelight-labs/mt7925-hackintosh-wifi/issues/3).
- `tools/recovery/`: a Terminal helper for Recovery and the installer that scans, joins with a typed password (not stored) and saves a capture. On 26.7.1 Recovery the native Wi-Fi menu should work by itself: its boot kernel collection has the same IO80211Family as the installed system.
- `mtkwifi` builds for macOS 10.15 and later, so it runs from older installers too.

- The BSS cache holds 128 networks instead of 48, and when it is full it drops the oldest network the running scan has not heard. In a crowded place one scan can hear more than 48 networks; the 5 GHz channels are scanned after 2.4 GHz, so the 2.4 GHz finds were the oldest and got evicted before the scan ended. Found and fixed first by thanhnndev in [ga403wm-hackintosh 4257353](https://github.com/thanhnndev/ga403wm-hackintosh/commit/4257353065b34181b42993c946fb0956c47c6e32) (Clear BSD). `MTK_SCAN_MAX` stays 48: it is now only the most results one `get_scan_results` call returns.
- WPA2 joins keep the join ROC until the keys are installed instead of releasing it right after association. Releasing it just before message 2 made the chip miss message 3 and its retries for several ms, so the handshake could end with reason 15. Ported from mt76 [fddc43d](https://github.com/openwrt/mt76/commit/fddc43d51481ca8c0e2d16a88caaca528791f359) (Mark Anthony Agarro, Devin Wittmayer). We had not seen this failure ourselves.

## 0.11.3 (2026-10-18)

- Tx data ring 256 -> 1024 slots. The in-flight cap stays at 128 frames with a Tx block-ack session (32 without).
- New user-client selector `kMtkUcTune` (5, administrator only) sets the in-flight cap live, so a value can be tested without a reboot. Keys in `MtkUserApi.h`; the current caps show in `MtkW6TxInflightCaps`.
- A sweep on the author's laptop (VHT 80 MHz, 2 streams, -53 dBm) gave about 410 Mbit/s of UDP for every cap from 128 to 512 once the system had settled. The first few minutes after login are slower at any cap: macOS scans about 20 times then. 0.11.2, which raised the cap to 512, was never released.

## 0.11.1 (2026-10-18)

- Front-end race fixed. With `mtkfe=native` the core used to start its Ethernet front end after a fixed 15 s and then refuse the native one for the whole boot, so on some boots the Wi-Fi menu was missing and the card showed up as an Ethernet port. The native front end spends 0 to 26 s inside Apple's `IO80211SkywalkInterface` start on 25G241 (five boots measured), so 15 s was a coin flip. A front end can now claim the core at probe (`MTK_CORE_FE_CLAIM` in `MtkCoreApi.h`); the core then waits up to 90 s, and a claimed front end that gives up declines at once. New properties `MtkFrontEndClaimMs` and `MtkFrontEndBoundMs` show the timing. Needs MtkWlanNative 0.4.10 for the claim; with an older front end nothing changes.
- Matches PCI `14C3:0717` as well, the AMD RZ717 flavour of the MT7925 on AM5 desktop boards (same firmware in Linux). Untested on that card so far.
- Tested on the author's laptop with MtkWlanNative 0.4.10 and a forced 25 s front-end delay: the native front end bound after 24.2 s, no fallback.

## 0.11.0 (2026-10-18)

- Core ops table v4 (`MtkCoreApi.h`): `set_mac` changes the station address after bring-up. The association ends first, the firmware station interface is removed and added again with the new address, and the manager rejoins the wanted network. macOS gives a Wi-Fi interface a private address per network; without this the station associated with the old address in the firmware and DHCP never saw a reply.
- `MtkMacAddr` and `MtkMacChanges` properties; `MtkMacChangeRefused` when the firmware rejects an address.
- `tools/mtk-report.sh`: one archive with everything a bug report needs, MAC addresses and SSIDs redacted.
- Ethernet front end unchanged. Tested with the native front end MtkWlanNative 0.4.1 on macOS 26.7.1 (25G241): macOS-driven join with a private address, DHCP lease on it.

## 0.10.0 (2026-10-17)

- Core ops table v3 (`MtkCoreApi.h`): `set_mode`, `get_bss`, `get_channels` and `tx_bytes`, so a native Wi-Fi front end can drive the core without a C++ link.
- Front-end selection: `MtkFrontEnd` in `Info.plist` picks the Ethernet front end (default) or a native front end built against the same ops table; if the chosen front end does not come up the core falls back to Ethernet (`MtkFrontEndFallback`). `OSBundleCompatibleVersion` is set so a separate front-end kext can link against the core.
- `mtkwifi status` reports the active front end and stands down (no join management) when the native front end owns the interface.
- Same behaviour as 0.9.0 on the Ethernet front end. Measured on the same open 5 GHz network: 192.7 Mb/s down, 134.6 Mb/s up (20 MB POST), 16 ms idle ping, 0 % loss, 15 dropped RX frames in the first minutes (0 in the 0.9.0 run; being watched).
- The release binary predates the last source edit of this version (`MtkNet.cpp`, `MtkFrontEndActive` property); see the README for both hashes.

## 0.9.0 (2026-10-17)

- The network name is no longer fixed in `Info.plist`. The driver scans from its 1 ms tick, keeps a cache of the networks it hears (with each one's security), and joins what it is asked to join.
- `tools/mtkwifi`: a command line tool that talks to the driver through an IOUserClient. Scan, list, join, disconnect, status, saved networks (stored as the derived PMK, never the passphrase), and an autojoin LaunchDaemon that joins the first saved network in range at boot.
- WPA2-PSK (CCMP): RSN element in the association request, the 4-way and group key handshakes in the driver, keys installed in the order Linux uses, host-side port gating, CCMP replay check after the block-ack reorder. Passes the host tests in `tests/` against a Python oracle. Not yet joined a real WPA2 network on air (see the README).
- Open-network fallback: when nothing is saved or asked for, the driver joins a network named in `MtkAutoOpenSsids` (exact match) 20 s after its first scan.
- The Ethernet front end starts before any association, so `en1` shows "no carrier" instead of an unknown link.
- Deauth during a join backs off and names a wrong key; legacy association retry on silence; a firmware watchdog stops the machines after three command timeouts in a row.
- Measured on the same open 5 GHz network as before: 190.8 Mb/s down, 174.8 Mb/s up, 16 ms idle ping.

## 0.8.0 (2026-10-16)

- The driver is split into a core (`MtkWlan`, IOService on the PCI device) and an Ethernet front end (`MtkWlanEth`, IOEthernetController) joined by a plain C ops table (`MtkCoreApi.h`), so a later native Wi-Fi front end can attach without a C++ link to the core.
- Native build on macOS with the Command Line Tools (`tools/build-mac.sh`).
- Same behaviour as 0.7.0: 195 Mb/s down, 97 Mb/s up in that boot (upload was a 20 MB POST, too short to settle).

## 0.7.0 (2026-10-15)

- Block ack in both directions: RX reorder (ported from mt76 `agg-rx.c`), TX ADDBA with the firmware building A-MPDUs.
- TX power: the firmware's country limit tables (CLC) and per-rate limits (SKU), sent as Linux mt76 does, with the TLV length fixes from Linux master.
- Station record TLVs in Linux order.
- 193 Mb/s down, 146 Mb/s up on an installed macOS 15.8.1.

## 0.6.1 (2026-10-15)

- Sampled TX status (rate, power, tries) and TX-free attempt counts in the registry.

## 0.6.0 (2026-10-15)

- HT/VHT association (80 MHz, 2 streams) with firmware rate control: 47.4 Mb/s down, 9.2 Mb/s up.

## 0.5.1 (2026-10-15)

- TX frames in flight capped at 32 and the output queue at 64: upload latency 1.2 s to 69 ms.

## 0.5.0 (2026-10-15)

- First working version: PCI attach, firmware download, scan, open-system join, Ethernet interface with DHCP. Safari and an Ookla speedtest from macOS Recovery: 19.4 Mb/s down, 9.6 Mb/s up.
