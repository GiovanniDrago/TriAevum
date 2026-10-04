"""Windows qualification battery of the line after alpha.2c.

Runs the layers of docs/TRIAEVUM_WINDOWS_RELEASE_QUALIFICATION.md section 3 and
writes one report. Machine-specific paths (build trees, build commands, private
inputs) come from a local JSON configuration; an example lives next to this
file. Nothing here reads or writes ROM-derived data outside the configured
private directories, and every output goes to the report directory.

usage: qualify_windows_battery.py --config LOCAL.json --output DIR [--layers build,unit]

Layers implemented here:
- build: the product targets and every test executable CTest knows, so a test
  that does not build fails the battery instead of disappearing;
- unit: CTest of each build tree (JUnit per test), the generator --check
  commands and the Python suites.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time
import xml.etree.ElementTree as ElementTree
from dataclasses import asdict, dataclass, field
from pathlib import Path

LAYERS = ("build", "unit")


@dataclass
class Check:
    layer: str
    name: str
    status: str  # pass, fail, notrun, skipped
    detail: str = ""
    seconds: float = 0.0
    log: str = ""


@dataclass
class Report:
    started: str
    configuration: str
    checks: list[Check] = field(default_factory=list)

    def add(self, check: Check) -> Check:
        self.checks.append(check)
        return check


def run_logged(command: list[str], log: Path, *, cwd: Path | None = None, timeout: float | None = None,
               env: dict[str, str] | None = None) -> tuple[int | str, float]:
    """Run a command with its output in a log file; returns (exit code or "timeout", seconds)."""
    log.parent.mkdir(parents=True, exist_ok=True)
    start = time.monotonic()
    with log.open("wb") as handle:
        handle.write(("$ " + subprocess.list2cmdline(command) + "\n").encode())
        handle.flush()
        try:
            process = subprocess.run(command, cwd=cwd, stdout=handle, stderr=subprocess.STDOUT, timeout=timeout,
                                     env=env)
            code: int | str = process.returncode
        except subprocess.TimeoutExpired:
            code = "timeout"
        except OSError as error:
            handle.write(f"cannot start: {error}\n".encode())
            code = "not started"
    return code, time.monotonic() - start


def wait_idle(names: list[str], poll: float = 20.0) -> None:
    """Wait until none of the named processes runs (builds and game instances share this machine)."""
    if not names:
        return
    lowered = [name.lower() for name in names]
    while True:
        listing = subprocess.run(["tasklist", "/FO", "CSV", "/NH"], capture_output=True, text=True,
                                 errors="replace").stdout.lower()
        if not any(f'"{name}"' in listing for name in lowered):
            return
        time.sleep(poll)


def ctest_test_targets(ctest: str, build_dir: Path) -> tuple[list[str], list[Path]]:
    """The CMake targets that build the tests of this tree, and the executables CTest expects.

    A test whose executable exists has a command (its first word is the executable; the target is its stem:
    the Ninja generator defines a phony target per CMake target and target names are global). CTest omits the
    command of a test whose executable was never built; such a test was registered by target name
    (CMake/oot3d/tests.cmake), so its name is the target.
    """
    out = subprocess.run([ctest, "--test-dir", str(build_dir), "--show-only=json-v1"], capture_output=True,
                         text=True, errors="replace")
    if out.returncode != 0:
        raise RuntimeError(f"ctest --show-only failed in {build_dir}: {out.stderr.strip()[:300]}")
    targets: list[str] = []
    executables: list[Path] = []
    root = build_dir.resolve()
    for test in json.loads(out.stdout).get("tests", []):
        command = test.get("command") or []
        if command:
            path = Path(command[0])
            try:
                path.resolve().relative_to(root)
            except ValueError:
                continue
            target = path.stem
            if path not in executables:
                executables.append(path)
        elif re.fullmatch(r"[A-Za-z0-9_\-]+", test.get("name", "")):
            target = test["name"]
        else:
            continue
        if target not in targets:
            targets.append(target)
    return targets, executables


def failed_build_outputs(log: Path) -> list[str]:
    """Ninja's FAILED: lines (with -k 0 every independent failure is listed)."""
    failed = []
    for line in log.read_text(errors="replace").splitlines():
        if line.startswith("FAILED: "):
            failed.append(line[len("FAILED: "):].strip())
    return failed


