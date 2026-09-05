#!/usr/bin/env python3
"""make fixtures-validate

Validates every fixture under
test/fixtures/*.yaml against the shape docs/testing/registry-and-fixtures.md
defines (test/fixtures/schema.json is the canonical field reference; this
script implements the checks directly rather than a generic JSON-Schema
interpreter, per Article IV.2's dependency-light-script preference).
EDGE_MODES, DOMAINS and REQUIRED_TOP_LEVEL are read from schema.json at
runtime (a local json.load, not a schema-interpreter library) rather than
kept as a hardcoded parallel copy of the same lists. Two rules beyond plain
structural validation: the inert-fixture rule (samples with no expected is
red — a fixture that asserts nothing isn't a fixture), and that a
forbidden-language token is only ever a violation in a fixture *key* or its
`fixture`/`function` values, never a provenance *value*
(scripts/hygiene/forbid-consumer.py already enforces that scoping; this
script does not duplicate that scan).
"""

import json
import re
import sys
from decimal import Decimal
from pathlib import Path

import yaml

# PyYAML's default (YAML 1.1) float resolver requires both a decimal point
# and a signed exponent to recognize scientific notation, so `1e9`, `1E9`,
# `-1e21` and even `1.5e21` (no exponent sign) are left as plain strings —
# only `1.5e+21` (decimal point *and* signed exponent) already resolves as a
# float. Fixture authors write numeric grid/window/lookback/sample values in
# all of these forms, and this script's numeric type checks (_is_number
# below) need every one of them recognized as a number, not a string (see
# adr-lint.py's `_NoTimestampLoader` for a similar PyYAML implicit-resolver
# override). A loader whose float implicit resolver additionally matches the
# broader numeric-literal grammar — `^[-+]?(\d+\.?\d*|\.\d+)([eE][-+]?\d+)?$`,
# i.e. scientific notation with or without a decimal point, with or without
# a signed exponent — closes that gap without touching anything else PyYAML
# already resolves as a number: the resolver is appended after PyYAML's own
# int/float resolvers for the same leading characters, so already-correct
# matches (plain integers, `0x1A` hex, a YAML-1.1-style `007` octal,
# decimal-with-signed-exponent floats) are found first and never reach this
# one.
class _ScientificNotationLoader(yaml.SafeLoader):
    pass


_ScientificNotationLoader.add_implicit_resolver(
    "tag:yaml.org,2002:float",
    re.compile(r"^[-+]?(\d+\.?\d*|\.\d+)([eE][-+]?\d+)?$"),
    list("-+0123456789."),
)

# A violation message embeds fixture values the same way the original
# fixtures-validate.mjs did: `${value}` template-literal coercion for
# format_value, JSON.stringify for format_json. Both ultimately go through
# JS's Number::toString algorithm (ECMA-262) for any numeric value, which
# neither YAML's int/float distinction nor Python's own float repr
# reproduces on its own:
#   - YAML doesn't distinguish "5" from "5.0" the way a message should:
#     whether a value parses as float or int depends only on how it
#     happens to be spelled in the fixture file. JS has one number type,
#     so -1.0 and -1 must render identically ("-1").
#   - Python's `str()`/`repr()` never switches to exponential notation
#     below 1e16, and switches at a different threshold above it, so
#     naive str(value) diverges from JS for any value >= 1e21 or
#     (nonzero and) < 1e-6 in magnitude — see #254.
# _js_number_to_string reproduces ECMA-262's Number::toString exactly:
# fixed notation for magnitudes in [1e-6, 1e21) (9.99e20 fixed, 1e-6
# fixed, 1e21 and 1e-7 exponential — the spec's own boundary cases), an
# always-signed exponent (`e+21`, `e-7`) outside that range.


