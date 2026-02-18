#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
code_audit_identifiers.py

Two modes:
  1) near    : find identifiers within edit distance <= k of a needle
  2) compare : compare counts of two identifiers (A vs B) per file / overall

Design goals:
- Fast enough for large codebases (small k typical: 1..3)
- Not a full C++ parser, but:
  * tokenizes identifiers via regex
  * (optional, default on) masks comments + string/char literals to avoid noise
- Optional multiprocessing for speed on big trees (Pool)  [see Python docs]
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import re
from dataclasses import dataclass, asdict
from pathlib import Path
from typing import Dict, List, Tuple, Optional, Iterable

# Conservative ASCII identifier tokenization (common in C/C++ codebases)
IDENT_RE = re.compile(r"\b[_A-Za-z][_A-Za-z0-9]*\b")


# ---------------------------
# Masking comments/strings
# ---------------------------

def mask_cpp_comments_and_strings(src: str) -> str:
    """
    Best-effort masking of:
      - // line comments
      - /* block comments */
      - "string literals" with escapes
      - 'char literals' with escapes
      - raw string literals: R"delim( ... )delim"

    Returns a string of same length:
      - masked regions replaced with spaces
      - newlines preserved (line numbers remain valid)
    """
    n = len(src)
    out = list(src)  # we will overwrite masked chars with ' '
    i = 0

    def put_spaces(a: int, b: int) -> None:
        # replace [a,b) with spaces except keep '\n'
        for k in range(a, b):
            if out[k] != "\n":
                out[k] = " "

    while i < n:
        c = src[i]

        # line comment //
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            j = i
            # mask until end of line
            i += 2
            while i < n and src[i] != "\n":
                i += 1
            put_spaces(j, i)
            continue

        # block comment /* ... */
        if c == "/" and i + 1 < n and src[i + 1] == "*":
            j = i
            i += 2
            while i + 1 < n and not (src[i] == "*" and src[i + 1] == "/"):
                i += 1
            i = min(n, i + 2)  # include closing */
            put_spaces(j, i)
            continue

        # normal string literal "..."
        if c == '"':
            j = i
            i += 1
            while i < n:
                if src[i] == "\\" and i + 1 < n:
                    i += 2
                    continue
                if src[i] == '"':
                    i += 1
                    break
                i += 1
            put_spaces(j, i)
            continue

        # char literal '...'
        if c == "'":
            j = i
            i += 1
            while i < n:
                if src[i] == "\\" and i + 1 < n:
                    i += 2
                    continue
                if src[i] == "'":
                    i += 1
                    break
                i += 1
            put_spaces(j, i)
            continue

        # raw string literal: R"delim( ... )delim"
        # minimal detection: starts with R"
        if c == "R" and i + 1 < n and src[i + 1] == '"':
            j = i
            i += 2  # after R"
            # delimiter up to '(' (can be empty)
            delim_start = i
            while i < n and src[i] != "(":
                # raw delimiter cannot contain spaces, backslash, parentheses per standard,
                # but we won't enforce; we just search '('.
                i += 1
            if i >= n:
                # malformed, mask rest
                put_spaces(j, n)
                break
            delim = src[delim_start:i]
            i += 1  # skip '('
            # find closing sequence: ')' + delim + '"'
            close_seq = ")" + delim + '"'
            close_pos = src.find(close_seq, i)
            if close_pos == -1:
                put_spaces(j, n)
                break
            i = close_pos + len(close_seq)
            put_spaces(j, i)
            continue

        i += 1

    return "".join(out)


# ---------------------------
# Edit distance (bounded)
# ---------------------------

