# Optional ADCH++ configuration UI

`adchpp-ui` is a standalone, cross-platform PyQt6 application for initial setup
and offline configuration. It manages:

- core hub, logging, buffer, and timeout settings;
- IPv4/IPv6 listeners and their TLS settings;
- the `users.txt` registration database used by `Scripts/access.lua`;
- plugin library discovery, load selection/order, and raw `<Plugin>.xml` files.

The initial `adchpp-ui` implementation was built with development assistance
from OpenAI Codex.

The application is deliberately separate from SCons. A normal ADCH++ build has
no Python or Qt dependency and does not package or launch the UI.

## Installation

Python 3.10 or newer is required. From the repository root, create an isolated
environment and install the application:

```powershell
python -m venv adchpp-ui\.venv
adchpp-ui\.venv\Scripts\python -m pip install -e adchpp-ui
adchpp-ui\.venv\Scripts\adchpp-ui -c .\etc
```

On Linux or macOS, activate the environment or use its executables directly:

```sh
python3 -m venv adchpp-ui/.venv
adchpp-ui/.venv/bin/python -m pip install -e adchpp-ui
adchpp-ui/.venv/bin/adchpp-ui -c ./etc
```

It can also be run from the source tree after installing `requirements.txt`:

```sh
python adchpp-ui/run.py -c ./etc
```

## Usage notes

Stop the hub or Windows service before saving. ADCH++ reads these files at
startup, and the Lua access script can also update `users.txt` while running.
The application validates all managed data before writing and replaces each
file through a temporary file in the same directory.

Plugin settings are intentionally exposed as raw XML because each plugin owns
its own schema. Unknown core XML elements, comments inside the root element,
and unknown user JSON metadata are preserved.

Run the model and offscreen widget smoke tests with:

```sh
adchpp-ui --self-test -c ./etc
```