def _js_number_to_string(x):
    """Render float x the way JS's Number.prototype.toString() (and, by
    extension, `${x}` and JSON.stringify(x)) would, per ECMA-262's
    Number::toString algorithm."""
    if x != x:  # NaN
        return "NaN"
    if x == 0:
        return "0"  # covers -0.0 too: String(-0) === "0" in JS
    if x < 0:
        return "-" + _js_number_to_string(-x)
    if x == float("inf"):
        return "Infinity"

    # repr(x) is Python's own shortest-round-trip decimal string for x.
    # ECMA-262 requires that same "fewest digits that still round-trip"
    # digit sequence (its s/k), so parsing repr(x) through Decimal —
    # exact, no further rounding — recovers the spec's s/k/n without
    # re-deriving shortest-round-trip digit selection by hand.
    _sign, digits, exponent = Decimal(repr(x)).as_tuple()
    digits = list(digits)
    while len(digits) > 1 and digits[-1] == 0:
        digits.pop()
        exponent += 1
    k = len(digits)
    n = exponent + k
    s = "".join(str(d) for d in digits)

    if k <= n <= 21:
        return s + "0" * (n - k)
    if 0 < n <= 21:
        return s[:n] + "." + s[n:]
    if -6 < n <= 0:
        return "0." + "0" * (-n) + s
    mantissa = s[0] + ("." + s[1:] if k > 1 else "")
    exp = n - 1
    return f"{mantissa}e{'+' if exp >= 0 else '-'}{abs(exp)}"


def _js_json_value(value):
    """Render value the way JSON.stringify would: same structure as
    json.dumps, but numeric leaves go through _js_number_to_string
    instead of Python's own float formatting."""
    if value is None:
        return "null"
    if value is True:
        return "true"
    if value is False:
        return "false"
    if _is_number(value):
        return _js_number_to_string(float(value))
    if isinstance(value, dict):
        return "{" + ",".join(f"{json.dumps(k)}:{_js_json_value(v)}" for k, v in value.items()) + "}"
    if isinstance(value, list):
        return "[" + ",".join(_js_json_value(v) for v in value) + "]"
    return json.dumps(value)


def format_json(value):
    """JSON-render value the way JSON.stringify(value) would."""
    return _js_json_value(value)


def format_value(value):
    """Render value as it would appear inline in a violation message,
    i.e. the way JS's `${value}` template-literal coercion would."""
    if value is None:
        return "null"
    if value is True:
        return "true"
    if value is False:
        return "false"
    if _is_number(value):
        return _js_number_to_string(float(value))
    return str(value)


args = sys.argv[1:]
root_idx = args.index("--root") if "--root" in args else -1
root = Path(args[root_idx + 1]) if root_idx != -1 else Path.cwd()

# schema.json describes this script's own contract, not the tree under
# --root — always read it from this script's real location so a
# materialized selftest fixture directory (which never includes a copy of
# it) still validates against the one canonical schema.
HERE = Path(__file__).resolve().parent
with open(HERE / ".." / "test" / "fixtures" / "schema.json", encoding="utf8") as f:
    SCHEMA = json.load(f)

EDGE_MODES = SCHEMA["properties"]["edge_mode"]["enum"]
DOMAINS = SCHEMA["properties"]["domain"]["enum"]
REQUIRED_TOP_LEVEL = SCHEMA["required"]
HISTOGRAM_LITERAL = SCHEMA["histogramLiteral"]

# Per docs/testing/registry-and-fixtures.md: "(t, v) or (t, v, st); NaN and
# "stale" are literal tokens" — a sample's value may always be one of these
# two literal strings, regardless of domain, in place of a domain-shaped
# value.
SAMPLE_LITERAL_TOKENS = ["NaN", "stale"]


def _is_number(value):
    # Python bools are ints, but a YAML `true`/`false` is never a realistic
    # numeric fixture value, so isinstance(value, bool) is excluded
    # explicitly to avoid Python's bool-is-an-int surprising a caller that
    # means "a number".
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def list_yaml_files(directory):
    if not directory.exists():
        return []
    out = []

    def walk(d):
        for entry in d.iterdir():
            if entry.is_dir():
                walk(entry)
            elif entry.name.endswith(".yaml") or entry.name.endswith(".yml"):
                out.append(entry)

    walk(directory)
    return out


# A minimal, hand-written matcher against the small subset of JSON-Schema
# vocabulary schema.json actually uses (type + nested properties for plain
# objects) — not a general interpreter, just enough to let histogram literal
# validation walk schema.json's own histogramLiteral definition instead of
# hardcoding a second copy of its field list.
def matches_type(value, node):
    if not node or "type" not in node:
        return True
    t = node["type"]
    if t == "number":
        return _is_number(value)
    if t == "integer":
        return isinstance(value, int) and not isinstance(value, bool)
    if t == "string":
        return isinstance(value, str)
    if t == "array":
        return isinstance(value, list)
    if t == "object":
        if value is None or not isinstance(value, dict):
            return False
        if "properties" in node:
            for key, val in value.items():
                if key in node["properties"] and not matches_type(val, node["properties"][key]):
                    return False
        return True
    return True


