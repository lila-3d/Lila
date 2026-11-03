"""Lists file-local names that two .cpp files of the same target both define.

Unity builds paste a target's sources into shared translation units, so two files that each define the same
internal-linkage name (anonymous namespace, static, namespace-scope const/constexpr, enumerators) break the build as
soon as a chunk boundary puts them together. Targets come from a build dir's compile_commands.json, else each source
directory counts as one. It cannot see a file-local name meeting a public one or a leaked using-directive; only a
compile shows those.

    python scripts/unity_clashes.py [dir ...]           default: src extensions tests
    python scripts/unity_clashes.py --build <dir>       read targets from that build dir
    python scripts/unity_clashes.py --verbose           print each definition line too
Exit code 1 when any clash is found.
"""

import collections
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

TYPE_DEF = re.compile(r"^(?:template\s*<.*>\s*)?(?:struct|class|union)\s+(?:LILA_\w+\s+)?(\w+)\b(?!.*;\s*$)")
ENUM_DEF = re.compile(r"^enum(\s+class|\s+struct)?\s+(\w+)\b(?!.*;\s*$)")
ALIAS = re.compile(r"^using\s+(\w+)\s*=")
CONST_VAR = re.compile(r"^(?:static\s+|inline\s+|thread_local\s+)*(?:constexpr|const)\b[^(=]*?\b(\w+)\s*(?:\[[^\]]*\])*\s*(?:=|\{)")
CONST_CTOR = re.compile(r"^(?:static\s+|inline\s+)*(?:constexpr|const)\s+[\w:<>,\s]+?\s(\w+)\s*\((?!.*\)\s*(?:const\s*)?\{?\s*$).*\);\s*$")
MUTABLE_VAR = re.compile(r"^(?!return\b|using\b|typedef\b|friend\b|template\b)[\w:<>,\s\*&]+?[\s\*&](\w+)\s*(?:\[[^\]]*\])*\s*(?:=.*|\{.*\})?;\s*$")
STATIC_VAR = re.compile(r"^static\s+(?!constexpr\b|const\b)[\w:<>,\s\*&]+?\b(\w+)\s*(?:\[[^\]]*\])*\s*(?:=|\{|;)\s*")
FUNC = re.compile(r"^(?:template\s*<.*>\s*)?(?:\[\[\w+\]\]\s*)?(?:static\s+|inline\s+|constexpr\s+)*[\w:<>,\*&\s]+?[\s\*&](\w+)\s*\(")
KEYWORDS = {"if", "for", "while", "return", "switch", "operator", "sizeof", "decltype", "alignas", "static_assert", "catch", "else"}


def code_lines(path):
    """Yields (line number, code with comments and literals blanked, raw line)."""
    text = open(path, encoding="utf-8", errors="replace").read()
    in_comment = False
    for no, raw in enumerate(text.splitlines(), 1):
        line = raw
        if in_comment:
            if "*/" not in line:
                continue
            line = line.split("*/", 1)[1]
            in_comment = False
        line = re.sub(r'R"(\w*)\(.*?\)\1"', '""', line)
        line = re.sub(r'"(?:\\.|[^"\\])*"', '""', line)
        line = re.sub(r"'(?:\\.|[^'\\])+'", "''", line)
        line = re.sub(r"/\*.*?\*/", "", line)
        if "/*" in line:
            line, in_comment = line.split("/*", 1)[0], True
        line = line.split("//")[0].rstrip()
        if line.strip() and not line.lstrip().startswith("#"):
            yield no, line, raw


