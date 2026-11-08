#!/usr/bin/env python3
"""Conservative, offline S1 dependency ratchet; exit 0=current, 1=delta, 2=bad input.

All conditional branches count. Unknown include operands and missing owned inputs
fail closed. This is a source firewall, not a compiler/AST or link-graph proof;
the future extracted Sim target needs those additional design acceptance gates.
"""
from __future__ import annotations

import argparse
from collections import Counter, deque
import heapq
import json
import os
from pathlib import Path, PurePosixPath
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
SCOPE = ROOT / "scripts/sim_include_scope.json"
BASELINE = ROOT / "scripts/sim_include_baseline.json"

# Standard and external libraries are terminal dependencies, never owned headers.
STANDARD = set("algorithm any array atomic barrier bit bitset cassert cctype cerrno cfenv cfloat charconv climits clocale cmath codecvt compare complex concepts condition_variable coroutine csetjmp csignal cstdarg cstddef cstdint cstdio cstdlib cstring cuchar cwchar cwctype deque exception execution expected filesystem format forward_list functional future initializer_list iomanip ios iosfwd iostream istream iterator latch limits list locale map mdspan memory memory_resource mutex new numbers numeric optional ostream print queue random ranges ratio regex scoped_allocator semaphore set shared_mutex source_location span sstream stack stdexcept stop_token streambuf string string_view strstream syncstream system_error thread tuple type_traits typeindex typeinfo unordered_map unordered_set utility valarray variant vector version assert.h ctype.h errno.h float.h limits.h locale.h math.h setjmp.h signal.h stdarg.h stddef.h stdint.h stdio.h stdlib.h string.h wchar.h wctype.h malloc.h intrin.h immintrin.h xmmintrin.h emmintrin.h".split())
EXTERNAL = {"zlib.h", "jpeglib.h", "png.h", "bzlib.h"}
LITERAL = r'R"(?P<delim>[^ ()\\\t\r\n]{0,16})\([\s\S]*?\)(?P=delim)"|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\''
COMMENTS_AND_LITERALS = re.compile(LITERAL + r"|//[^\n]*|/\*[\s\S]*?\*/")
LITERALS = re.compile(LITERAL)
INCLUDE = re.compile(r"^[ \t]*#[ \t]*include[ \t]+(.+?)[ \t]*$", re.M)  # not \s: it crosses newlines once literals are blanked
DEFINE = re.compile(r'^[ \t]*#[ \t]*define[ \t]+(\w+)[ \t]+([^\n]+)$', re.M)
TOKENS = re.compile(r"\b[A-Za-z_]\w*\b")


def blank(value):
    return re.sub(r"[^\n]", " ", value)


def source_text(source):
    source = re.sub(r"\\\r?\n", "", source)
    return COMMENTS_AND_LITERALS.sub(
        lambda m: blank(m[0]) if m[0].startswith(("//", "/*")) else m[0], source)


def header_rule(name):
    name = name.replace("\\", "/").casefold()
    parts = PurePosixPath(name).parts
    base = PurePosixPath(name).name
    if "camera" in base or base in {"dcamera.h"}:
        return "camera"
    if base in {"timing.h", "clock.h", "clock.hpp", "chrono", "ctime", "time.h", "sys/time.h"} or name == "sys/time.h":
        return "wall-clock"
    if "gameos" in parts or base.startswith(("gameos", "platform_")):
        return "gameos"
    if "rendercore" in parts or "renderworld" in parts:
        return "rendercore"
    if base.startswith("sdl"):
        return "sdl"
    if "vulkan" in parts or "gl" in parts or base in {"glew.h", "glad.h"}:
        return "gpu"
    if base in {"windows.h", "mmsystem.h", "profileapi.h", "sysinfoapi.h"}:
        return "windows"
    if base in {"userinput.h", "duserinput.h"}:
        return "input"
    if base in {"soundsys.h", "dsoundsys.h"}:
        return "audio"
    return None


