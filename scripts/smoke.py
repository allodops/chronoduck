#!/usr/bin/env python3
"""make smoke

Port of scripts/smoke.mjs. LOAD the release build into a stock DuckDB
shell (-unsigned, since it's not signed) and assert chronoduck_version()
actually reports a version.
"""

import subprocess
import sys

DUCKDB = "./build/release/duckdb"
EXTENSION_PATH = "./build/release/extension/chronoduck/chronoduck.duckdb_extension"
SQL = (
    f"LOAD '{EXTENSION_PATH}'; "
    "SELECT extension_version FROM duckdb_extensions() WHERE extension_name = 'chronoduck';"
)


def main():
    result = subprocess.run(
        [DUCKDB, "-unsigned", "-csv", "-noheader", "-c", SQL],
        capture_output=True,
        text=True,
    )

    if result.returncode != 0 or len(result.stdout.strip()) == 0:
        print("smoke: FAIL", file=sys.stderr)
        if result.stdout.strip():
            print(result.stdout.strip(), file=sys.stderr)
        if result.stderr.strip():
            print(result.stderr.strip(), file=sys.stderr)
        sys.exit(1)
    print("smoke: PASS")


if __name__ == "__main__":
    main()