def scan(path):
    """Yields (namespace, name, line, text) for internal-linkage definitions at namespace scope."""
    stack = []  # ("ns", name, anonymous), ("enum", namespace) for an unscoped enum body, ("block",)
    pending = None  # what the next "{" opens when a declaration line ends without one
    for no, s, raw in code_lines(path):
        at_ns = all(e[0] == "ns" for e in stack)
        ns = "::".join(e[1] for e in stack if e[0] == "ns" and e[1])
        anon = any(e[0] == "ns" and e[2] for e in stack)
        opener = None
        if stack and stack[-1][0] == "enum":
            for part in re.sub(r"[{};]", "", s).split(","):
                m = re.match(r"^\s*([A-Za-z_]\w*)\s*(?:=.*)?$", part)
                if m:
                    yield stack[-1][1], m.group(1), no, raw.strip()
        m = re.match(r"^\s*(?:inline\s+)?namespace\s*([\w:]*)\s*(\{)?", s)
        if m and "=" not in s:
            opener = ("ns", m.group(1), m.group(1) == "")
        elif at_ns and not s[0].isspace():
            name = None
            head = s.split("(")[0].split("=")[0]
            qualified = re.search(r"::~?\w+\s*$", head)
            if (m := ENUM_DEF.match(s)) and not qualified:
                name = m.group(2)
                if not m.group(1):
                    opener = ("enum", ns)
            elif (m := TYPE_DEF.match(s)) and not qualified:
                name = m.group(1)
            elif (m := ALIAS.match(s)) and anon:
                name = m.group(1)
            elif (m := CONST_VAR.match(s)) and not qualified and "extern" not in s:
                name = m.group(1)
            elif (m := CONST_CTOR.match(s)) and not qualified:
                name = m.group(1)
            elif (m := STATIC_VAR.match(s)) and not qualified and "(" not in head:
                name = m.group(1)
            elif anon and not re.match(r"(?:namespace|using|extern)\b", s) and (m := MUTABLE_VAR.match(s)) and not qualified and "(" not in re.split(r"[={]", s)[0]:
                name = m.group(1)
            elif (m := FUNC.match(s)) and not qualified and not s.endswith(";"):
                if anon or re.match(r"^(?:template\s*<.*>\s*)?static\b", s):
                    name = m.group(1)
            if name and name not in KEYWORDS:
                yield ns, name, no, raw.strip()
        for ch in s:
            if ch == "{":
                if opener:
                    stack.append(opener)
                    opener = None
                elif pending:
                    stack.append(pending)
                    pending = None
                else:
                    stack.append(("block",))
            elif ch == "}" and stack:
                stack.pop()
        if opener and not s.rstrip().endswith(";"):
            pending = opener


def load_targets(builds):
    """Maps each source (repo-relative) to its CMake target, from the first build dir's compile_commands.json."""
    for build in builds:
        path = os.path.join(ROOT, build, "compile_commands.json")
        if not os.path.exists(path):
            continue
        targets = {}
        for entry in json.load(open(path, encoding="utf-8")):
            m = re.search(r"CMakeFiles[\\/]([^\\/]+)\.dir[\\/]", entry.get("output", ""))
            if m and not m.group(1).startswith("cmake_pch"):
                rel = os.path.relpath(entry["file"], ROOT).replace("\\", "/")
                targets[rel] = m.group(1)
        return targets
    return {}


TARGETS = {}


def target_of(rel):
    """The CMake target when known, else the directory: unity chunks never span targets."""
    return TARGETS.get(rel) or rel.rsplit("/", 1)[0]


def collect(roots):
    defs = collections.defaultdict(lambda: collections.defaultdict(list))
    for r in roots:
        for d, ds, fs in os.walk(os.path.join(ROOT, r)):
            ds[:] = [x for x in ds if not x.startswith((".", "build"))]
            for f in sorted(fs):
                if f.endswith(".cpp"):
                    p = os.path.join(d, f)
                    rel = os.path.relpath(p, ROOT).replace("\\", "/")
                    for ns, name, no, text in scan(p):
                        defs[target_of(rel)][(ns, name)].append((rel, no, text))
    return defs


def main():
    argv = sys.argv[1:]
    builds = ["build-nounity", "build", "build-night"]
    if "--build" in argv:
        i = argv.index("--build")
        builds = [argv[i + 1]]
        del argv[i:i + 2]
    args = [a for a in argv if not a.startswith("--")]
    verbose = "--verbose" in argv
    TARGETS.update(load_targets(builds))
    defs = collect(args or ["src", "extensions", "tests"])
    total = 0
    for target in sorted(defs):
        clashes = {k: v for k, v in defs[target].items() if len({f for f, _, _ in v}) > 1}
        if not clashes:
            continue
        print(f"## {target} ({len(clashes)})")
        for (ns, name), where in sorted(clashes.items(), key=lambda kv: (-len({f for f, _, _ in kv[1]}), kv[0][1])):
            files = sorted({f.rsplit("/", 1)[-1] for f, _, _ in where})
            print(f"  {name:28} {len(files):3}  {' '.join(files)}")
            if verbose:
                for f, no, text in where:
                    print(f"      {f}:{no}: {text[:140]}")
        total += len(clashes)
    print(f"TOTAL {total}")
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main())