def bounded_levenshtein(a: str, b: str, k: int) -> int:
    """
    Levenshtein distance with cutoff:
      returns k+1 if distance > k

    Uses banded DP + early exit; efficient for small k (1..3 typical).
    """
    if a == b:
        return 0
    na, nb = len(a), len(b)
    if abs(na - nb) > k:
        return k + 1

    # Make a the shorter one (slight benefit)
    if na > nb:
        a, b = b, a
        na, nb = nb, na

    big = k + 1
    prev = list(range(nb + 1))

    for i in range(1, na + 1):
        start = max(1, i - k)
        end = min(nb, i + k)

        cur = [big] * (nb + 1)
        cur[0] = i

        row_min = big
        ai = a[i - 1]
        for j in range(start, end + 1):
            cost = 0 if ai == b[j - 1] else 1
            cur[j] = min(
                prev[j] + 1,        # deletion
                cur[j - 1] + 1,     # insertion
                prev[j - 1] + cost  # substitution
            )
            if cur[j] < row_min:
                row_min = cur[j]

        if row_min > k:
            return big

        prev = cur

    return prev[nb] if prev[nb] <= k else big


# ---------------------------
# Filesystem helpers
# ---------------------------

def should_skip_dir(dirname: str, patterns: List[str]) -> bool:
    for p in patterns:
        if p.endswith("*"):
            if dirname.startswith(p[:-1]):
                return True
        else:
            if dirname == p:
                return True
    return False


def iter_source_files(root: Path, exts: List[str], skip_dirs: List[str]) -> List[Path]:
    files: List[Path] = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if not should_skip_dir(d, skip_dirs)]
        for fn in filenames:
            p = Path(dirpath) / fn
            if p.suffix in exts:
                files.append(p)
    return files


def relpath_str(p: Path, root: Path) -> str:
    try:
        return str(p.relative_to(root))
    except Exception:
        return str(p)


def path_matches_any_glob(rel: str, globs: List[str]) -> bool:
    # simple fnmatch-like behavior via Path.match
    rp = Path(rel)
    for g in globs:
        if rp.match(g):
            return True
    return False


# ---------------------------
# Data structures
# ---------------------------

@dataclass
class Occurrence:
    path: str
    line: int
    col: int


@dataclass
class NearTokenAggregate:
    token: str
    count: int
    locs: List[Occurrence]


@dataclass
class CompareFileRow:
    path: str
    count_a: int
    count_b: int
    diff: int
    locs_a: List[Occurrence]
    locs_b: List[Occurrence]


# ---------------------------
# Tokenization per line
# ---------------------------

def iter_identifiers_with_pos(text: str) -> Iterable[Tuple[str, int, int]]:
    """
    Yields (token, line_no, col_1based) for each identifier match.
    """
    for lineno, line in enumerate(text.splitlines(), start=1):
        for m in IDENT_RE.finditer(line):
            yield m.group(0), lineno, m.start() + 1


def load_text(path: Path, encoding: str) -> str:
    return path.read_text(encoding=encoding, errors="replace")


# ---------------------------
# NEAR mode worker
# ---------------------------

def scan_file_near(
    path: Path,
    root: Path,
    needle: str,
    max_dist: int,
    case_insensitive: bool,
    same_first_char: bool,
    ignore_tokens: List[str],
    max_locs_per_token: int,
    mask: bool,
    encoding: str
) -> Dict[int, Dict[str, NearTokenAggregate]]:
    """
    Returns:
      dist -> token -> aggregate(count, locs-limited)
    """
    try:
        text = load_text(path, encoding)
    except Exception:
        return {}

    if mask:
        text = mask_cpp_comments_and_strings(text)

    ncmp = needle.lower() if case_insensitive else needle
    ignore_set = set(t.lower() if case_insensitive else t for t in ignore_tokens)

    out: Dict[int, Dict[str, NearTokenAggregate]] = {}

    for tok, line, col in iter_identifiers_with_pos(text):
        tcmp = tok.lower() if case_insensitive else tok

        if tcmp in ignore_set:
            continue

        if same_first_char:
            if not tcmp or not ncmp or tcmp[0] != ncmp[0]:
                continue

        if abs(len(tcmp) - len(ncmp)) > max_dist:
            continue

        d = bounded_levenshtein(tcmp, ncmp, max_dist)
        if d <= max_dist:
            bucket = out.setdefault(d, {})
            agg = bucket.get(tok)
            if agg is None:
                agg = NearTokenAggregate(token=tok, count=0, locs=[])
                bucket[tok] = agg
            agg.count += 1

            if max_locs_per_token == 0 or len(agg.locs) < max_locs_per_token:
                agg.locs.append(Occurrence(path=relpath_str(path, root), line=line, col=col))

    return out


