# Debugging over the network: plan

Status: agreed plan. Phase 0 and most of phase 1 are implemented and wait for
tests on Rev 8 (see "Phase 0: what was built" and "Phase 1: what was built").
WiFi comes first, cellular later.
The decisions made so far, and the ones still open, are listed at the end.

Revised 2026-11-13 after re-reading the firmware and sidp_agent, and again
after discussion. Decided so far:

- **Two modes, one device code path.** On a local network the device connects
  straight to the agent on the user's computer. For cellular the device
  connects to a relay server, and a remote user's agent connects to the same
  relay.
- **USB has priority** over the network.
- **Local mode is plain `ws` with a device password** that never crosses the
  network. The LAN is assumed free of anyone who can alter traffic.
- **The password login is ESP-IDF's protocomm Security 2 handshake** (SRP6a),
  in both modes, not a design of our own.
- **Relay mode is `wss`.** The relay is a standalone program.
- **No eFuses are burned for now.** Secure boot, flash encryption and a
  hardware-bound identity key wait until ESP32-S31 chips are easier to get.
- **One long-lived connection** from the device, which reconnects at once
  when it drops.

## What exists today

- **Firmware WebSocket transport.** `components/sidp_client` has
  `websocket_transport`, with the device as the WebSocket client. Each binary
  WebSocket message carries one SIDP frame, automatic reconnect is off, and the
  connection manager calls `reconnect()` after target cleanup
  (`components/sidp_client/docs/integration.md`). Text messages are ignored.
  Nothing in `main` uses it, so it has never run on hardware.
- **`main/comm/wifi_manager`.** Starts station mode and retries, but has no
  credentials (TODO) and nothing calls it. WiFi has never run on the ESP32-S31
  in this project.
- **`main/comm/modem_manager`.** The Quectel PPP link, with light sleep
  working. Nothing calls it either.
- **`sidp_service`.** Only knows the USB port: its task, its responses, the
  debug frames, power data and power events all use `cdc_slip_transport`
  directly.
- **sidp_agent.** `Session<T: Transport>` works with any transport, but
  `UsbCdc` is the only one. The library is blocking, with no async runtime.
  The desktop app names `Session<UsbCdc>` directly and its `Connection` holds
  a serial port path. It opens a new connection for each operation, and two or
  three during one debug run.

## Topology: the device always connects out

The device is always the WebSocket client. What it connects to depends on the
mode, and it cannot tell the two apart:

| | Local mode (WiFi, phase 1 to 3) | Relay mode (cellular, phase 4) |
|---|---|---|
| Device connects to | the agent on the user's computer | a relay server |
| User's agent | is the server; speaks SIDP itself | connects out to the relay |
| Who forwards frames | nobody | the relay, unchanged |
| Device is found by | mDNS on the LAN | the relay's URL |

- **Why the device connects out in both modes:** a cellular modem sits behind
  carrier NAT, so nothing can connect in to it. The client transport already
  exists. One code path on the device then serves both modes, and relay mode
  adds only a server and a different network link.
- **The SIDP wire protocol does not change.**
- **Cost in local mode:** the computer must accept an incoming connection, so
  its firewall has to allow the agent's port. The desktop app should say so
  when no device shows up.

### Local mode: who listens

Something on the computer has to be listening when the device connects.

- **Desktop app:** it listens while it runs. Devices on the LAN connect to it
  and appear in the device list.
- **Command line:** `sidp-agent serve` is the same listener without a window.
  It stays running and holds the device connections; the other commands
  (`gdb`, `debug`, `job`, `power record`, `log`) reach a device through it
  with `--port ws://127.0.0.1:7420/SERIAL`. Without it, every command would
  have to wait for the device's next connection attempt.
- Only one listener can own the port, so the desktop app and `serve` do not
  run together; the app's listener serves the command line too.