def symbol_rule(token):
    if token in {"Camera", "GameCamera", "eye"}:
        return "camera"
    if token in {"Clock", "chrono", "system_clock", "steady_clock", "high_resolution_clock", "timeGetTime", "GetTickCount", "GetTickCount64", "QueryPerformanceCounter", "QueryPerformanceFrequency", "time", "clock", "gettimeofday", "clock_gettime", "gos_GetElapsedTime", "gos_GetTime"}:
        return "wall-clock"
    if token in {"rand", "srand", "gos_rand", "gos_srand"}:
        return "rng"
    if token in {"UserInput", "userInput"}:
        return "input"
    if token in {"SoundSystem", "soundSystem"}:
        return "audio"
    if token in {"RenderWorld", "RenderCore"}:
        return "rendercore"
    if token.startswith("SDL_"):
        return "sdl"
    if token.startswith("gos_"):
        return "gameos"
    if re.search(r"(?:wall.*(?:clock|time|now)|clock.*(?:shim|now)|(?:read|get).*wall)", token, re.I):
        return "wall-clock"
    return None


class InputError(ValueError):
    pass


class Sources:
    def __init__(self, root, files=None):
        self.root = Path(root).resolve()
        self.files = files
        self.cache = {}

    def exists(self, path):
        return path in self.files if self.files is not None else (self.root / path).is_file()

    def read(self, path):
        if path not in self.cache:
            if not self.exists(path):
                raise InputError(f"missing manifest/include input: {path}")
            self.cache[path] = self.files[path] if self.files is not None else (self.root / path).read_text(encoding="utf-8-sig")
        return self.cache[path]

    def normalize(self, path):
        resolved = (self.root / path.replace("\\", "/")).resolve()
        if not resolved.is_relative_to(self.root):
            raise InputError(f"include escapes source root: {path}")
        return resolved.relative_to(self.root).as_posix()

    def resolve(self, current, name, quoted, dirs):
        candidates = ([str(PurePosixPath(current).parent / name)] if quoted else [])
        candidates += [str(PurePosixPath(d) / name) for d in dirs]
        for candidate in candidates:
            candidate = self.normalize(candidate)
            if self.exists(candidate):
                return candidate
        return None


def explicit_pch_exclusions(sources, scope):
    excluded = scope.get("pch_excluded", [])
    if not isinstance(excluded, list) or any(not isinstance(tu, str) for tu in excluded):
        raise InputError("PCH exclusions require translation unit paths")
    if len(excluded) != len(set(excluded)) or not set(excluded) <= set(scope["translation_units"]):
        raise InputError("PCH exclusions must be distinct scoped translation units")
    if not excluded:
        return set()
    def source_key(path):
        return os.path.normcase(sources.normalize(path))

    excluded_keys = {source_key(path) for path in excluded}
    # Only unconditional literal declarations are evidence; dynamic CMake needs a new proof mechanism.
    text = sources.read("CMakeLists.txt")
    text = re.sub(r"#\[(=*)\[[\s\S]*?\]\1\]", "", text)
    text = re.sub(r'"(?:\\.|[^"\\])*"|#[^\n]*',
                  lambda m: m[0] if m[0].startswith('"') else "", text)
    nesting = []
    declarations = []
    previous_end = 0
    for command in re.finditer(r"(?im)^[ \t]*([a-z_]\w*)\s*\(", text):
        if command.start() < previous_end:
            continue
        name = command[1].lower()
        cursor, depth, quoted = command.end(), 1, False
        end = cursor
        while end < len(text) and depth:
            char = text[end]
            if char == '"' and (end == 0 or text[end - 1] != "\\"):
                quoted = not quoted
            if not quoted:
                if char == "(": depth += 1
                if char == ")": depth -= 1
            end += 1
        if depth:
            raise InputError("unterminated CMake command in PCH proof")
        previous_end = end
        args = [token.strip('"') for token in re.findall(r'"[^"\n]*"|[^\s]+', text[cursor:end - 1])]
        if name in {"if", "foreach", "while", "function", "macro", "block"}:
            nesting.append(name)
        elif name in {"endif", "endforeach", "endwhile", "endfunction", "endmacro", "endblock"}:
            if not nesting or name != "end" + nesting.pop():
                raise InputError("unbalanced CMake control block in PCH proof")
        elif name in {"set_source_files_properties", "set_property"} and "SKIP_PRECOMPILE_HEADERS" in args:
            property_index = args.index("SKIP_PRECOMPILE_HEADERS")
            value = args[property_index + 1:property_index + 2]
            if any("$" in arg for arg in args[:property_index]) and value != ["ON"]:
                raise InputError("opaque CMake PCH override cannot prove exclusions")
            if name != "set_source_files_properties" or "PROPERTIES" not in args:
                paths = args[1:args.index("PROPERTY")] if args[:1] == ["SOURCE"] and "PROPERTY" in args else []
                if {source_key(path) for path in paths if "$" not in path} & excluded_keys:
                    raise InputError("unsupported explicit PCH property declaration")
                continue
            split = args.index("PROPERTIES")
            paths = [source_key(path) if "$" not in path else path for path in args[:split]]
            properties = args[split + 1:]
            if set(paths) & excluded_keys:
                if nesting or properties != ["SKIP_PRECOMPILE_HEADERS", "ON"]:
                    raise InputError("PCH exclusions require unconditional SKIP_PRECOMPILE_HEADERS ON")
                declarations.append(paths)
    if nesting or len(declarations) != 1 or sorted(declarations[0]) != sorted(excluded_keys):
        raise InputError("PCH exclusions differ from root CMake source properties")
    return set(excluded)