# ---------------------------
# COMPARE mode worker
# ---------------------------

def scan_file_compare(
    path: Path,
    root: Path,
    a: str,
    b: str,
    case_insensitive: bool,
    locs_per_file: int,
    mask: bool,
    encoding: str
) -> CompareFileRow:
    try:
        text = load_text(path, encoding)
    except Exception:
        return CompareFileRow(path=relpath_str(path, root), count_a=0, count_b=0, diff=0, locs_a=[], locs_b=[])

    if mask:
        text = mask_cpp_comments_and_strings(text)

    acmp = a.lower() if case_insensitive else a
    bcmp = b.lower() if case_insensitive else b

    ca = 0
    cb = 0
    la: List[Occurrence] = []
    lb: List[Occurrence] = []

    rp = relpath_str(path, root)

    for tok, line, col in iter_identifiers_with_pos(text):
        tcmp = tok.lower() if case_insensitive else tok
        if tcmp == acmp:
            ca += 1
            if locs_per_file == 0 or len(la) < locs_per_file:
                la.append(Occurrence(path=rp, line=line, col=col))
        elif tcmp == bcmp:
            cb += 1
            if locs_per_file == 0 or len(lb) < locs_per_file:
                lb.append(Occurrence(path=rp, line=line, col=col))

    return CompareFileRow(path=rp, count_a=ca, count_b=cb, diff=ca - cb, locs_a=la, locs_b=lb)


# ---------------------------
# Pretty printing helpers
# ---------------------------

def print_kv(title: str, value: str) -> None:
    print(f"{title:<18} {value}")


def format_locs(locs: List[Occurrence], max_items: int = 5) -> str:
    shown = locs[:max_items]
    s = ", ".join(f"{o.line}:{o.col}" for o in shown)
    if len(locs) > max_items:
        s += f", …(+{len(locs)-max_items})"
    return s


# ---------------------------
# Main
# ---------------------------

