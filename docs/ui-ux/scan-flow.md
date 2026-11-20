# Scan and capture on Flipper

This page describes the current beacon workflow. Phase T10 adds a capture-mode
choice on top of it; see [Capture modes](capture-modes.md).

## Workflow

1. Open **Scan area**. The Devboard listens for 2.4 GHz beacons and sends up to 16 discovered points to Flipper. Back cancels the scan. **Cancel failed** reports a receiver shutdown error. The previous list stays available when a rescan fails.
2. Navigate with Up and Down. Press OK to select a network. Select one to five points, then press Right on **Start**. A scan by itself writes no capture.
3. During a round, the screen shows the current network, capture count, and Stop. **Saved** in the **Beacons** list means the Devboard verified the beacon PCAP and receipt. **Not seen** means the point finished without a matching beacon. An SD or radio error remains a separate result.
4. A finished round returns to the same list. Saved rows are skipped by later rounds. Select a **Not seen** row to retry it. After an ordinary finish, focus the first missed row. When Stop is pressed, keep the focus and the selection of unvisited rows. A save already in progress can finish. A completed miss remains **Not seen** if the deadline prevents the next visit.
5. Select other networks and press Start again. Rescan keeps the selection and outcomes of points that still appear, matched by device address. Lists and per-point progress are held during the app session.

The recent Devboard presents this flow as its default. Flipper uses the network name in the scan list and progress view. Distinct access points with the same name receive numbered labels. Hidden names receive a fallback label. BSSIDs and channels are internal to the scan, verified PCAPs, and session journals. Existing manual profiles remain readable for older board firmware.

**Capture mode** chooses the mode of the next round; see [Capture modes](capture-modes.md). **Flash board hardware**, **Test board**, and **About** are empty screens in this change. Back returns to the menu.

## Mockup

Open [Scan and capture on Flipper](scan-prototype.html) in a browser. Choose
TELEPORT and Studio, stop after the first capture, then add Guest. Use the
simulator's **Сымитировать «не найдена»** control to try a miss followed
by a retry. The screen is 128 × 64 and supports mouse and keyboard input.
The demo list uses the device labels `Beacons` and `Saved`.

## Interfaces and verification

The Devboard advertises the optional scan capability over UART. Both devices must support it to use five-point rounds. The radio stays in receive-only mode. It listens during a bounded scan or a capture, one at a time. The Flipper requests scan results one at a time and publishes the list only after the last complete reply.

Run `make test` for the protocol and native Flipper emulator and `make embedded` for both binaries. Firmware verification on physical devices is a separate bench check.

## Review

- **Crowded scans.** Filling the list in reception order could hide points
  on later channels. When the list is full, a stronger beacon now replaces
  the weakest candidate. A host regression fills the list on one channel
  and checks that a stronger candidate on a later channel is retained.
- **Retry and Stop.** A missed point remains available for explicit retry.
  The emulator verifies that Stop preserves a captured point and leaves
  an interrupted point selected, that a rescan keeps its matching results,
  and that a successful retry replaces **Not seen** with **Saved**.
- **Compatibility and capacity.** The old profile file format stays limited
  to three targets. The new code negotiates the five-point capacity over
  UART. Protocol and journal tests exercise the fifth result and filename.
- **Physical acceptance.** Check the scan yield, text layout, and latency
  on Flipper and the ESP32-S2 with an approved set of test points. The
  emulator models the receiver and storage. The browser mockup uses demo
  network names.

## Status labels

Use **Beacons** as the list heading and **Saved** for a point with a
verified beacon PCAP and receipt. Keep **Not seen** for a finished visit
without a matching beacon and **SD error** or **Radio error** for failures.
In the Flipper secondary font, **Saved** takes 25 pixels, **Captured** takes
38, and **Beacon saved** takes 56. The shorter label gives the network name
31 more pixels than **Beacon saved**.

Retry applies to the same access point. If a hotspot reappears with a
changed address, scan again and explicitly select the new row. A matching
SSID alone is insufficient to carry previous outcomes or selection over.
