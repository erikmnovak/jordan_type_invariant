#!/usr/bin/env python3
"""Run named cases in fresh processes, retaining failures as well as timings.

The C++ program owns the case catalogue and all measurements. This optional
POSIX/GNU-time driver only supplies repetitions, resource limits and records.
"""
import argparse
import csv
import hashlib
import io
import json
import os
from pathlib import Path
import resource
import signal
import subprocess
import tempfile
import time


def digest(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("program", type=Path)
    parser.add_argument("output", type=Path, help="Measurement CSV; a .runs.csv companion records status and memory")
    parser.add_argument("cases", nargs="*", help="Named cases; omit to use the complete catalogue")
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--timeout", type=float, default=600)
    parser.add_argument("--memory-mib", type=int, default=2048)
    parser.add_argument("--cache", choices=["reuse", "reset", "both"], default="both")
    args = parser.parse_args()
    if args.repeats < 1 or args.timeout <= 0 or args.memory_mib < 1:
        parser.error("Repetitions, timeout and memory allowance must be positive")
    program = args.program.resolve()
    listing = subprocess.run([str(program), "--list"], capture_output=True, text=True, check=True)
    catalogue = list(csv.DictReader(io.StringIO(listing.stdout)))
    names = {case["workload"] for case in catalogue}
    if set(args.cases) - names:
        parser.error("Unknown cases: " + ", ".join(sorted(set(args.cases) - names)))
    selected = [case for case in catalogue if not args.cases or case["workload"] in args.cases]
    directory = Path(__file__).resolve().parent
    sources = json.loads((directory / "data/sources.json").read_text())
    recorded = {(directory / "data" / item["file"]).resolve(): item["sha256"] for item in sources["presentations"]}
    hashes = {}
    for case in selected:
        for field in ["presentation", "slice_file"]:
            if case[field]:
                path = Path(case[field]).resolve()
                if path not in hashes:
                    hashes[path] = digest(path)
                    if path in recorded and hashes[path] != recorded[path]:
                        raise RuntimeError("Dataset differs from its recorded source: " + str(path))
    binary_hash = digest(program)
    recipes_hash = digest(directory / "workloads.hpp")
    status_path = args.output.with_suffix(".runs.csv")
    if args.output.exists() or status_path.exists():
        parser.error("Result files already exist; choose a new output name")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    fields = ["workload", "repeat", "cache", "seed", "status", "returncode", "elapsed_s", "peak_rss_kib",
              "timeout_s", "memory_mib", "presentation_sha256", "slices_sha256", "program_sha256", "recipes_sha256", "error"]
    failure = False
    with args.output.open("w", newline="") as measured, status_path.open("w", newline="") as statuses:
        status_writer = csv.DictWriter(statuses, fieldnames=fields)
        status_writer.writeheader()
        measurement_writer = None
        for case in selected:
            for repeat in range(1, args.repeats + 1):
                # Different generated instances, identical across cache policies.
                # Fixed-file and deterministic cases repeat their same input.
                seed = (int(case["seed"]) + repeat - 1) % (2**32) if case["seed"] else None
                policies = ["reuse", "reset"] if args.cache == "both" else [args.cache]
                if repeat % 2 == 0:
                    policies.reverse()
                expected = None
                for policy in policies:
                    with tempfile.TemporaryDirectory(prefix="jordan-suite-run-") as temporary:
                        memory_file = Path(temporary) / "memory.txt"
                        command = ["/usr/bin/time", "-f", "%M", "-o", str(memory_file), str(program), case["workload"], "--cache", policy]
                        if seed is not None:
                            command += ["--seed", str(seed)]

                        def limits():
                            size = args.memory_mib * 1024 * 1024
                            resource.setrlimit(resource.RLIMIT_AS, (size, size))

                        start = time.monotonic()
                        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                                   text=True, start_new_session=True, preexec_fn=limits)
                        status = "ok"
                        try:
                            stdout, stderr = process.communicate(timeout=args.timeout)
                        except subprocess.TimeoutExpired:
                            os.killpg(process.pid, signal.SIGKILL)
                            stdout, stderr = process.communicate()
                            status = "timeout"
                        elapsed = time.monotonic() - start
                        if status == "ok" and process.returncode != 0:
                            status = "memory_limit" if "bad_alloc" in stderr else "error"
                        rows = list(csv.DictReader(io.StringIO(stdout))) if status == "ok" else []
                        if status == "ok" and (not rows or any(row.get("workload") != case["workload"] or
                                                              "checksum" not in row or None in row or
                                                              None in row.values() for row in rows)):
                            status = "invalid_output"
                        if status == "ok":
                            checksums = [(row.get("method", "jordan_type"), row["checksum"]) for row in rows]
                            if expected is not None and checksums != expected:
                                status = "result_mismatch"
                            expected = checksums
                        if status == "ok":
                            if measurement_writer is None:
                                measurement_writer = csv.DictWriter(measured, fieldnames=["repeat", *rows[0].keys()])
                                measurement_writer.writeheader()
                            for row in rows:
                                measurement_writer.writerow({"repeat": repeat, **row})
                            measured.flush()
                        else:
                            failure = True
                        memory = memory_file.read_text().strip().splitlines() if memory_file.exists() else []
                        rss = memory[-1] if memory and memory[-1].isdigit() else ""
                        status_writer.writerow({"workload": case["workload"], "repeat": repeat, "cache": policy,
                            "seed": "" if seed is None else seed, "status": status, "returncode": process.returncode,
                            "elapsed_s": f"{elapsed:.6f}", "peak_rss_kib": rss, "timeout_s": args.timeout,
                            "memory_mib": args.memory_mib,
                            "presentation_sha256": hashes.get(Path(case["presentation"]).resolve(), "") if case["presentation"] else "",
                            "slices_sha256": hashes.get(Path(case["slice_file"]).resolve(), "") if case["slice_file"] else "",
                            "program_sha256": binary_hash, "recipes_sha256": recipes_hash, "error": stderr[-2000:].strip()})
                        statuses.flush()
                        print(case["workload"], repeat, policy, status, f"{elapsed:.2f}s", flush=True)
    return int(failure)


if __name__ == "__main__":
    raise SystemExit(main())