def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(
        description="Audit C/C++ identifier usage: near-miss search and A/B count comparison."
    )

    # Global options
    ap.add_argument("--root", type=str, default=".", help="Root directory of the codebase")
    ap.add_argument(
        "--ext",
        nargs="*",
        default=[".h", ".hpp", ".hh", ".hxx", ".c", ".cc", ".cpp", ".cxx", ".ipp", ".inl"],
        help="File extensions to scan",
    )
    ap.add_argument(
        "--skip-dirs",
        nargs="*",
        default=[".git", "build", "dist", "out", "node_modules", "venv", ".venv", "cmake-build*", ".idea", ".vs"],
        help="Directory names to skip (supports prefix wildcard via *)",
    )
    ap.add_argument("--encoding", type=str, default="utf-8", help="File encoding used to read sources")
    ap.add_argument("--jobs", type=int, default=1, help="Parallel processes (>=2 uses multiprocessing Pool)")
    ap.add_argument(
        "--no-mask",
        action="store_true",
        help="Do NOT mask comments/strings (by default they are masked to reduce noise)",
    )
    ap.add_argument(
        "--only-glob",
        nargs="*",
        default=[],
        help="Restrict to files whose relative path matches any glob (e.g. 'src/**', '**/Foo*.cpp')",
    )
    ap.add_argument(
        "--file",
        type=str,
        default="",
        help="Restrict to a single file path (relative to root or absolute)",
    )

    sub = ap.add_subparsers(dest="cmd", required=True)

    # near
    p_near = sub.add_parser("near", help="Find near-miss identifiers by edit distance buckets")
    p_near.add_argument("--needle", required=True, help="Target identifier (e.g. Root)")
    p_near.add_argument("--max-dist", type=int, default=2, help="Maximum edit distance (e.g. 3)")
    p_near.add_argument("--case-insensitive", action="store_true", help="Compare case-insensitively (Root == root)")
    p_near.add_argument(
        "--same-first-char",
        action="store_true",
        help="Speed filter: only consider tokens with same first character as needle",
    )
    p_near.add_argument(
        "--ignore-token",
        nargs="*",
        default=[],
        help="Tokens to ignore (space-separated list)",
    )
    p_near.add_argument("--max-locs-per-token", type=int, default=20, help="0 = unlimited")
    p_near.add_argument("--json", type=str, default="", help="Write full results to JSON")
    p_near.add_argument("--csv", type=str, default="", help="Write summary to CSV")

    # compare
    p_cmp = sub.add_parser("compare", help="Compare counts of two identifiers (A vs B) per file")
    p_cmp.add_argument("--a", required=True, help="Identifier A (baseline, e.g. Elefant)")
    p_cmp.add_argument("--b", required=True, help="Identifier B (candidate, e.g. Giraffe)")
    p_cmp.add_argument("--case-insensitive", action="store_true", help="Compare case-insensitively")
    p_cmp.add_argument(
        "--expect",
        choices=["equal", "a>=b", "b>=a", "ratio"],
        default="equal",
        help="Expectation to validate per-file and overall",
    )
    p_cmp.add_argument(
        "--ratio",
        type=float,
        default=1.0,
        help="Expected A/B ratio if --expect ratio (default 1.0)",
    )
    p_cmp.add_argument(
        "--tolerance",
        type=int,
        default=0,
        help="Allowed absolute difference for 'equal' (0 means exact equality)",
    )
    p_cmp.add_argument(
        "--locs-per-file",
        type=int,
        default=5,
        help="How many locations to store per file per token (0 = unlimited)",
    )
    p_cmp.add_argument(
        "--only-mismatches",
        action="store_true",
        help="Print only files that violate the expectation",
    )
    p_cmp.add_argument(
        "--max-rows",
        type=int,
        default=200,
        help="Limit printed rows (still computes all). 0 = unlimited",
    )
    p_cmp.add_argument("--json", type=str, default="", help="Write full results to JSON")
    p_cmp.add_argument("--csv", type=str, default="", help="Write per-file rows to CSV")

    return ap


def collect_files(args, root: Path) -> List[Path]:
    if args.file:
        fp = Path(args.file)
        if not fp.is_absolute():
            fp = root / fp
        return [fp] if fp.exists() else []

    files = iter_source_files(root, args.ext, args.skip_dirs)

    if args.only_glob:
        filtered: List[Path] = []
        for f in files:
            rel = relpath_str(f, root)
            if path_matches_any_glob(rel, args.only_glob):
                filtered.append(f)
        files = filtered

    return files


def check_expectation(row: CompareFileRow, expect: str, ratio: float, tolerance: int) -> bool:
    if expect == "equal":
        return abs(row.diff) <= tolerance
    if expect == "a>=b":
        return row.diff >= -tolerance
    if expect == "b>=a":
        return row.diff <= tolerance
    # ratio: check A approx ratio * B (integer counts -> compare cross-mult)
    # Use tolerance as allowed absolute diff on A - ratio*B rounded? We'll do cross-mult with float:
    target = ratio * row.count_b
    return abs(row.count_a - target) <= tolerance
    # (If you want stricter ratio checking: use integer scaling and relative tolerance.)