`serve` forwarding loopback commands to a device is the same job the relay
does in relay mode, so the relay is this code on a public server with user
login added.

### The device stays connected; the server marks sessions

The device keeps one WebSocket open while networking is on. The server (the
agent in local mode, the relay in relay mode) marks the start and end of each
session with text WebSocket messages, which do the job DTR does on USB:

| Message | Direction | Meaning |
|---------|-----------|---------|
| `open` | server to device | A user wants the device. The device starts a new SIDP session. |
| `ready` | device to server | The session is accepted; SIDP frames may flow, starting with the login (Security, part A). |
| `busy` | device to server | Refused: USB is using the device. |
| `close` | server to device | The user left. The device ends the session and cleans up the target. |
| `closed` | device to server | The session is over and the target is cleaned up. Also sent unasked when USB takes over. |

- **Why not close the device's WebSocket after each session:**
  - The device could not tell "connected, idle" from "someone is using me",
    which the USB priority rule needs.
  - Every session would cost a new connection and login, and in relay mode a
    TLS handshake. The desktop app opens a session per operation, two or
    three in one debug run.
- **`ready` replaces the settle delay.** On USB the agent waits a fixed 250 ms
  and hopes the device accepted.
- **`closed` proves cleanup,** which USB cannot do. If the device never sends
  it, the next user is told instead of being let in.
- **Loss of the device's WebSocket** ends the session as before: the device
  cleans up the target and the user gets an error. See the next section.
- **In the transport,** `open` and `close` are handled the way the CDC
  transport handles a DTR change.

### When the wireless link drops

- **Short losses do not drop anything.** TCP retransmits, so a second or two
  without radio shows up as a slow request. The connection only ends when the
  link stays dead past the timeout (about 20 s), or the device gets a new
  address.
- **The device reconnects at once.** First retry immediately, to the server
  it was last connected to; then 1, 2, 4 s and so on, up to 30 s.
- **A session does not survive a dropped connection.** SIDP never replays a
  request, because the device may or may not have carried it out. The target
  is cleaned up and the user starts again; with *keep serving*, the agent
  attaches again by itself as soon as the device is back.
- Resuming a session across a reconnect would need a protocol change. It is
  left out until tests show real links need it.

### USB has priority

- USB takes over from a network session. The device ends the network session,
  cleans up the target, sends `closed`, and then serves USB.
- While USB is in use, `open` is answered with `busy`.
- **USB takes over on its first valid SIDP request, not on DTR alone.**
  Plugging the device into a computer is enough for some software to open the
  port (ModemManager on Linux probes new serial ports), and that must not end
  someone's remote session.
- An idle network connection never blocks USB, and a GDB server forgotten on
  another computer cannot lock the device out.

## Security

Over the network, SIDP gives full SWD access and can push jobs. Three things
have to hold:

| | Requirement | Applies to |
|---|---|---|
| A | Only someone who knows the device's password can use it. | Both modes |
| B | Nobody between device, relay and user can read or alter traffic. | Relay mode |
| C | The device cannot be replaced or modified without the owner noticing. | Relay mode |

Common to both modes: networking is off until it is set up over USB, and
**network settings and the password can be changed over USB only.** Otherwise
anyone who reaches the device over the network could point it at another
server.

### A. The device password (both modes)

The login is the session handshake of ESP-IDF's protocomm component, run over
our WebSocket instead of protocomm's own transports. **Security 2 (SRP6a) is
used in both modes.** The device calls `protocomm_security2`'s
`security_req_handler` with the bytes it receives; the agent implements the
client side.

- **Setup, over USB.** The user chooses a password, or lets the agent
  generate one. The agent computes the SRP salt and verifier and stores those
  on the device; the password itself is never stored there.
