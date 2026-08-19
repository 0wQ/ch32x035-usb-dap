#!/usr/bin/env python3

import argparse
import os
import signal
import subprocess
import sys
import time


EXPECTED_LINES = (
    "Attached chip: CH32V30X [CH32V307VCT6] (ChipID: 0x30700528)",
    "FlashSize(288KB) UID(5b-a8-0d-10-53-5c-bb-14)",
)


def stop_process_group(process: subprocess.Popen[str]) -> None:
    os.killpg(process.pid, signal.SIGTERM)
    try:
        process.wait(timeout=1.0)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.wait()


def run_status(device: int, chip: str, timeout: float) -> tuple[bool, str, float]:
    command = [
        "wlink",
        "--device",
        str(device),
        "--chip",
        chip,
        "status",
    ]
    start = time.monotonic()
    process = subprocess.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        start_new_session=True,
    )

    try:
        output, _ = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        stop_process_group(process)
        return False, f"TIMEOUT after {timeout:.1f}s", time.monotonic() - start

    elapsed = time.monotonic() - start
    missing = [expected for expected in EXPECTED_LINES if expected not in output]
    if process.returncode != 0:
        return False, f"exit={process.returncode}\n{output.rstrip()}", elapsed
    if missing:
        details = "\n".join(f"missing: {line}" for line in missing)
        return False, f"{details}\n{output.rstrip()}", elapsed
    return True, output.rstrip(), elapsed


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--device", type=int, default=1)
    parser.add_argument("--chip", default="CH32V30X")
    parser.add_argument("--iterations", type=int, default=5)
    parser.add_argument("--timeout", type=float, default=30.0)
    args = parser.parse_args()

    for iteration in range(1, args.iterations + 1):
        passed, details, elapsed = run_status(args.device, args.chip, args.timeout)
        result = "PASS" if passed else "FAIL"
        print(f"[{iteration}/{args.iterations}] {result} {elapsed:.3f}s", flush=True)
        if not passed:
            print(details, flush=True)
            return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