def cmd_near(args) -> int:
    root = Path(args.root).resolve()
    files = collect_files(args, root)
    mask = not args.no_mask

    print_kv("Root:", str(root))
    print_kv("Mode:", "near")
    print_kv("Needle:", args.needle)
    print_kv("max-dist:", str(args.max_dist))
    print_kv("case-ins:", str(args.case_insensitive))
    print_kv("mask:", str(mask))
    print_kv("files:", str(len(files)))
    print("")

    # Aggregate: dist -> token -> (count, locs)
    agg: Dict[int, Dict[str, NearTokenAggregate]] = {}

    if args.jobs and args.jobs >= 2:
        # multiprocessing
        from multiprocessing import Pool  # documented in stdlib :contentReference[oaicite:2]{index=2}

        work = [
            (f, root, args.needle, args.max_dist, args.case_insensitive, args.same_first_char,
             args.ignore_token, args.max_locs_per_token, mask, args.encoding)
            for f in files
        ]

        with Pool(processes=args.jobs) as pool:
            for partial in pool.starmap(scan_file_near, work):
                for d, token_map in partial.items():
                    bucket = agg.setdefault(d, {})
                    for tok, a in token_map.items():
                        cur = bucket.get(tok)
                        if cur is None:
                            bucket[tok] = NearTokenAggregate(tok, a.count, list(a.locs))
                        else:
                            cur.count += a.count
                            if args.max_locs_per_token == 0:
                                cur.locs.extend(a.locs)
                            else:
                                remain = args.max_locs_per_token - len(cur.locs)
                                if remain > 0:
                                    cur.locs.extend(a.locs[:remain])
    else:
        for f in files:
            partial = scan_file_near(
                f, root, args.needle, args.max_dist, args.case_insensitive,
                args.same_first_char, args.ignore_token, args.max_locs_per_token, mask, args.encoding
            )
            for d, token_map in partial.items():
                bucket = agg.setdefault(d, {})
                for tok, a in token_map.items():
                    cur = bucket.get(tok)
                    if cur is None:
                        bucket[tok] = NearTokenAggregate(tok, a.count, list(a.locs))
                    else:
                        cur.count += a.count
                        if args.max_locs_per_token == 0:
                            cur.locs.extend(a.locs)
                        else:
                            remain = args.max_locs_per_token - len(cur.locs)
                            if remain > 0:
                                cur.locs.extend(a.locs[:remain])

    # Print buckets
    total = sum(a.count for d in agg.values() for a in d.values())
    print_kv("Total hits:", str(total))
    print("")

    for dist in range(0, args.max_dist + 1):
        token_map = agg.get(dist, {})
        if not token_map:
            continue
        print("=" * 90)
        print(f"EDIT DISTANCE = {dist} | unique tokens: {len(token_map)} | hits: {sum(a.count for a in token_map.values())}")
        print("=" * 90)

        for tok, a in sorted(token_map.items(), key=lambda kv: (-kv[1].count, kv[0])):
            print(f"\n{tok} | count={a.count}")
            for o in a.locs:
                print(f"  - {o.path}:{o.line}:{o.col}")
        print("")

    # Export
    if args.json:
        out = {
            "root": str(root),
            "mode": "near",
            "needle": args.needle,
            "max_dist": args.max_dist,
            "case_insensitive": args.case_insensitive,
            "masked": mask,
            "results": {
                str(d): {
                    tok: {"count": a.count, "locs": [asdict(o) for o in a.locs]}
                    for tok, a in agg.get(d, {}).items()
                }
                for d in range(0, args.max_dist + 1)
            },
        }
        Path(args.json).write_text(json.dumps(out, indent=2, ensure_ascii=False), encoding="utf-8")
        print(f"Wrote JSON: {args.json}")

    if args.csv:
        rows = []
        for d in range(0, args.max_dist + 1):
            for tok, a in agg.get(d, {}).items():
                rows.append((d, tok, a.count))
        rows.sort(key=lambda r: (r[0], -r[2], r[1]))

        with open(args.csv, "w", newline="", encoding="utf-8") as f:
            w = csv.writer(f)
            w.writerow(["distance", "token", "count"])
            w.writerows(rows)
        print(f"Wrote CSV: {args.csv}")

    return 0


