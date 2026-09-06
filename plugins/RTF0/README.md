<!-- Copyright (C) 2026 iceman50. Licensed under GPL-3.0-or-later. -->

# RTF0 plugin

This plugin implements the hub portion of the
[RTF0 draft specification](https://github.com/janvidar/adc-extensions/blob/main/ADC-rtf0-extension.md),
version 0.2 dated 2026-08-03.

When enabled it advertises `ADRTF0`. ADCH++'s existing `MSG` path then relays
`RT1` and the decoded message unchanged to all intended recipients. Rendering,
Markdown validation, attachment handling, and direct client-to-client
negotiation remain client responsibilities.

When the plugin is loaded but disabled in `RTF0.xml`, it removes `RT1` before
relaying a message, as required for a hub that has not announced the feature.