def parse_junit(path: Path) -> list[tuple[str, str, float, str]]:
    """(name, status, seconds, message) for every testcase of a CTest JUnit file."""
    results = []
    root = ElementTree.parse(path).getroot()
    for case in root.iter("testcase"):
        name = case.get("name", "?")
        seconds = float(case.get("time") or 0.0)
        status = case.get("status", "")
        failure = case.find("failure")
        skipped = case.find("skipped")
        output = case.findtext("system-out") or ""
        if failure is not None or status == "fail":
            # The test's own failure lines say more than CTest's "Failed".
            lines = [line.strip() for line in output.splitlines()
                     if re.search(r"FAILED|error|Error|exception|assert", line)]
            message = "; ".join(lines[:3]) or (failure.get("message", "") if failure is not None else "")
            status = "fail"
        elif status in ("notrun", "disabled") or skipped is not None:
            message = (skipped.get("message", "") if skipped is not None else "") or status
            status = "skipped" if status == "disabled" or "skip" in message.lower() else "notrun"
        else:
            message = ""
            status = "pass"
        results.append((name, status, seconds, message[:300]))
    return results


def unittest_summary(log: Path) -> tuple[str, str]:
    """Status and a one-line summary of a `python -m unittest` run."""
    lines = log.read_text(errors="replace").splitlines()
    # Tests may print after unittest's summary (subprocess output flushed late): find the last "Ran N tests"
    # and the verdict that follows it.
    for index in range(len(lines) - 1, -1, -1):
        match = re.match(r"^Ran (\d+) tests? in", lines[index])
        if not match:
            continue
        for verdict in lines[index + 1:]:
            verdict = verdict.strip()
            if verdict.startswith("OK"):
                return "pass", f"{match.group(1)} tests, {verdict}"
            if verdict.startswith("FAILED"):
                return "fail", f"{match.group(1)} tests, {verdict}"
        return "fail", f"{match.group(1)} tests, no verdict"
    errors = [line for line in lines if "Error" in line or "error" in line]
    return "fail", (errors[-1] if errors else (lines[-1] if lines else "no output"))[:200]


def pytest_summary(log: Path) -> tuple[str, str]:
    """Status and summary of a `python -m pytest -q` run (its last counts line)."""
    lines = [line.strip("= ").strip() for line in log.read_text(errors="replace").splitlines()]
    for line in reversed(lines):
        if re.search(r"\b\d+ (passed|failed|error|errors|skipped)\b", line):
            bad = re.search(r"\b\d+ (failed|error|errors)\b", line)
            return ("fail" if bad else "pass"), line[:200]
    return "fail", (lines[-1] if lines else "no output")[:200]


def layer_build(config: dict, out: Path, report: Report) -> None:
    ctest = config["ctest"]
    for tree in config["build_trees"]:
        build_dir = Path(tree["dir"])
        wait_idle(config.get("wait_idle_processes", []))
        # Regenerate the build files first (Ninja re-runs CMake when a list file changed), so CTest lists
        # the tests of the current sources.
        log = out / "logs" / f"configure-{tree['name']}.log"
        code, seconds = run_logged(list(tree["build_command"]) + ["build.ninja"], log, timeout=3600)
        if code != 0:
            report.add(Check("build", f"{tree['name']}: configure", "fail", f"exit {code}", seconds, str(log)))
            continue
        try:
            tests, _ = ctest_test_targets(ctest, build_dir)
        except (RuntimeError, OSError, json.JSONDecodeError) as error:
            report.add(Check("build", f"{tree['name']}: test list", "fail", str(error)))
            continue
        targets = list(dict.fromkeys(list(tree.get("targets", [])) + tests))
        log = out / "logs" / f"build-{tree['name']}.log"
        code, seconds = run_logged(list(tree["build_command"]) + ["-k", "0"] + targets, log,
                                   timeout=tree.get("timeout_seconds", 14400))
        failed = failed_build_outputs(log)
        status = "pass" if code == 0 else "fail"
        detail = f"{len(targets)} targets ({len(tests)} test targets), exit {code}"
        if failed:
            detail += f"; {len(failed)} failed outputs: " + ", ".join(failed[:8])
        report.add(Check("build", tree["name"], status, detail, seconds, str(log)))