def cmd_compare(args) -> int:
    root = Path(args.root).resolve()
    files = collect_files(args, root)
    mask = not args.no_mask

    print_kv("Root:", str(root))
    print_kv("Mode:", "compare")
    print_kv("A:", args.a)
    print_kv("B:", args.b)
    print_kv("expect:", args.expect)
    print_kv("tolerance:", str(args.tolerance))
    if args.expect == "ratio":
        print_kv("ratio A/B:", str(args.ratio))
    print_kv("case-ins:", str(args.case_insensitive))
    print_kv("mask:", str(mask))
    print_kv("files:", str(len(files)))
    print("")

    rows: List[CompareFileRow] = []

    if args.jobs and args.jobs >= 2:
        from multiprocessing import Pool  # :contentReference[oaicite:3]{index=3}
        work = [
            (f, root, args.a, args.b, args.case_insensitive, args.locs_per_file, mask, args.encoding)
            for f in files
        ]
        with Pool(processes=args.jobs) as pool:
            rows = pool.starmap(scan_file_compare, work)
    else:
        for f in files:
            rows.append(scan_file_compare(
                f, root, args.a, args.b, args.case_insensitive, args.locs_per_file, mask, args.encoding
            ))

    # Overall totals
    total_a = sum(r.count_a for r in rows)
    total_b = sum(r.count_b for r in rows)

    overall_row = CompareFileRow(path="<TOTAL>", count_a=total_a, count_b=total_b, diff=total_a - total_b, locs_a=[], locs_b=[])
    overall_ok = check_expectation(overall_row, args.expect, args.ratio, args.tolerance)

    print_kv("TOTAL A:", str(total_a))
    print_kv("TOTAL B:", str(total_b))
    print_kv("TOTAL diff:", str(total_a - total_b))
    print_kv("TOTAL ok?:", "YES" if overall_ok else "NO")
    print("")

    # Determine mismatches per file
    mismatches: List[CompareFileRow] = []
    for r in rows:
        if not check_expectation(r, args.expect, args.ratio, args.tolerance):
            mismatches.append(r)

    print_kv("Mismatching files:", str(len(mismatches)))
    print("")

    # Sort rows: biggest absolute diff first, then path
    rows_sorted = sorted(rows, key=lambda r: (-abs(r.diff), r.path))
    mism_sorted = sorted(mismatches, key=lambda r: (-abs(r.diff), r.path))

    to_print = mism_sorted if args.only_mismatches else rows_sorted
    if args.max_rows != 0:
        to_print = to_print[:args.max_rows]

    # Print header-ish
    print(f"{'FILE':<60} {'A':>8} {'B':>8} {'DIFF':>8}  LOCS(A) / LOCS(B)")
    print("-" * 110)

    for r in to_print:
        la = format_locs(r.locs_a, max_items=min(5, args.locs_per_file if args.locs_per_file else 5))
        lb = format_locs(r.locs_b, max_items=min(5, args.locs_per_file if args.locs_per_file else 5))
        print(f"{r.path:<60} {r.count_a:>8} {r.count_b:>8} {r.diff:>8}  {la} / {lb}")

    print("")

    # Export
    if args.json:
        out = {
            "root": str(root),
            "mode": "compare",
            "a": args.a,
            "b": args.b,
            "expect": args.expect,
            "tolerance": args.tolerance,
            "ratio": args.ratio,
            "case_insensitive": args.case_insensitive,
            "masked": mask,
            "totals": {"a": total_a, "b": total_b, "diff": total_a - total_b, "ok": overall_ok},
            "rows": [asdict(r) for r in rows],
            "mismatches": [asdict(r) for r in mismatches],
        }
        Path(args.json).write_text(json.dumps(out, indent=2, ensure_ascii=False), encoding="utf-8")
        print(f"Wrote JSON: {args.json}")

    if args.csv:
        with open(args.csv, "w", newline="", encoding="utf-8") as f:
            w = csv.writer(f)
            w.writerow(["path", "count_a", "count_b", "diff"])
            for r in rows_sorted:
                w.writerow([r.path, r.count_a, r.count_b, r.diff])
        print(f"Wrote CSV: {args.csv}")

    return 0


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()

    if args.cmd == "near":
        return cmd_near(args)
    if args.cmd == "compare":
        return cmd_compare(args)

    return 2


if __name__ == "__main__":
    raise SystemExit(main())
