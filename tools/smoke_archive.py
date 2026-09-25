#!/usr/bin/env python3
"""S0 smoke: offline integrity check of this archive worktree.

Verifies that the sealed evidence this repository is built around is still
present and byte-exact, that the 2026-09-25 de-duplication really removed the
redundant copies, and that no relative Markdown link dangles.

This is pure stdlib, CPU-only, and runs in well under a second. It performs no
compilation, no simulation and no GPU work.

Usage:
    python3 tools/smoke_archive.py [REPO_ROOT]
"""
import hashlib
import json
import pathlib
import re
import sys

REPO = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
ARCHIVE = REPO / "archive/hbserve-gddr-p2-rerun-20260922-r1"
ADAPTER = ARCHIVE / "control/structure-adapter"

# Canonical sweep comparison base. See receipts/README.md.
CANON_DATA_SHA = "58eabb56d9f20c9bf6f49589ccaa209ac4468d326ec66fb0618f76b2a6031e19"

# Flat copies removed on 2026-09-25 as byte-identical duplicates of result-r1/.
REMOVED_DUPES = ("receipts/data.json", "receipts/receipt.json", "receipts/report.md")

_ok = 0
_fail = 0


def check(label, cond, detail=""):
    global _ok, _fail
    if cond:
        _ok += 1
        print(f"  PASS  {label}")
    else:
        _fail += 1
        print(f"  FAIL  {label}" + (f"  -- {detail}" if detail else ""))


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    print("== 1. sealed binaries still match the receipt SHA pins ==")
    receipt_path = ADAPTER / "run-r1/receipt.json"
    if receipt_path.is_file():
        receipt = json.loads(receipt_path.read_text())
        for name, meta in receipt.get("binaries", {}).items():
            path = ADAPTER / "run-r1" / name
            if path.is_file():
                got = sha256(path)
                check(
                    f"binaries.{name} sha256",
                    got == meta["sha256"],
                    f"got {got[:16]} want {meta['sha256'][:16]}",
                )
            else:
                check(f"binaries.{name} present", False, str(path))
    else:
        check("structure-adapter run-r1 receipt present", False, str(receipt_path))

    print("== 2. canonical sweep data is byte-exact ==")
    data = ARCHIVE / "receipts/result-r1/data.json"
    if data.is_file():
        check("receipts/result-r1/data.json sha256", sha256(data) == CANON_DATA_SHA)
    else:
        check("receipts/result-r1/data.json present", False, str(data))

    print("== 3. removed duplicates are really gone ==")
    for rel in REMOVED_DUPES:
        check(f"{rel} absent", not (ARCHIVE / rel).exists())
    stray = sorted(p.name for p in (ADAPTER / "run-r1").glob("*.d"))
    check("no compiler .d files restored", not stray, ", ".join(stray))

    print("== 4. acceptance tables readable and non-trivial ==")
    for name in ("acceptance-prefill-sweep.json", "acceptance-decode-sweep.json"):
        path = ARCHIVE / "receipts" / name
        try:
            tables = json.loads(path.read_text())["tables"]
            usable = sum(1 for t in tables if "r4 Read" in t.get("header", []))
            check(f"{name}: {usable} of {len(tables)} tables carry r4 columns", usable > 0)
        except Exception as exc:  # noqa: BLE001
            check(f"{name} readable", False, repr(exc))

    print("== 5. every relative Markdown link resolves ==")
    dangling = []
    scanned = 0
    for md in sorted(set(REPO.glob("*.md")) | set(REPO.glob("docs/*.md"))
                     | set(REPO.glob("*/*.md")) | set(REPO.glob("*/*/*.md"))):
        scanned += 1
        text = md.read_text(encoding="utf-8", errors="replace")
        for m in re.finditer(r"\]\(([^)\s]+)\)", text):
            target = m.group(1).strip().split("#", 1)[0]
            if not target or target.startswith(("http://", "https://", "mailto:", "/")):
                continue
            if not (md.parent / target).resolve().exists():
                dangling.append(f"{md.relative_to(REPO)} -> {target}")
    check(f"all links resolve across {scanned} files", not dangling, "; ".join(dangling[:6]))

    print()
    print(f"S0 archive integrity: {_ok} pass / {_fail} fail")
    return 0 if _fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
