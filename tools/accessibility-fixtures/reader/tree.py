#!/usr/bin/env python3
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time


DIRECTORY = Path(__file__).resolve().parent
READER = DIRECTORY / "build/ax-tree"
DEFAULT_FIELDS = "AXRole,AXSubrole,AXTitle,AXDescription,AXIdentifier,AXDOMIdentifier,AXValue,AXEnabled,AXFocused,AXSelected,AXExpanded,AXDisclosing,AXMinValue,AXMaxValue,AXSelectedTextRange,AXNumberOfCharacters,AXPosition,AXSize"
ABSENT = {-25205, -25212, -25208}


class Incomplete(Exception):
    pass


def load(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def save(path, value):
    with Path(path).open("x", encoding="utf-8") as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2, allow_nan=False)
        stream.write("\n")


def equal(left, right):
    if isinstance(left, bool) != isinstance(right, bool):
        return False
    if isinstance(left, dict) and isinstance(right, dict):
        return left.keys() == right.keys() and all(equal(left[key], right[key]) for key in left)
    if isinstance(left, list) and isinstance(right, list):
        return len(left) == len(right) and all(equal(a, b) for a, b in zip(left, right))
    return left == right


def validate_spec(spec):
    if set(spec) != {"version", "cases"} or spec["version"] != 1 or not isinstance(spec["cases"], list) or not spec["cases"]:
        raise ValueError("spec must contain version: 1 and a nonempty cases list")
    names = set()
    fields = set()
    for case in spec["cases"]:
        if set(case) - {"name", "match", "baseline", "candidate", "attributes", "expected", "subtree"}:
            raise ValueError("unknown case option")
        name = case.get("name")
        if not isinstance(name, str) or not name or name in names:
            raise ValueError("case names must be unique nonempty strings")
        names.add(name)
        if ("match" in case) == ("baseline" in case or "candidate" in case):
            raise ValueError(f"{name}: specify match, or both baseline and candidate selectors")
        for side in ("baseline", "candidate"):
            selector = case.get(side, case.get("match"))
            if not isinstance(selector, dict) or not selector:
                raise ValueError(f"{name}: missing {side} selector")
            if not all(isinstance(key, str) and key.startswith("AX") and isinstance(value, (str, int, float, bool)) for key, value in selector.items()):
                raise ValueError(f"{name}: selectors use exact scalar AX attributes")
            fields.update(selector)
        attributes = case.get("attributes")
        if not isinstance(attributes, list) or not attributes or len(set(attributes)) != len(attributes):
            raise ValueError(f"{name}: attributes must be a nonempty unique list")
        if not all(isinstance(field, str) and (field.startswith("AX") or field in ("@actions", "@queries")) for field in attributes):
            raise ValueError(f"{name}: invalid attribute")
        if not isinstance(case.get("subtree", False), bool):
            raise ValueError(f"{name}: subtree must be boolean")
        expected = case.get("expected", {})
        if not isinstance(expected, dict) or set(expected) - set(attributes):
            raise ValueError(f"{name}: expected keys must be compared attributes")
        fields.update(field for field in attributes if field.startswith("AX"))
    return sorted(fields | {"AXRole"})


def index_snapshot(snapshot):
    if snapshot.get("schema") != "asb-ax-tree-1" or snapshot.get("readOnly") is not True or snapshot.get("trusted") is not True:
        raise Incomplete("not a trusted public AX snapshot")
    if snapshot.get("complete") is not True or snapshot.get("issues") != []:
        raise Incomplete("incomplete snapshot: " + str(snapshot.get("issues")))
    nodes = snapshot.get("nodes", [])
    index = {node["id"]: node for node in nodes}
    if not nodes or len(nodes) != len(index) or not snapshot.get("roots"):
        raise Incomplete("empty or duplicate node identities")
    for node in nodes:
        if node.get("childrenError") not in ABSENT | {0} or any(child not in index for child in node["children"]):
            raise Incomplete("unread child edges")
    seen, queue = set(), list(snapshot["roots"])
    for key in queue:
        if key not in index:
            raise Incomplete("unread root")
        if key not in seen:
            seen.add(key)
            queue.extend(index[key]["children"])
    if seen != index.keys():
        raise Incomplete("unreachable nodes")
    return index


def observation(node, field):
    if field == "@actions":
        return node.get("actions", {"unread": True})
    if field == "@queries":
        return node.get("queries", {"unread": True})
    return node["attributes"].get(field, {"unread": True})


def normalize(value, identities, allow_absent):
    if not isinstance(value, dict):
        raise Incomplete("invalid encoded observation")
    if set(value) == {"ref"}:
        if value["ref"] not in identities:
            raise Incomplete("reference outside selected subtree; select a larger root")
        return {"ref": identities[value["ref"]]}
    if set(value) == {"items"}:
        return {"items": [normalize(item, identities, allow_absent) for item in value["items"]]}
    if set(value) == {"error"} and value["error"] in ABSENT and allow_absent:
        return value
    if "value" in value and not set(value) - {"value", "type"}:
        return value
    raise Incomplete("attribute not readable: " + str(value))