def validate_histogram_literal(value, rel, idx):
    violations = []
    if not matches_type(value, HISTOGRAM_LITERAL):
        violations.append(f'{rel}: samples[{idx}] value is not a valid histogram literal object for domain "HISTOGRAM", got {format_json(value)}')
        return violations
    if isinstance(value, dict) and "schema" in value and "custom_bounds" in value:
        violations.append(f'{rel}: samples[{idx}] histogram literal cannot declare both "schema" and "custom_bounds" (mutually exclusive per schema.json)')
    return violations


def validate_sample_value(v, domain, rel, idx):
    if isinstance(v, str) and v in SAMPLE_LITERAL_TOKENS:
        return []
    if domain == "HISTOGRAM":
        return validate_histogram_literal(v, rel, idx)
    if not _is_number(v):
        return [f'{rel}: samples[{idx}] value must be a number for domain "{domain}" (or literal token {"/".join(SAMPLE_LITERAL_TOKENS)}), got {format_json(v)}']
    return []


def validate_fixture(doc, rel):
    violations = []

    for key in REQUIRED_TOP_LEVEL:
        if key not in doc:
            violations.append(f'{rel}: missing required field "{key}"')
    if violations:
        return violations  # structural — no point checking further

    if doc.get("edge_mode") not in EDGE_MODES:
        violations.append(f'{rel}: edge_mode "{format_value(doc.get("edge_mode"))}" is not one of {", ".join(EDGE_MODES)}')
    if doc.get("domain") not in DOMAINS:
        violations.append(f'{rel}: domain "{format_value(doc.get("domain"))}" is not one of {", ".join(DOMAINS)}')

    for field in ["window", "lookback"]:
        node = SCHEMA["properties"][field]
        if not matches_type(doc.get(field), node):
            violations.append(f'{rel}: {field} must be a {node["type"]}, got {format_json(doc.get(field))}')

    grid = doc.get("grid") or {}
    for field in ["start", "end", "step"]:
        if not _is_number(grid.get(field)):
            violations.append(f"{rel}: grid.{field} must be a number")
    if _is_number(grid.get("step")) and grid.get("step") <= 0:
        violations.append(f'{rel}: grid.step must be positive, got {format_value(grid.get("step"))}')

    samples = doc.get("samples")
    if not isinstance(samples, list):
        violations.append(f"{rel}: samples must be an array")
    else:
        for i, s in enumerate(samples):
            if not isinstance(s, list) or len(s) < 2 or len(s) > 3:
                violations.append(f"{rel}: samples[{i}] must be [t, v] or [t, v, st]")
                continue
            violations.extend(validate_sample_value(s[1], doc.get("domain"), rel, i))

    provenance = doc.get("provenance")
    if not provenance or not isinstance(provenance, dict) or not provenance.get("source"):
        violations.append(f"{rel}: provenance.source is required")

    # The inert-fixture rule: non-empty samples with no (or empty) expected
    # asserts nothing — a fixture with no assertion is dead weight, not a
    # fixture.
    has_samples = isinstance(samples, list) and len(samples) > 0
    expected = doc.get("expected")
    has_expected = isinstance(expected, list) and len(expected) > 0
    if has_samples and not has_expected:
        violations.append(f"{rel}: inert fixture — samples present but no expected")

    return violations


files = list_yaml_files(root / "test" / "fixtures")
violations = []

for file in files:
    rel = file.relative_to(root)
    try:
        with open(file, encoding="utf8") as f:
            doc = yaml.load(f, Loader=_ScientificNotationLoader)
    except yaml.YAMLError as e:
        violations.append(f"{rel}: could not parse YAML ({e})")
        continue
    violations.extend(validate_fixture(doc or {}, rel))

if violations:
    print("fixtures-validate: FAIL", file=sys.stderr)
    for v in violations:
        print(f"  {v}", file=sys.stderr)
    sys.exit(1)
print(f"fixtures-validate: PASS ({len(files)} fixture(s))")