def scan(sources, scope):
    if scope.get("schema_version") != 1:
        raise InputError("unsupported scope schema")
    tus = scope.get("translation_units", [])
    dirs = scope.get("include_dirs", [])
    if not tus or len(tus) != len(set(tus)) or not dirs:
        raise InputError("scope requires distinct translation units and include directories")
    for path in [*tus, *([scope["pch"]] if scope.get("pch") else [])]:
        if sources.normalize(path) != path:
            raise InputError(f"noncanonical manifest input: {path}")
        sources.read(path)
    skipped = set()
    if scope.get("pch_skip"):
        text = re.sub(r"#[^\n]*", "", sources.read(scope["pch_skip"]))
        skipped = set(re.findall(r'"([^"\n]+\.cpp)"', text))
    skipped.update(explicit_pch_exclusions(sources, scope))
    findings = []
    for tu in sorted(tus):
        queue = deque([(tu, [tu])])
        if scope.get("pch") and tu not in skipped:
            queue.append((scope["pch"], [tu, "<PCH>", scope["pch"]]))
        visited = set()
        routes = []
        witnesses = {}
        while queue:
            file, chain = queue.popleft()
            if file in visited:
                continue
            visited.add(file)
            text = source_text(sources.read(file))
            occurrences = Counter()

            def emit(rule, subject, evidence, suffix=None):
                identity = (rule, subject, evidence)
                occurrences[identity] += 1
                findings.append({"tu": tu, "file": file, "chain": chain + (suffix or []),
                                 "rule": rule, "subject": subject, "evidence": evidence,
                                 "occurrence": occurrences[identity]})
                tail = tuple([file] + (suffix or []))
                key = file, rule
                if key not in witnesses or (len(tail), tail) < (len(witnesses[key]), witnesses[key]):
                    witnesses[key] = tail

            defines = {}
            for macro, operand in DEFINE.findall(text):
                defines.setdefault(macro, set()).add(operand)
            for match in INCLUDE.finditer(text):
                raw_operand = match[1].strip()
                for operand in sorted(defines.get(raw_operand, {raw_operand})):
                    parsed = re.fullmatch(r'([<"])([^>"\n]+)[>"]', operand)
                    if not parsed:
                        raise InputError(f"unresolved include operand in {file}: {match[1]}")
                    name = parsed[2].replace("\\", "/")
                    target = sources.resolve(file, name, parsed[1] == '"', dirs)
                    rule = header_rule(target or name)
                    evidence = " ".join(match[0].split())
                    if rule:
                        emit(rule, name, evidence, [target or f"<{name}>"])
                        continue
                    if target:
                        if target.startswith("3rdparty/"):
                            continue
                        routes.append((file, chain, target, name, evidence))
                        queue.append((target, chain + [target]))
                    elif name not in STANDARD | EXTERNAL:
                        raise InputError(f"unresolved include in {file}: {name}")
            body = LITERALS.sub(lambda m: blank(m[0]), text)
            body = INCLUDE.sub(lambda m: blank(m[0]), body)
            for line in body.splitlines():
                evidence = " ".join(line.split())
                for match in TOKENS.finditer(line):
                    token = match[0]
                    # A stored member named time is simulation data, not the
                    # C wall-clock API. Keep calls and bare function addresses.
                    if (token == "time" and re.search(r"(?:\.|->)\s*$", line[:match.start()])
                            and not re.match(r"\s*\(", line[match.end():])):
                        continue
                    rule = symbol_rule(token)
                    if rule:
                        emit(rule, token, evidence)
            # External functions/pointers are opaque access paths without a pure interface contract.
            for match in re.finditer(r'\bextern\b[^;{}]*[;{]', body):
                declaration = " ".join(match[0].split())
                if "(" in declaration:
                    emit("extern-shim", "extern callable", declaration)
        # Each owned include edge needs its own witness: a second route must not
        # escape merely because its forbidden destination was already reachable.
        parents = {}
        for file, _, target, _, _ in routes:
            parents.setdefault(target, set()).add(file)
        pending = [(len(tail), tail, file, rule) for (file, rule), tail in witnesses.items()]
        heapq.heapify(pending)
        while pending:
            _, tail, file, rule = heapq.heappop(pending)
            if witnesses[file, rule] != tail:
                continue
            for parent in sorted(parents.get(file, set())):
                candidate = (parent,) + tail
                key = parent, rule
                if key not in witnesses or (len(candidate), candidate) < (len(witnesses[key]), witnesses[key]):
                    witnesses[key] = candidate
                    heapq.heappush(pending, (len(candidate), candidate, parent, rule))
        route_occurrences = Counter()
        for file, chain, target, name, evidence in routes:
            for (dependency, rule), tail in sorted(witnesses.items()):
                if dependency != target:
                    continue
                key = file, target, rule, evidence
                route_occurrences[key] += 1
                findings.append({"tu": tu, "file": file, "chain": chain + list(tail),
                                 "rule": rule, "subject": f"transitive {name}", "evidence": evidence,
                                 "occurrence": route_occurrences[key]})
    return sorted(findings, key=identity)


