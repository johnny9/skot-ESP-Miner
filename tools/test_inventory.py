#!/usr/bin/env python3
"""List active C tests and verify that each test has one owning runtime."""

import argparse
import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TEST_PATTERN = re.compile(
    r'TEST_CASE\s*\(\s*"((?:\\.|[^"\\])*)"\s*,\s*"((?:\\.|[^"\\])*)"\s*\)',
    re.MULTILINE,
)


def strip_comments(source: str) -> str:
    output = []
    index = 0
    state = "code"

    while index < len(source):
        current = source[index]
        following = source[index + 1] if index + 1 < len(source) else ""

        if state == "code" and current == "/" and following == "/":
            output.extend("  ")
            index += 2
            state = "line-comment"
        elif state == "code" and current == "/" and following == "*":
            output.extend("  ")
            index += 2
            state = "block-comment"
        elif state == "line-comment" and current == "\n":
            output.append(current)
            index += 1
            state = "code"
        elif state == "block-comment" and current == "*" and following == "/":
            output.extend("  ")
            index += 2
            state = "code"
        elif state in {"line-comment", "block-comment"}:
            output.append("\n" if current == "\n" else " ")
            index += 1
        else:
            output.append(current)
            index += 1

    return "".join(output)


def find_tests() -> list[dict[str, object]]:
    tests = []
    for path in sorted(ROOT.glob("components/*/test/test_*.c")):
        source = strip_comments(path.read_text(encoding="utf-8"))
        relative_path = path.relative_to(ROOT).as_posix()
        for match in TEST_PATTERN.finditer(source):
            tags = re.findall(r"\[([^]]+)\]", match.group(2))
            owner = "qemu-integration" if "qemu-integration" in tags else "host-unit"
            tests.append(
                {
                    "name": match.group(1),
                    "tags": tags,
                    "owner": owner,
                    "source": relative_path,
                    "line": source.count("\n", 0, match.start()) + 1,
                }
            )
    return tests


def check_inventory(tests: list[dict[str, object]]) -> list[str]:
    errors = []
    host_cmake = (ROOT / "host-tests" / "CMakeLists.txt").read_text(encoding="utf-8")
    names: set[str] = set()

    for test in tests:
        name = str(test["name"])
        source = str(test["source"])
        owner = str(test["owner"])
        if name in names:
            errors.append(f"duplicate test name: {name}")
        names.add(name)

        host_reference = f"../{source}"
        if owner == "host-unit" and host_reference not in host_cmake:
            errors.append(f"host test is not in host-tests/CMakeLists.txt: {source}")
        if owner == "qemu-integration" and host_reference in host_cmake:
            errors.append(f"QEMU integration test is also owned by the host suite: {source}")

    if not any(test["owner"] == "qemu-integration" for test in tests):
        errors.append("no focused QEMU integration test is registered")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", help="fail on missing or duplicate ownership")
    arguments = parser.parse_args()

    tests = find_tests()
    errors = check_inventory(tests)
    if arguments.check:
        if errors:
            for error in errors:
                print(f"ERROR: {error}")
            return 1
        host_count = sum(test["owner"] == "host-unit" for test in tests)
        qemu_count = sum(test["owner"] == "qemu-integration" for test in tests)
        print(f"Test inventory is valid: {host_count} host, {qemu_count} QEMU")
        return 0

    print(json.dumps({"tests": tests, "errors": errors}, indent=2))
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
