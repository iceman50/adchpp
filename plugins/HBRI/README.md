<!-- Copyright (C) 2026 iceman50. Licensed under GPL-3.0-or-later. -->

# HBRI plugin

This plugin implements the hub side of the unofficial HBRI hybrid IPv4/IPv6
reachability extension. A dual-stack client logs in immediately over its
primary address family. The hub strips the unverified secondary address and
sends an `ITCP` challenge; the client opens a short-lived connection over the
other family and responds with `HTCP`. Only the address observed on that
connection is then published in the client's INF.

The implementation uses single-use, 192-bit cryptographically random tokens
with a ten-second lifetime. It remembers the secondary UDP port until
validation, supports post-login revalidation, rejects same-family validation
attempts, and never blocks login while validation is in progress. A client may
use the unspecified `I6::` or `I40.0.0.0` value to request validation; these
values are intent markers and are never published as client addresses.

## Configuration

Configure `HBRI.xml` with the shared listening port and set `Enabled="1"`.
Validation addresses may be configured there as IP literals or family-specific
DNS names. DNS names are resolved once during startup and only numeric,
correct-family endpoints are sent in `ITCP`.

The preferred listener form keeps the bind and public addresses together:

```xml
<Server Port="2780"
  BindAddress4="0.0.0.0" BindAddress6="::"
  HubAddress4="192.0.2.10" HubAddress6="2001:db8::10"/>
```

When `Address4` or `Address6` is empty in `HBRI.xml`, the corresponding
`HubAddress4` or `HubAddress6` value is used. Existing configurations with two
separate `BindAddress` listeners and both addresses in `HBRI.xml` remain
supported.

Both addresses must lead to the configured ADCH++ listener. The hub advertises
`ADHBRI` only after it has successfully bound IPv4 and IPv6 listeners on the
configured port and resolved both validation endpoints. For an encrypted hub,
both listeners must use the same TLS configuration expected by clients.

References:

- <https://github.com/maksis/adchpp-hbri>
- <https://github.com/janvidar/uhub/issues/79>