def project(index, case, side):
    selector = case.get(side, case.get("match"))
    matches = [node for node in index.values() if all(
        "value" in observation(node, field) and equal(observation(node, field)["value"], value)
        for field, value in selector.items())]
    if len(matches) != 1:
        raise Incomplete(f"{side} selector matched {len(matches)} nodes: {selector}")
    root = matches[0]
    keys, identities = [root["id"]], {root["id"]: 0}
    if case.get("subtree"):
        for key in keys:
            for child in index[key]["children"]:
                if child not in identities:
                    identities[child] = len(keys)
                    keys.append(child)
    fields = sorted(set(case["attributes"]) | ({"AXRole"} if case.get("subtree") else set()))
    result = []
    for key in keys:
        node = index[key]
        row = {"attributes": {field: normalize(observation(node, field), identities,
            bool(case.get("subtree")) and field != "AXRole") for field in fields}}
        if case.get("subtree"):
            row["children"] = [identities[child] for child in node["children"]]
        result.append(row)
    return result


def differences(left, right, path="", output=None):
    output = [] if output is None else output
    if equal(left, right) or len(output) >= 101:
        return output
    if isinstance(left, dict) and isinstance(right, dict) and left.keys() == right.keys():
        for key in left:
            differences(left[key], right[key], f"{path}/{key}", output)
    elif isinstance(left, list) and isinstance(right, list) and len(left) == len(right):
        for i, (a, b) in enumerate(zip(left, right)):
            differences(a, b, f"{path}/{i}", output)
    else:
        output.append({"path": path, "baseline": left, "candidate": right})
    return output


def compare(spec, baseline, candidate, after=None):
    validate_spec(spec)
    results = []
    try:
        before_index = index_snapshot(baseline)
        candidate_index = index_snapshot(candidate)
        after_index = index_snapshot(after) if after is not None else None
        if after is not None and any(baseline.get(key) != after.get(key) for key in ("pid", "executable", "processStartedAt")):
            raise Incomplete("baseline process identity changed")
    except Incomplete as error:
        return {"status": "INCOMPLETE", "reason": str(error), "cases": []}
    for case in spec["cases"]:
        result = {"name": case["name"]}
        try:
            native = project(before_index, case, "baseline")
            if after_index is not None and not equal(native, project(after_index, case, "baseline")):
                raise Incomplete("baseline changed during paired capture")
            for field, expected in case.get("expected", {}).items():
                observed = native[0]["attributes"][field]
                if "value" not in observed or not equal(observed["value"], expected):
                    raise Incomplete(f"baseline precondition failed: {field}")
            mirrored = project(candidate_index, case, "candidate")
            result["status"] = "PASS" if equal(native, mirrored) else "FAIL"
            result["comparedNodes"] = [len(native), len(mirrored)]
            result["attributes"] = case["attributes"]
            changed = differences(native, mirrored)
            result["differences"] = changed[:100]
            result["differencesTruncated"] = len(changed) > 100
        except Incomplete as error:
            result.update(status="INCOMPLETE", reason=str(error))
        results.append(result)
    status = "INCOMPLETE" if any(r["status"] == "INCOMPLETE" for r in results) else "FAIL" if any(r["status"] == "FAIL" for r in results) else "PASS"
    return {"status": status, "scope": "read-only public AX parity", "baselineBracketed": after is not None,
        "baselinePID": baseline.get("pid"), "candidatePID": candidate.get("pid"),
        "sameProcess": baseline.get("pid") == candidate.get("pid"), "cases": results}


def capture(pid, fields, args, window=""):
    if not READER.is_file():
        raise ValueError("build the reader first: tools/accessibility-fixtures/reader/build.sh")
    command = [str(READER), str(pid), str(args.max_nodes), str(args.timeout), ",".join(fields), window]
    start = time.monotonic()
    try:
        process = subprocess.run(command, capture_output=True, text=True, timeout=args.timeout + 1)
        snapshot = json.loads(process.stdout)
        if process.returncode != 0:
            snapshot["complete"] = False
            snapshot.setdefault("issues", []).append(f"reader exit {process.returncode}")
    except (subprocess.TimeoutExpired, json.JSONDecodeError) as error:
        snapshot = {"schema": "asb-ax-tree-1", "complete": False, "issues": [type(error).__name__], "nodes": [], "pid": pid}
    snapshot["commandElapsedMs"] = (time.monotonic() - start) * 1000
    return snapshot