def layer_unit(config: dict, out: Path, report: Report) -> None:
    ctest = config["ctest"]
    for tree in config["build_trees"]:
        build_dir = Path(tree["dir"])
        wait_idle(config.get("wait_idle_processes", []))
        junit = out / f"ctest-{tree['name']}.xml"
        log = out / "logs" / f"ctest-{tree['name']}.log"
        command = [ctest, "--test-dir", str(build_dir), "--output-junit", str(junit.resolve()),
                   "--timeout", str(tree.get("test_timeout_seconds", 900)), "-j", str(tree.get("test_jobs", 1))]
        # Private inputs reach the tests through the environment (for example OOT3D_TEST_EXTRACT_ROOT);
        # a test that lacks them reports itself skipped.
        environment = os.environ.copy()
        environment.update(tree.get("test_env", {}))
        code, seconds = run_logged(command, log, timeout=tree.get("ctest_timeout_seconds", 21600), env=environment)
        if not junit.exists():
            report.add(Check("unit", f"{tree['name']}: ctest", "fail", f"no JUnit output, exit {code}", seconds,
                             str(log)))
            continue
        cases = parse_junit(junit)
        counts: dict[str, int] = {}
        for name, status, case_seconds, message in cases:
            counts[status] = counts.get(status, 0) + 1
            if status != "pass":
                report.add(Check("unit", f"{tree['name']}: {name}", status, message, case_seconds, str(log)))
        summary = ", ".join(f"{key} {value}" for key, value in sorted(counts.items()))
        clean = not counts.get("fail") and not counts.get("notrun")
        report.add(Check("unit", f"{tree['name']}: ctest", "pass" if clean else "fail",
                         f"{len(cases)} tests: {summary}", seconds, str(log)))
    for check in config.get("generator_checks", []):
        log = out / "logs" / f"generator-{check['name']}.log"
        code, seconds = run_logged(list(check["command"]), log, cwd=Path(check.get("cwd", ".")),
                                   timeout=check.get("timeout_seconds", 1800))
        last = log.read_text(errors="replace").strip().splitlines()[-1:] or [""]
        report.add(Check("unit", f"generator: {check['name']}", "pass" if code == 0 else "fail",
                         f"exit {code}: {last[0][:200]}", seconds, str(log)))
    repository = Path(config["repository"])
    for suite in config.get("python_suites", []):
        name = suite["name"]
        log = out / "logs" / f"python-{name.replace('/', '_')}.log"
        python = config.get("python", sys.executable)
        pytest = suite.get("runner") == "pytest"
        if pytest:
            command = [python, "-m", "pytest", "-q", "-p", "no:cacheprovider", suite["start"]]
        else:
            command = [python, "-m", "unittest", "discover", "-s", suite["start"], "-p",
                       suite.get("pattern", "test_*.py")]
            if suite.get("top"):
                command += ["-t", suite["top"]]
        environment = os.environ.copy()
        for key, value in suite.get("env", {}).items():
            # Relative paths in a suite's environment are relative to the repository.
            environment[key] = os.pathsep.join(str((repository / part).resolve()) if part and not Path(part).is_absolute()
                                               else part for part in value.split(os.pathsep))
        code, seconds = run_logged(command, log, cwd=repository, timeout=suite.get("timeout_seconds", 3600),
                                   env=environment)
        status, detail = pytest_summary(log) if pytest else unittest_summary(log)
        if code not in (0, 1):
            status, detail = "fail", f"exit {code}; {detail}"
        report.add(Check("unit", f"python: {name}", status, detail, seconds, str(log)))


def write_report(report: Report, out: Path) -> tuple[Path, Path]:
    data = asdict(report)
    totals: dict[str, dict[str, int]] = {}
    for check in report.checks:
        totals.setdefault(check.layer, {}).setdefault(check.status, 0)
        totals[check.layer][check.status] += 1
    data["totals"] = totals
    json_path = out / "battery-report.json"
    json_path.write_text(json.dumps(data, indent=2), encoding="utf-8")
    lines = [f"# Windows battery report", "", f"Started {report.started}; configuration `{report.configuration}`.", ""]
    for layer, counts in totals.items():
        lines.append(f"- {layer}: " + ", ".join(f"{key} {value}" for key, value in sorted(counts.items())))
    lines += ["", "| Layer | Check | Status | Detail |", "| --- | --- | --- | --- |"]
    for check in report.checks:
        if check.status != "pass" or check.name.endswith(": ctest") or check.layer == "build":
            detail = check.detail.replace("|", "/").replace("\n", " ")[:240]
            lines.append(f"| {check.layer} | {check.name} | {check.status} | {detail} |")
    md_path = out / "battery-report.md"
    md_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return json_path, md_path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--layers", default=",".join(LAYERS))
    args = parser.parse_args()
    config = json.loads(args.config.read_text(encoding="utf-8"))
    layers = [layer.strip() for layer in args.layers.split(",") if layer.strip()]
    unknown = [layer for layer in layers if layer not in LAYERS]
    if unknown:
        parser.error(f"unknown layers: {', '.join(unknown)} (known: {', '.join(LAYERS)})")
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    report = Report(started=time.strftime("%Y-%m-%d %H:%M:%S"), configuration=str(args.config))
    if "build" in layers:
        layer_build(config, out, report)
    if "unit" in layers:
        layer_unit(config, out, report)
    json_path, md_path = write_report(report, out)
    failing = [check for check in report.checks if check.status in ("fail", "notrun")]
    print(f"battery: {len(report.checks)} checks, {len(failing)} failing; report {md_path}")
    return 1 if failing else 0


if __name__ == "__main__":
    sys.exit(main())
