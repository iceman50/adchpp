"""Command-line entry point for the ADCH++ configuration manager."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from .model import run_model_self_test


def _arguments(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="ADCH++ cross-platform configuration manager")
    parser.add_argument(
        "-c",
        "--config",
        type=Path,
        default=Path.cwd() / "config",
        help="ADCH++ configuration directory (default: ./config)",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="run model and offscreen widget construction tests, then exit",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    options = _arguments(argv)
    if options.self_test:
        run_model_self_test()

    try:
        from PyQt6.QtWidgets import QApplication
    except ImportError:
        print(
            "PyQt6 is required. Install the UI with: pip install -e adchpp-ui",
            file=sys.stderr,
        )
        return 2

    from .window import ConfigWindow, run_ui_self_test

    if options.self_test:
        run_ui_self_test(options.config.resolve(strict=False))
        return 0

    app = QApplication(sys.argv if argv is None else [sys.argv[0], *argv])
    QApplication.setApplicationName("ADCH++ Configuration Manager")
    QApplication.setOrganizationName("ADCH++")
    window = ConfigWindow(options.config.resolve(strict=False))
    window.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
