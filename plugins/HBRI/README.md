<!-- Copyright (C) 2026 iceman50. Licensed under GPL-3.0-or-later. -->

# HBRI plugin

This plugin implements the hub side of the unofficial HBRI hybrid IPv4/IPv6
reachability extension. A dual-stack client logs in immediately over its
primary address family. The hub strips the unverified secondary address and
sends an `ITCP` challenge; the client opens a short-lived connection over the
other family and responds with `HTCP`. Only the address observed on that
connection is then published in the client's INF.

The implementation uses single-use 192-bit opaque tokens, remembers the
secondary UDP port until validation, supports post-login revalidation, rejects
same-family validation attempts, and never blocks login while validation is in
progress.

## Configuration

Configure `HBRI.xml` with public IPv4 and IPv6 validation addresses and the
shared listening port, then set `Enabled="1"`. Address values may be IP
literals or family-specific DNS names. The hub advertises `ADHBRI` only when
the plugin is enabled and all three endpoint values are valid.

Both addresses must lead to the configured ADCH++ listener. For an encrypted
hub, that listener must use the same TLS configuration expected by clients.

References:

- <https://github.com/maksis/adchpp-hbri>
- <https://github.com/janvidar/uhub/issues/79>