- **Login, at every `open`,** as the first SIDP requests: `LOGIN` (0x0150)
  carries protocomm's `SessionData` message, twice. Until it succeeds, a
  network connection answers every other request with `STATUS_UNAUTHORIZED`
  (31). Running it inside SIDP keeps SIDP's framing, CRC and timeouts, and the
  transport stays simple. The password does not cross the network, and both
  sides prove they know it. A wrong password costs one second and restarts the
  handshake.
- **No pairing code is pre-loaded at the factory.** USB is the trusted
  channel for setup, so nothing has to be printed on the device or its box.
  If setup without a cable is wanted later, a printed code can serve as the
  device's first password without changing the login.
- **Anyone holding the device can change its password over USB,** or point
  it at another server. They cannot do it unnoticed: the owner's next login
  fails, and in relay mode the device stops appearing at the owner's relay.
  The agent must say so plainly ("this device's password was changed, or it
  is not your device") instead of reporting a wrong password.
- **Only the handshake is used, not protocomm's encryption.** It keeps one
  cipher state for a strict request-then-response exchange. SIDP
  sends events and power data at any time, so the two ends would fall out of
  step. Local mode traffic therefore stays in the clear, as decided.
- **The device finds agents by mDNS** and may see several in one office. It
  connects to each; only an agent whose user has typed this device's password
  can open a session.
- **In relay mode the same login runs end to end** between the user's agent
  and the device, with the relay only forwarding it. A relay account alone is
  not enough to use a device, and neither is breaking into the relay.

- **Why Security 2 and not Security 1:**

  | | Security 1 | Security 2 (chosen) |
  |---|---|---|
  | Method | Curve25519 key exchange, mixed with the password's hash | SRP6a, a password-authenticated key exchange |
  | Device stores | the password, or a key derived from it | a salt and verifier; nobody can log in with these |
  | Someone listening | learns nothing to test passwords against | the same |
  | Someone who can take part in a login (a hostile relay) | can then test passwords offline | gets one guess per login |
  | Status in ESP-IDF 6.1 | off by default; "Security 2 is recommended for new designs" | on by default |

  Relay mode needs Security 2, and using it in local mode too means one
  scheme and one client.
- **The client in sidp_agent** is SRP6a with a 3072-bit group and SHA-512,
  and must match Espressif's exactly. Espressif's `esp_prov` Python tool is
  the reference to test it against. This is the riskiest part of phase 2, so
  it is built and tested first.
- **What this does not cover.** In local mode, traffic after login is neither
  encrypted nor authenticated. Someone able to read the LAN sees firmware
  images, and someone able to alter traffic could take over a session. This
  is the stated assumption for local mode.

### B. Relay mode: encryption

- **Device to relay and user to relay are both `wss`.**
- **The device trusts one CA certificate, copied onto it at setup,** not the
  200 authorities in the built-in bundle. It checks the relay's host name.
- **The user's agent** checks the relay's certificate as usual.
- **Users log in to the relay** with a token, so that strangers cannot list
  or reach devices at all. The password login (A) still decides who may use
  each device.
- **Limit:** the relay decrypts and re-encrypts, so it sees SIDP in the clear,
  firmware images included, and a broken-into relay could alter a session
  after login. Closing that needs encryption between the user's agent and the
  device themselves, which is not planned for the first version.

### C. Relay mode: a device that cannot be replaced or modified

The case to defend against is a contract manufacturer who holds the device
and wants the firmware images pushed to it, or wants to program something
else.

Full protection needs eFuses, which are permanent, and ESP32-S31 chips are
scarce. So this is done in two stages, with the same protocol in both.

**Stage 1, with relay mode: an identity key in software. No eFuse.**

- The device creates a key pair on first boot and keeps it in NVS.
- **Enrollment, over USB, before the device is shipped:** the agent reads the
  device's public key and stores it with the serial number.
- **At every session,** the user's agent sends a nonce and the device signs it
  together with its serial number and firmware version. The agent checks the
  signature against the enrolled key. The relay can run the same check when a
  device connects.
- The messages carry the key type, so stage 2 changes no protocol.
- **What it catches:** a device swapped for another one, and a device that
  was erased and reflashed.
- **What it does not catch:** someone who reads the flash chip gets the key,
  the device password data, the WiFi password, the job and the firmware
  images. They can then build a copy that passes the check, or run modified
  firmware on the real device. Against a manufacturer who is determined and
  competent, stage 1 is not enough, and the agent should show "identity not
  hardware-protected" for such a device.

**Stage 2, later: bind it to the hardware. Burns eFuses.**

- **Identity key in the Digital Signature peripheral** (RSA; the ESP32-S31
  also has the HMAC peripheral it depends on). Only the hardware can use the
  key; firmware cannot read it and a flash dump does not contain it in usable
  form.
- **Secure Boot v2,** so only signed firmware runs. Without it, modified
  firmware on the genuine device could still use the identity key.
- **Flash encryption,** so the flash chip cannot be read.
- Release mode also turns off JTAG and the ROM's plain download mode.
- These change how devices are built, flashed and updated, so they need their
  own plan, and one chip to try it on, before any device is fused.

**Until stage 2, reduce what a stolen device gives away:**

- Send a device to an untrusted site with only the job it needs, and a
  password used nowhere else.
- Turn on the target's readout protection in the job.
- Push firmware images shortly before they are used, not weeks ahead.

**Not covered by either stage:**

- **Hardware attacks.** Wires on the SWD lines read the firmware as it is
  programmed, whatever the Soul Injector does. A sealed enclosure only makes
  this visible.
- **The targets themselves.** The manufacturer holds every programmed board.
- A device that is unplugged or kept offline. The relay can only show when it
  was last seen.

### What each phase needs

| | Local mode (phases 1 to 3) | Relay mode (phase 4) | Later (phase 5) |
|---|---|---|---|
| A. Password login (Security 2) | yes | yes, end to end | |
| B. `wss`, pinned CA, relay login | no | yes | |
| C. Identity key and enrollment | no | yes, in software | moved into hardware |
| C. Secure boot and flash encryption | no | no | yes |

## Phase 0: spike

Plain `ws`, network settings stored over USB, and a bare WebSocket transport
in the agent that listens for the device. No security. It answers:

1. **Does WiFi work on this board at all** with 320 MHz, PSRAM and automatic
   light sleep?
2. **Does WiFi disturb the power sampler?** The sampler runs on core 0 at
   priority 9; the WiFi task is pinned to core 0 at a higher priority and lwIP
   has no core affinity. Record 60 s at 588 µs with WiFi busy and count missed
   conversions. If there are gaps, try core pinning and priorities now.
3. **How long is one request?** Time `step` and a 4 KiB memory read against
   USB.

If item 2 cannot be fixed, power measurement over WiFi is dropped from the
plan and the rest goes ahead.

### Phase 0: what was built

Firmware:

- Network settings in NVS, namespace `si.cfg`: enabled, SSID, WiFi password,
  server URL. New management opcodes `NET_CONFIG_GET` (0x0140),
  `NET_CONFIG_SET` (0x0141, refused unless the request came over USB) and
  `NET_STATUS` (0x0142). Settings are used from the next boot.
- `sidp_service` serves USB or the WebSocket, whichever connects first, with
  one WebSocket connection per SIDP connection. After each connection, and
  after a failed attempt, it connects again: at once, then every 0.5 s up to
  3 s. During a WiFi session it holds a no-light-sleep lock and turns modem
  power save off.
- WebSocket log frames (power data) get the 5 s send timeout instead of
  20 ms (phase 1 item 8, first half).
- `CONFIG_SI_NETWORK_ENABLE`, on for Rev 8 only. With WiFi the app is about
  2.2 MB, so Rev 8 uses `partitions_rev8.csv`: two 4 MB app slots for A/B
  updates, 256 KB `nvs`, 1 MB `log` and the remaining 6.7 MB for `data`.
- Rev 8 turns off `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`. With it on,
  linking WiFi fails in ESP-IDF 6.1 (and master): on the ESP32-S31, PSRAM
  shares the bus with flash rodata, and the `.ext_ram.dummy` section cannot
  move back past WiFi's NOLOAD log strings. Nothing in the project used PSRAM
  `.bss`.

Agent:

- `transport::WebSocket`: listens, accepts one device, one frame per binary
  message, Nagle off. Every command accepts `--port ws://ADDRESS:PORT`.
- `sidp-agent network set | show | off`.
- Desktop: a *Network…* sheet that stores the settings over USB.

Checked without hardware: the agent served GDB over WebSocket to the real C++
`sidp_session` (attach, memory reads, detach, close seen by the device).
Still to measure on Rev 8: phase 0 items 1 to 3 above.

## Phase 1: firmware (soul-injector, sidp_client)

1. **Make `sidp_service` transport-independent.** It serves the active
   connection's `transport_intf`; responses, debug frames and power output go
   to that connection. The service has to watch both links.
2. **One link in use at a time, USB first,** as described under "USB has
   priority". The service has to read both links to do this.
3. **Network settings in NVS, with new management opcodes (0x0140 and up):**
   - set/get, refused unless the request came over USB: WiFi SSID, WiFi
     password (write-only), device password data (write-only), server URL
     or "find on the LAN", enabled;
   - status: WiFi state, IP address, RSSI, server connection and last error.
4. **A network task** that loops: connect WiFi, find the agent, connect the
   WebSocket, serve sessions, reconnect with backoff. Replaces the credentials
   TODO in `wifi_manager`.
5. **Finding the agent on a LAN.** A laptop's address changes, so a stored
   `wss://192.168.1.20:7420` stops working the next day. The agent announces
   itself by mDNS (`_sidp._tcp`) and the device looks it up; the password
   login decides whether it may use the device. A fixed URL is for relay
   mode.
6. **Add the session messages** (`open`, `ready`, `busy`, `close`, `closed`)
   to `websocket_transport`, and the Security 2 login. Reconnect as
   described under "When the wireless link drops".
7. **Detect a dead link quickly.** The WebSocket client's defaults are a ping
   every 10 s and 120 s without a pong before it gives up, so a target could
   stay claimed for two minutes. Set the pong timeout to about 20 s and turn
   on TCP keepalive.
8. **Expendable frames (power data) on WebSocket.**
   - Today a log-queue frame that is not sent within 20 ms closes the session
     (`deliver_tx_frame`). Harmless on USB, but WiFi delays will trigger it.
   - It cannot simply be dropped: `esp_websocket_client` aborts the connection
     when a send times out, and a half-sent frame would corrupt the stream
     anyway.
   - So on WebSocket a started send gets the normal 5 s timeout. Frames are
     dropped only in the queue, before sending, as they already are when it
     is full.
   - The log queue is 4 KiB, about half a second of power data at the fastest
     rate. Make it larger for the network link.
   - Turn on `CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK`, so a send does not wait
     for the client's receive loop.
9. **TCP settings.**
   - `sdkconfig.defaults` sets the lwIP send buffer and receive window to
     5744 bytes, smaller than one 8 KiB SIDP frame. Raise both for WiFi.
   - Check that Nagle's algorithm is off on the device's socket. With it on,
     a small response followed by a small event can wait for the peer's
     delayed ACK.
10. **Power management.**
    - While an agent session is open, hold a no-light-sleep lock and turn WiFi
      power save off; otherwise every request is slower.
    - While idle, stay connected with WiFi power save on.
11. **Show WiFi and connection status on the LCD.**

### Phase 1: what was built

Firmware:

- Item 2: USB has priority. During a network connection, USB's first request
  ends it: the device cleans up the target, sends `closed`, and serves USB.
  An `open` during a USB connection gets `busy`. While nothing is connected,
  opening the USB port still starts a USB connection at once, as before, so a
  serial terminal left open on the port makes the network get `busy`.
- Items 3 and 6: the device password (SRP salt and verifier) is part of the
  network settings, write-only, and networking cannot be enabled without
  one. The session messages are `open`, `ready`, `busy`, `close` and `closed`;
  the login is the `LOGIN` request described above, using ESP-IDF's
  `protocomm_security2` handler.
- Item 8: the WebSocket log queue is 32 KiB (USB keeps 4 KiB).
  `CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK` is not turned on: the client
  releases its lock while it waits to read, so sends do not wait for it.
- Item 9: Nagle is off on the device's socket. `esp_transport` never sets
  `TCP_NODELAY`, and the WebSocket layer writes each frame's header and
  payload separately, so every response would have waited for the agent's
  delayed ACK (up to 40 ms on Linux). The service builds its own TCP and
  WebSocket transport so the socket can be reached. Rev 8's lwIP send buffer
  and receive window are 16 KiB.
- The device stays connected to the server between connections and only
  reconnects when the WebSocket drops.

Agent: `transport::WebSocket` sends `open` and waits for `ready` (a clear
error for `busy`), ends with `close` and waits for `closed`, and reports
`closed` sent on its own. `login` is the SRP6a client; every `ws://` command
logs in (`--password`, or `SIDP_PASSWORD`, or asked). `network set` stores the
device password. The desktop's network sheet has a device password field.

Checked without hardware:

- The SRP6a client matches Espressif's reference client (`srp6a.py`) on test
  vectors, and logs in to ESP-IDF's own `security2.c` and `esp_srp.c`, built
  for the host, in 270 of 270 rounds with the right password and is refused
  in 30 of 30 with a wrong one.
- `sidp-agent gdb --port ws://...` against the real C++ `sidp_session` with
  that handler answering `LOGIN`: login, attach, memory reads, detach,
  `close` and `closed`; a wrong password is refused with a clear message.

Not done yet:

- Item 5, finding the agent by mDNS. It needs the agent to announce itself,
  which is phase 2, so both halves go together; until then the device uses
  the stored server URL.
- Item 11, WiFi and connection status on the LCD.

## Phase 2: agent (sidp_agent), local mode

1. **A WebSocket `Transport`** for the existing session. GDB, power, jobs and
   the production log then work over it unchanged.
   - Blocking, like the rest of the library (`tungstenite`), with Nagle off.
     TLS (`rustls`) is added in phase 4.
   - Behind a Cargo feature, so the library can still be built with only its
     present dependencies.
2. **The Security 2 client** (SRP6a), tested against a device and against
   `esp_prov`'s implementation before the rest is built on it.
3. **The listener** (library code, used by the desktop app and by
   `sidp-agent serve`):
   - plain `ws`; runs the password login for each session, with the password
     the user typed for that device;
   - holds the connected devices, by serial number, and lists them;
   - opens and closes sessions with the messages above; one user per device,
     and a second is told the device is in use;
   - pings devices and drops one that stops answering;
   - announces itself by mDNS;
   - accepts loopback clients, so command-line tools can use a device it
     holds.
4. **`--port ws://127.0.0.1:7420/SERIAL`** in every command: `gdb`, `debug`,
   `power record`, `job`, `log`.
5. **Setup commands.** `sidp-agent network setup --port /dev/ttyACM0 --ssid ...`
   stores the WiFi settings and the device password data over USB; both
   passwords are typed at a prompt. Also `network status` and `network off`.
6. **Desktop.**
   - The worker's session type stops naming `UsbCdc`; a connection is USB or
     network.
   - The listener is built in, and the device list shows the devices
     connected to it.
   - A failed login is explained: wrong password typed, or the device's
     password was changed.
   - A network setup page, which also explains the firewall. The app
     remembers device passwords in the operating system's keychain.

## Phase 3: tests on Rev 8 with the STM32WLE5 board

- **GDB over WiFi:** attach, `step`, `next`, `continue`, breakpoints, timed
  against USB.
- **Power sampler interference:** repeat the phase 0 measurement with the real
  code.
- **Power data over WiFi:** gaps and dropped frames, at each sample rate.
- **Link loss:** turn the access point off mid-session, and separately kill
  the agent. The target must be cleaned up within the timeout, the device
  must reconnect, and the agent must report an error rather than hang.
- **Several sessions in a row** on one device connection: `ready` and
  `closed` arrive, and a target left running by one session is found halted or
  released by the next, as on USB.
- **USB and WiFi together:** a USB request takes over a network session and
  the network user gets a clear error; `open` during USB use gets `busy`;
  opening the USB port without sending a request changes nothing.
- **Password changed over USB by someone else:** the owner's agent reports
  it as a changed password or a different device.
- **Wrong password** is refused by the device and by the agent, repeated
  attempts are slowed down, and a recorded login cannot be replayed.
- **Two agents on the LAN:** the device ends up usable only from the one that
  has its password.
- **Reconnect:** after the access point comes back, time until the device is
  usable again.
- **Settings over the network** are refused.
- **Speed:** push a firmware image over WiFi.

## Phase 4: cellular, relay mode (later)

- **The relay server:** the phase 2 listener on a public server, with `wss`,
  user login, and forwarding to remote agents instead of loopback ones. The
  remote user runs `--port wss://relay.example/SERIAL`.
- **Security B, and C stage 1:** the pinned CA, relay login, and the software
  identity key with enrollment. Measure the TLS handshake on the device here.
- Use `modem_manager` as a second network link, WiFi first with cellular as a
  fallback or a setting. Move the APN into the network settings; it is
  hard-coded as `quectel.st.std`. The WebSocket client can be bound to one
  network interface.
- **The relay is standalone:** `sidp-agent relay`, which anyone can run on a
  server they control.
  - Its user list is a file of tokens. The check sits behind one interface,
    so an account system can replace the file later without touching the
    device or the protocol.
  - It forwards frames in real time and keeps no data, so it needs no
    database.
  - Because the password login and the identity check run end to end, the
    relay decides only who can reach a device, never who can use it.
- **Delay.** An LTE-M round trip is roughly 50–300 ms and a GDB step takes
  several requests, so stepping will feel slow. `OP_READ_MEMORY_VECTOR` is
  defined but has no device handler; implementing it would cut the number of
  requests.
- **Power data.** Revisit the deferred delta encoding, lower sample rates and
  data cost.
- **Keepalive.** The WebSocket ping interval must be shorter than the
  carrier's NAT timeout, which has to be weighed against the modem's power
  saving and data cost.

## Phase 5: hardware-bound security (when chips allow)

Security C stage 2: the identity key in the Digital Signature peripheral,
Secure Boot v2 and flash encryption. It needs its own plan first, covering
signing keys, factory flashing, updates, and what happens to devices already
in use.

## Decisions

Made:

- **Topology:** the device connects out; to the local agent on WiFi, to a
  relay server on cellular.
- **USB and network together:** USB has priority.
- **The device's connection:** long-lived, with session messages, and an
  immediate reconnect when it drops.
- **Local mode security:** plain `ws` and a password login that keeps the
  password off the network.
- **Password login:** protocomm Security 2 (SRP6a) in both modes, handshake
  only.
- **Relay mode security:** `wss`, end-to-end password login, and a device
  identity key, in software for now.
- **Pairing code:** none pre-loaded; setup is over USB.
- **eFuses:** none burned for now; phase 5.
- **The relay:** standalone.

Nothing is open. Hosting and user accounts beyond the relay's token file
are for later.

Implementation would start with phase 0, then phase 1 step 1, because
everything else depends on it.
