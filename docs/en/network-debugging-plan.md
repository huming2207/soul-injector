# Debugging over the network: plan

Status: proposal, nothing implemented yet. WiFi comes first, cellular later.
The four decisions at the end are open.

## What exists today

- **Firmware WebSocket transport.** `components/sidp_client` has
  `websocket_transport`, with the device as the WebSocket client. Each binary
  WebSocket message carries one SIDP frame, automatic reconnect is off, and the
  connection manager calls `reconnect()` after target cleanup
  (`docs/integration.md`). Nothing in `main` uses it, so it has never run on
  hardware.
- **`main/comm/wifi_manager`.** Starts station mode and retries, but has no
  credentials (TODO) and nothing calls it.
- **`main/comm/modem_manager`.** The Quectel PPP link, with light sleep
  working. Nothing calls it either.
- **`sidp_service`.** Only knows the USB port: debug frames, power data and
  power events are all sent to `cdc_slip_transport` directly.
- **sidp_agent.** `Session<T: Transport>` works with any transport, but
  `UsbCdc` is the only one. The desktop's `Connection` holds a serial port
  path.

## Proposed topology: the device connects out to a relay

The device is always the WebSocket client and connects to a relay. The agent
also connects to the relay and names the device by serial number.

- **Why:** a cellular modem sits behind carrier NAT, so nothing can connect in
  to the device. The client transport already exists. WiFi and cellular then
  share one code path, and the cellular phase is only a different network link.
- **On a LAN:** `sidp-agent relay` runs on the PC, and the desktop app embeds
  one. For cellular, the same relay runs on a server.
- **Connection rules stay the same.** One WebSocket connection is one SIDP
  connection. When the agent leaves, the relay closes the device's WebSocket;
  the device cleans up the target as it does when USB DTR drops, then
  reconnects. The wire protocol does not change.
- **Alternative:** the device listens and the agent connects to, for example,
  `ws://si-xxxx.local`. Simpler for WiFi alone, but it does not work for
  cellular and would be replaced.

## Security from the first version

Over the network, SIDP gives full SWD access and can push jobs, so:

- Encrypted WebSocket (`wss`) only. The device checks the relay's
  certificate, which is copied onto it over USB during setup.
- One token per device, and tokens for agents connecting to the relay.
- Networking is off until it is set up over USB.

## Phase 1: firmware (soul-injector, sidp_client)

1. **Make `sidp_service` transport-independent.** It serves the active
   connection's `transport_intf`; debug frames and power output go to that
   connection.
2. **One connection at a time.** Whichever link opens first wins. The other
   link answers every request with `STATUS_BUSY` instead of timing out.
3. **Network settings in NVS, with new management opcodes (0x0140 and up):**
   - set/get: WiFi SSID, password (write-only), relay URL, relay certificate,
     token, enabled;
   - status: WiFi state, IP address, RSSI, relay state.
4. **A network task** that loops: connect WiFi, connect the WebSocket, serve
   the session, clean up the target, reconnect with backoff. Replaces the
   credentials TODO in `wifi_manager`.
5. **Power management.** While a network session is open, hold a no-light-sleep
   lock and turn WiFi power save off; otherwise every request is slower.
6. **Don't let expendable frames end the session.** A log-queue frame (power
   data) that cannot be sent within 20 ms closes the whole session
   (`sidp_transport_queue.cpp`, failed `deliver_tx_frame`). Harmless on USB,
   but WiFi delays will trigger it. Such frames should be dropped instead.
7. **Show WiFi and relay status on the LCD.**

## Phase 2: agent (sidp_agent)

1. **A WebSocket `Transport`** for the existing session. GDB, power, jobs and
   the production log then work over it unchanged.
2. **`sidp-agent relay`:**
   - creates a self-signed certificate on first run and checks tokens;
   - pairs an agent with a device by serial number and forwards frames
     unchanged;
   - one agent per device; lists the devices online.
3. **`--port wss://host:7420/SERIAL`** in every command: `gdb`, `debug`,
   `power record`, `job`, `log`.
4. **Setup commands.** `sidp-agent network setup --port /dev/ttyACM0 --ssid ...
   --relay wss://...` copies the certificate and token onto the device; the
   WiFi password is typed at a prompt. Also `network status` and
   `network off`.
5. **Desktop.** A connection is USB or network; the device list shows devices
   online at the relay; the relay is built in; a network setup page.

## Phase 3: tests on Rev 8 with the STM32WLE5 board

- **GDB over WiFi:** attach, `step`, `next`, `continue`, breakpoints, timed
  against USB.
- **Power profiler interference (biggest risk).** The sampler runs on core 0
  at priority 9; the WiFi task is pinned to core 0 at a higher priority and
  lwIP has no core affinity. Record 60 s at 588 µs with WiFi busy and look for
  missed conversions. If WiFi causes gaps, change core pinning or priorities.
- **Power data over WiFi:** gaps and dropped frames.
- **Link loss:** turn the access point off mid-session. The target must be
  cleaned up, the device must reconnect, and the agent must report an error
  rather than hang.
- **USB and WiFi together:** the second link gets `STATUS_BUSY`.
- **Speed:** push a firmware image over WiFi.

## Phase 4: cellular (later)

- Use `modem_manager` as a second network link, WiFi first with cellular as a
  fallback or a setting. Move the APN into the network settings; it is
  hard-coded as `quectel.st.std`.
- Run the relay on a public server.
- **Delay.** An LTE-M round trip is roughly 50–300 ms and a GDB step takes
  several requests, so stepping will feel slow. `OP_READ_MEMORY_VECTOR` is
  defined but has no device handler; implementing it would cut the number of
  requests.
- **Power data.** Revisit the deferred delta encoding, lower sample rates and
  data cost.
- **Keepalive.** The WebSocket ping interval must be shorter than the
  carrier's NAT timeout, which has to be weighed against the modem's power
  saving.

## Open decisions

1. **Topology:** device connects out to a relay (recommended), or the device
   listens on WiFi.
2. **LAN security:** `wss` and tokens from the first version (recommended), or
   plain `ws` first with security added before cellular.
3. **USB and network both connecting:** first one wins and the other gets
   `STATUS_BUSY` (recommended), or USB always takes over.
4. **Where the cellular relay is hosted:** can wait until phase 4.

Implementation would start with phase 1 step 1, because everything else
depends on it.
