"""CLI entry-point for the Manus glove SDK client.

Usage
-----
    manus-client run              # start the glove data stream
    manus-client run --zmq-port 2044
    manus-client info             # print paths and version
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys
from pathlib import Path

_PKG_DIR = Path(__file__).resolve().parent
_BIN = _PKG_DIR / "_bin" / "ManusClient"
_LIB = _PKG_DIR / "_lib"
_DATA = _PKG_DIR / "_data"


def _check_binary() -> None:
    if not _BIN.exists():
        print(
            "Error: ManusClient binary not found.\n"
            f"  Expected at: {_BIN}\n"
            "  The package may not have been built correctly.\n"
            "  Try reinstalling:  pip install --force-reinstall .",
            file=sys.stderr,
        )
        sys.exit(1)

    if not os.access(_BIN, os.X_OK):
        try:
            _BIN.chmod(_BIN.stat().st_mode | 0o755)
        except OSError:
            print(
                f"Error: {_BIN} is not executable and chmod failed.",
                file=sys.stderr,
            )
            sys.exit(1)


def _build_env() -> dict[str, str]:
    """Return a copy of the environment with LD_LIBRARY_PATH set."""
    env = os.environ.copy()
    ld = env.get("LD_LIBRARY_PATH", "")
    env["LD_LIBRARY_PATH"] = str(_LIB) + (":" + ld if ld else "")
    return env


def cmd_run(args: argparse.Namespace) -> None:
    _check_binary()
    env = _build_env()

    # Replace the current process with the C++ binary.
    # Signals (Ctrl-C, SIGTERM, …) go straight to ManusClient.
    os.execvpe(str(_BIN), [str(_BIN)], env)


def cmd_info(_args: argparse.Namespace) -> None:
    from manus_client import __version__

    print(f"manus-client  v{__version__}")
    print(f"  binary : {_BIN}  ({'OK' if _BIN.exists() else 'MISSING'})")
    print(f"  lib    : {_LIB}")
    print(f"  data   : {_DATA}")

    # Show bundled calibration files
    if _DATA.exists():
        cals = list(_DATA.glob("*.mcal"))
        if cals:
            for c in cals:
                print(f"  cal    : {c.name}")

    # Show ManusSDK library
    sdk_so = _LIB / "libManusSDK_Integrated.so"
    if sdk_so.exists():
        size_mb = sdk_so.stat().st_size / (1024 * 1024)
        print(f"  sdk    : libManusSDK_Integrated.so ({size_mb:.0f} MB)")


def cmd_copy_calibration(_args: argparse.Namespace) -> None:
    """Copy bundled calibration files to the current directory."""
    if not _DATA.exists():
        print("No bundled calibration data found.", file=sys.stderr)
        sys.exit(1)
    for cal in _DATA.glob("*.mcal"):
        dest = Path.cwd() / cal.name
        shutil.copy2(cal, dest)
        print(f"  copied {cal.name} → {dest}")


def main() -> None:
    parser = argparse.ArgumentParser(
        prog="manus-client",
        description="Manus Glove SDK client — streams hand tracking data over ZMQ.",
    )
    sub = parser.add_subparsers(dest="command")

    sub.add_parser("run", help="Start the Manus SDK client")
    sub.add_parser("info", help="Show install paths and version")
    sub.add_parser(
        "copy-calibration",
        help="Copy bundled calibration files (*.mcal) to the current directory",
    )

    args = parser.parse_args()

    commands = {
        "run": cmd_run,
        "info": cmd_info,
        "copy-calibration": cmd_copy_calibration,
    }

    handler = commands.get(args.command)
    if handler is None:
        parser.print_help()
        sys.exit(1)

    handler(args)
