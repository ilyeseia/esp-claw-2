<!--
SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
SPDX-License-Identifier: Apache-2.0
-->

# cap_vpn — Tailscale gateway (Option C)

ESP32-S3 cannot run Tailscale natively (it needs the Go-based client and the
Tailscale control plane, far beyond the chip's resources). So ESP-Claw uses the
**gateway model**: the device stays an ordinary LAN client and a **Tailscale
subnet-router** running on a small always-on box on your LAN bridges it to your
tailnet. No WireGuard runs on the device — firmware cost is ~0.

```
   Anywhere on your tailnet                 Your LAN
  ┌───────────────────────┐        ┌──────────────────────────────┐
  │  laptop / phone        │        │  ┌────────────────────────┐  │
  │  searxng.tailXXXX.ts.net│◀──────▶│  │ Tailscale subnet-router │  │
  └───────────────────────┘  tailnet│  │ (Linux / RPi / OpenWrt) │  │
             ▲                        │  └───────────┬────────────┘  │
             │                        │              │ routes/DNS     │
             │                        │      ┌───────▼───────┐        │
             └── reachable via ───────┼─────▶│  ESP-Claw     │        │
                 the subnet-router    │      │  (ESP32-S3)   │        │
                                      │      └───────────────┘        │
                                      └──────────────────────────────┘
```

`cap_vpn` itself only stores the gateway/tailnet settings and exposes the
`vpn_status` agent tool, which resolves and TCP-probes a configured tailnet host
so the agent and the Web UI can confirm the route is up.

## 1. Gateway setup (on a Linux box / Raspberry Pi / OpenWrt on the LAN)

Install Tailscale and bring the node up as a **subnet router**. Replace
`192.168.1.0/24` with your actual LAN subnet.

```bash
curl -fsSL https://tailscale.com/install.sh | sh

# Let the LAN reach the tailnet AND let the tailnet reach the LAN:
sudo tailscale up \
  --advertise-routes=192.168.1.0/24 \
  --accept-routes \
  --snat-subnet-routes=true
```

Then, **in the Tailscale admin console → Machines → this node → Route settings**,
approve the advertised subnet route. Enable **MagicDNS** under DNS settings if you
want `*.ts.net` names.

- **To reach the ESP-Claw device from anywhere:** the advertised LAN route above
  is enough — any tailnet device can now hit the device's LAN IP.
- **To let the device reach tailnet services (e.g. your SearXNG):** add a static
  route on your LAN router so the tailnet CGNAT range `100.64.0.0/10` is forwarded
  to the gateway box's LAN IP, and make sure IP forwarding is enabled on the
  gateway (`net.ipv4.ip_forward=1`). The device then reaches tailnet hosts by IP.
  For `*.ts.net` names to resolve on the device, either point the LAN DHCP DNS at
  the gateway (running a resolver that knows MagicDNS) or use the tailnet IP / a
  local DNS entry as the test host.

> Tip: the simplest setup is to run the service you care about (SearXNG, MQTT
> broker, …) **on the gateway box itself or elsewhere on the same LAN**. Then the
> device reaches it over plain LAN and the tailnet is only used for *remote*
> access to the device — no device-side routing changes needed.

## 2. ESP-Claw configuration

Web UI → **VPN** tab (or the `/api/config` `vpn` group):

| Field           | Meaning                                                        | Example                     |
| --------------- | ------------------------------------------------------------- | --------------------------- |
| `vpn_enabled`   | Turn the gateway integration / diagnostics on                 | `true`                      |
| `vpn_gateway`   | The subnet-router's LAN IP (informational)                    | `192.168.1.10`              |
| `vpn_test_host` | A tailnet host `vpn_status` probes (name if MagicDNS, else IP) | `searxng.tailXXXX.ts.net`   |
| `vpn_test_port` | TCP port to probe                                             | `80`                        |

## 3. Checking status

Ask the agent to run **`vpn_status`**, or call it from a tool client. It returns:

```json
{
  "mode": "tailscale-gateway",
  "enabled": true,
  "gateway": "192.168.1.10",
  "test_host": "searxng.tailXXXX.ts.net",
  "test_port": 80,
  "dns_ok": true,
  "dns_ms": 12,
  "resolved_ip": "100.101.102.103",
  "reachable": true,
  "connect_ms": 34
}
```

- `dns_ok:false` → the tailnet route/MagicDNS is not reachable from the device
  (check the gateway route/DNS, or use an IP as the test host).
- `reachable:false` → DNS worked but the TCP connect failed (gateway/route down
  or the tailnet host is offline).

## Kconfig

`CONFIG_APP_CLAW_CAP_VPN` (default `y`) enables the capability. The tools are
always registered when enabled; probing only does anything once `vpn_enabled` is
set and a `vpn_test_host` is configured.