def identity(finding):
    # Witness paths can shorten after a removal without creating a new ingress.
    return json.dumps({k: v for k, v in finding.items() if k != "chain"}, sort_keys=True, ensure_ascii=True)


def delta(current, baseline):
    old = {identity(f): f for f in baseline}
    new = {identity(f): f for f in current}
    if len(old) != len(baseline) or len(new) != len(current):
        raise InputError("duplicate finding identities")
    return ([new[k] for k in sorted(new.keys() - old.keys())],
            [old[k] for k in sorted(old.keys() - new.keys())])


def summary(findings):
    return {"by_rule": dict(sorted(Counter(f["rule"] for f in findings).items())),
            "by_directory": dict(sorted(Counter(str(PurePosixPath(f["file"]).parent) for f in findings).items()))}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--scope", type=Path, default=SCOPE)
    parser.add_argument("--baseline", type=Path, default=BASELINE)
    parser.add_argument("--rebaseline", action="store_true", help="create once, thereafter remove findings only")
    args = parser.parse_args(argv)
    try:
        scope = json.loads(args.scope.read_text(encoding="utf-8"))
        current = scan(Sources(args.root), scope)
        if not args.baseline.exists():
            if not args.rebaseline:
                raise InputError("baseline missing; initial capture requires --rebaseline")
            old = []
            print(f"INITIAL baseline: +{len(current)} findings")
        else:
            value = json.loads(args.baseline.read_text(encoding="utf-8"))
            if value.get("schema_version") != 1 or value.get("scope") != scope:
                raise InputError("baseline schema/scope mismatch; scope changes require design review")
            old = value["violations"]
            added, removed = delta(current, old)
            print(f"delta: +{len(added)} -{len(removed)}")
            for label, entries in (("NEW", added), ("REMOVED", removed)):
                for entry in entries:
                    print(f"{label} {entry['rule']}: {' -> '.join(entry['chain'])}: {entry['subject']} | {entry['evidence']}")
            if added or (removed and not args.rebaseline):
                print("FAIL: new findings forbidden; removals require --rebaseline")
                return 1
        if args.rebaseline:
            value = {"schema_version": 1, "scope": scope, "violations": current}
            args.baseline.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8", newline="\n")
            print(f"wrote baseline: {args.baseline}")
        print(f"sim_include_firewall: PASS ({len(scope['translation_units'])} TUs, {len(current)} pinned violations)")
        print(json.dumps(summary(current), sort_keys=True))
        return 0
    except (OSError, ValueError, KeyError, TypeError) as exc:
        print(f"sim_include_firewall: INPUT ERROR: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
