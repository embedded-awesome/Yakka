#!/usr/bin/env python3
"""Measure no-op build time for a Yakka project and, when available, Ninja on the generated manifest.

Usage: tools/benchmark_noop.py [--yakka BIN] [--runs N] [--project NAME] [-- yakka arguments...]
Example: tools/benchmark_noop.py --runs 20 -- link! yakka_cli xcode
"""
import argparse
import os
import shutil
import statistics
import subprocess
import sys
import tempfile
import time


def measure(label, command):
    # The warm-up run converges any pending work so the timed runs are true no-ops
    subprocess.run(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    samples = []
    for _ in range(args.runs):
        start = time.perf_counter()
        result = subprocess.run(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        samples.append((time.perf_counter() - start) * 1000)
        if result.returncode != 0:
            sys.exit(f"{label} failed ({result.returncode}): {result.stderr.decode(errors='replace')}")
    print(f"{label:<6} min {min(samples):7.1f} ms  median {statistics.median(samples):7.1f} ms  max {max(samples):7.1f} ms  ({len(samples)} runs)")
    return statistics.median(samples)


parser = argparse.ArgumentParser()
parser.add_argument("--yakka", default="./output/yakka_cli-xcode/yakka_cli-xcode")
parser.add_argument("--runs", type=int, default=10)
parser.add_argument("--project")
parser.add_argument("arguments", nargs="*")
args = parser.parse_args()
arguments = args.arguments or ["link!", "yakka_cli", "xcode"]
project = args.project or "-".join(a for a in arguments if not a.endswith("!") and not a.startswith("-"))

# Copy the binary so a build that relinks it cannot change the program under test
with tempfile.TemporaryDirectory() as temp:
    binary = os.path.join(temp, "yakka")
    shutil.copy2(args.yakka, binary)
    yakka_ms = measure("yakka", [binary, *arguments])

manifest = os.path.join("output", project, "build.ninja")
if os.path.exists(manifest) and shutil.which("ninja"):
    ninja_ms = measure("ninja", ["ninja", "-f", manifest])
    print(f"ratio  {yakka_ms / ninja_ms:.2f}x")
