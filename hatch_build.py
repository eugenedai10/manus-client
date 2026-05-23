"""Hatch build hook that compiles the C++ ManusClient binary.

Runs during ``pip install .`` (wheel build) — invokes ``make`` in the project
root, then bundles the compiled binary and the ManusSDK shared library into the
wheel so that the ``manus-client`` CLI works out of the box.
"""

from __future__ import annotations

import os
import stat
import subprocess

from hatchling.builders.hooks.plugin.interface import BuildHookInterface


class CustomBuildHook(BuildHookInterface):
    PLUGIN_NAME = "custom"

    def initialize(self, version, build_data):
        # Only compile when building a wheel (not sdist)
        if self.target_name != "wheel":
            return

        root = self.root
        binary_path = os.path.join(root, "ManusClient.out")

        # Check for ManusSDK shared library — required for C++ compilation.
        # When installing from git the proprietary SDK is not available, so we
        # skip the build and install only the Python CLI/library parts.
        sdk_so = os.path.join(root, "ManusSDK", "lib", "libManusSDK_Integrated.so")
        if not os.path.isfile(sdk_so) or os.path.getsize(sdk_so) < 1024:
            self.app.display_warning(
                "ManusSDK shared library not found — skipping C++ compilation. "
                "The Python package will be installed without the native binary. "
                "To compile ManusClient.out, clone the repo on a machine with "
                "the ManusSDK and run 'make -j' manually."
            )
            return

        # ── 1. Compile ──────────────────────────────────────────────────
        self.app.display_info("Compiling ManusClient C++ binary …")
        try:
            subprocess.check_call(
                ["make", "-j"],
                cwd=root,
            )
        except FileNotFoundError:
            raise RuntimeError(
                "'make' is not installed.  Install build dependencies:\n"
                "  sudo apt install build-essential libzmq3-dev libncurses-dev"
            )
        except subprocess.CalledProcessError as exc:
            raise RuntimeError(
                f"C++ compilation failed (exit {exc.returncode}).  "
                "Make sure system libraries are installed:\n"
                "  sudo apt install build-essential libzmq3-dev libncurses-dev"
            ) from exc

        if not os.path.isfile(binary_path):
            raise RuntimeError(
                f"Compilation succeeded but {binary_path} was not produced."
            )

        # Ensure the binary is executable
        st = os.stat(binary_path)
        os.chmod(binary_path, st.st_mode | stat.S_IEXEC | stat.S_IXGRP | stat.S_IXOTH)

        # ── 2. Bundle artifacts into the wheel ──────────────────────────
        build_data["force_include"][binary_path] = "manus_client/_bin/ManusClient"

        # ManusSDK shared library (required at runtime)
        sdk_so = os.path.join(root, "ManusSDK", "lib", "libManusSDK_Integrated.so")
        if os.path.isfile(sdk_so):
            build_data["force_include"][sdk_so] = (
                "manus_client/_lib/libManusSDK_Integrated.so"
            )
        else:
            self.app.display_warning(
                f"ManusSDK library not found at {sdk_so} — "
                "the installed package will not work without it."
            )

        # Calibration files (optional but handy)
        for cal in ("Calibration_left.mcal", "Calibration_right.mcal"):
            cal_path = os.path.join(root, cal)
            if os.path.isfile(cal_path):
                build_data["force_include"][cal_path] = f"manus_client/_data/{cal}"