def vm_viewer(name, timeout):
    sys.path.insert(0, str(DIRECTORY.parents[1] / "headless-api"))
    import asb
    client = asb.connect()
    identity = client.vm_id(name)
    state = client.status(identity)
    if not state["running"]:
        code, response = client.start(identity)
        if code != 202:
            raise RuntimeError(f"VM start failed: {response}")
    deadline = time.monotonic() + timeout
    while not client.display_ready(identity):
        if time.monotonic() >= deadline:
            raise RuntimeError("VM display readiness deadline exceeded")
        time.sleep(1)
    code, response = client.open_display(identity)
    if code != 200:
        raise RuntimeError(f"VM display open failed: {response}")
    discovery = load(asb._discovery_path())
    if discovery["endpoint"].rstrip("/") + "/v1" != client.base:
        raise RuntimeError("API owner changed during VM startup")
    state = client.status(identity)
    return client, identity, discovery["pid"], state["name"] + " — Display", {
        "id": str(identity), "name": state["name"], "osType": state["osType"],
        "host": client.version(), "before": state}


def main():
    parser = argparse.ArgumentParser(description="Read and compare the host's public macOS accessibility tree.")
    commands = parser.add_subparsers(dest="command", required=True)
    snapshot_parser = commands.add_parser("snapshot")
    snapshot_parser.add_argument("--pid", type=int, required=True)
    snapshot_parser.add_argument("--window", default="")
    snapshot_parser.add_argument("--attributes", default=DEFAULT_FIELDS)
    snapshot_parser.add_argument("--output", type=Path, required=True)
    compare_parser = commands.add_parser("compare")
    compare_parser.add_argument("--baseline", type=Path, required=True)
    compare_parser.add_argument("--candidate", type=Path, required=True)
    compare_parser.add_argument("--baseline-after", type=Path)
    compare_parser.add_argument("--output", type=Path)
    pair_parser = commands.add_parser("pair")
    pair_parser.add_argument("--baseline-pid", type=int, required=True)
    target = pair_parser.add_mutually_exclusive_group(required=True)
    target.add_argument("--candidate-pid", type=int)
    target.add_argument("--vm", help="start/open this VM through the existing headless API")
    pair_parser.add_argument("--vm-timeout", type=float, default=180)
    pair_parser.add_argument("--baseline-window", default="")
    pair_parser.add_argument("--candidate-window", default="")
    pair_parser.add_argument("--output-dir", type=Path, required=True)
    for subparser in (compare_parser, pair_parser):
        subparser.add_argument("--spec", type=Path, required=True)
    for subparser in (snapshot_parser, pair_parser):
        subparser.add_argument("--max-nodes", type=int, default=8192, choices=range(1, 65537), metavar="1..65536")
        subparser.add_argument("--timeout", type=float, default=10)
    args = parser.parse_args()
    try:
        if args.command != "compare" and not 0 < args.timeout <= 120:
            raise ValueError("timeout must be in (0, 120]")
        if args.command == "snapshot":
            result = capture(args.pid, args.attributes.split(","), args, args.window)
            save(args.output, result)
            print(json.dumps({"complete": result["complete"], "nodes": len(result["nodes"]), "elapsedMs": result["commandElapsedMs"], "output": str(args.output)}))
            return 0 if result["complete"] else 2
        spec = load(args.spec)
        fields = validate_spec(spec)
        if args.command == "pair":
            args.output_dir.mkdir(parents=True, exist_ok=False)
            save(args.output_dir / "spec.json", spec)
            vm = None
            if args.vm:
                if not 0 < args.vm_timeout <= 600:
                    raise ValueError("VM timeout must be in (0, 600]")
                client, identity, args.candidate_pid, args.candidate_window, vm = vm_viewer(args.vm, args.vm_timeout)
                save(args.output_dir / "vm-before.json", vm)
                if args.baseline_pid == args.candidate_pid:
                    raise ValueError("the native baseline must be a different process from the VM viewer")
            captures = []
            for name, pid, window in (("baseline", args.baseline_pid, args.baseline_window),
                    ("candidate", args.candidate_pid, args.candidate_window),
                    ("baseline-after", args.baseline_pid, args.baseline_window)):
                captured = capture(pid, fields, args, window)
                save(args.output_dir / f"{name}.json", captured)
                captures.append(captured)
            result = compare(spec, *captures)
            if vm is not None:
                state = client.status(identity)
                result["vm"] = {"id": str(identity), "name": state["name"], "osType": state["osType"]}
                save(args.output_dir / "vm-after.json", state)
                if not state["running"] or not state["displayOpen"] or not state["agentOnline"]:
                    result.update(status="INCOMPLETE", reason="VM or viewer became unavailable during capture")
            result["captureMs"] = [item["commandElapsedMs"] for item in captures]
            result["sha256"] = {name: hashlib.sha256((args.output_dir / name).read_bytes()).hexdigest()
                for name in ("spec.json", "baseline.json", "candidate.json", "baseline-after.json")}
            result["readerSHA256"] = hashlib.sha256(READER.read_bytes()).hexdigest()
            save(args.output_dir / "result.json", result)
        else:
            result = compare(spec, load(args.baseline), load(args.candidate), load(args.baseline_after) if args.baseline_after else None)
            if args.output:
                save(args.output, result)
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return {"PASS": 0, "FAIL": 1, "INCOMPLETE": 2}[result["status"]]
    except (OSError, ValueError, KeyError, TypeError, RuntimeError) as error:
        print(str(error), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
