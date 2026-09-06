<!-- Copyright (C) 2026 iceman50. Licensed under GPL-3.0-or-later. -->

# BBS0 plugin

This plugin implements the hub portion of the
[BBS0 draft specification](https://github.com/janvidar/adc-extensions/blob/main/ADC-bbs0-extension.md),
version 0.1 dated 2026-08-11. Its behavior and configuration model are based
on the corresponding implementation in
[uHub](https://github.com/janvidar/uhub).

The hub stores only an index. Post documents remain ordinary TTH-addressed
files and move between clients through ADC's existing search and transfer
commands. The plugin does not parse, store, fetch, or serve post bodies.

## Configuration

`BBS0.xml` contains global limits and one or more board definitions. Permission
attributes name the lowest ADCH++ client type that may perform the operation:
`guest`, `registered`, `operator`, `superuser`, or `owner`; `none` disables an
operation. The wire-level `PE` bitmask is computed independently for every
session.

`IndexFile` is a relative path beneath the ADCH++ configuration directory or an
absolute path. The on-disk format is an append-only, versioned journal. Fields
that can contain user text are hexadecimal encoded, incomplete final records
are ignored after an interrupted write, retention deletions are journaled, and
the journal is periodically compacted through a recoverable snapshot rotation.

The index contains metadata only. For durable board contents, an operator
should run a client that subscribes to each board and fetches and shares its
post documents.
