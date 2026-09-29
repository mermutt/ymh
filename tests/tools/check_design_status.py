#!/usr/bin/env python3
"""Design/tracker drift TRIPWIRE -- a detector, NOT a proof.

This checks that `docs/design/DESIGN_STATUS.md` and each spec's own header status
block still agree: status (verified/draft), revision, a non-empty reviewer and a
gate-pass token for every `verified` row, and that no status block carries both a
verified and a draft marker.

It does NOT prove that code followed design. It only catches drift between the
two documents. It is a dev-side check: it runs on the merged tree (where `docs/`
and `tests/` coexist) and is never registered on a main-only checkout, where
`tests/` does not exist and it could silently no-op green.

Known limitation: it compares the tracker's Verified/Reviewer cells and the
spec's Status block, but it does not parse the free-form Notes prose; a stale
Notes sentence is caught only by review, not by this script.
"""

from __future__ import annotations

import argparse
import glob
import os
import re
import sys


def parse_tracker(path: str):
    rows = {}
    for line in open(path, encoding="utf-8"):
        if not line.lstrip().startswith("|"):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) < 6 or not re.fullmatch(r"\d+", cells[0]):
            continue
        m = re.search(r"`([^`]+)`", cells[1])
        if not m:
            continue
        rows[int(cells[0])] = {
            "file": m.group(1),
            "verified": " ".join(cells[3].split()),
            "reviewer": " ".join(cells[4].split()),
            "line": line.rstrip(),
        }
    return rows


def header_block(lines):
    for i, line in enumerate(lines[:40]):
        if line.strip() == "```":
            for j in range(i + 1, min(len(lines), i + 60)):
                if lines[j].strip() == "```":
                    block = lines[i + 1 : j]
                    if any(re.match(r"^\s*\**Status:", x) for x in block):
                        return block
                    break
    for i, line in enumerate(lines[:160]):
        if re.match(r"^\s*\**Status:", line):
            out = [line]
            for k in range(i + 1, min(i + 30, len(lines))):
                nxt = lines[k]
                if nxt.strip() == "" or nxt.strip() == "```":
                    break
                if re.match(
                    r"^\s*(Status|Verification status|Revision|Component|Depends on|"
                    r"Supersedes|Amends|Retained|Scope)\s*:",
                    nxt,
                ):
                    break
                out.append(nxt)
            return out
    return []


def field(block, name):
    text = "\n".join(block)
    m = re.search(
        r"(?ms)^\s*\**" + name + r":\**\s*(.+?)(?=\n\s*\**\w[\w ]*:\s|\Z)", text
    )
    return " ".join(m.group(1).split()) if m else ""


def first_rev(text):
    if not text:
        return None
    m = re.search(r"Rev(?:ision)?\s*(\d+)", text, re.I)
    return m.group(1) if m else None


def claim(status, verif):
    text = f"{status} {verif}"
    draft = bool(re.search(r"\bdraft\b", text, re.I))
    verified = False
    for m in re.finditer(r"verified", text, re.I):
        prefix = text[max(0, m.start() - 4) : m.start()].lower()
        if prefix.endswith("not ") or prefix.endswith("un"):
            continue
        verified = True
        break
    if (
        re.search(r"verified:\s*[—–-]", text, re.I)
        or re.search(r"verified:\s*pending", text, re.I)
        or "not yet" in text.lower()
    ):
        verified = False
    return verified, draft


def tracker_state(row):
    value = row["verified"].lower()
    if re.search(r"\bdraft\b", value):
        return "draft"
    if "verified" in value or "implemented" in value:
        return "verified"
    return "other"


def gate_token_ok(row):
    text = re.sub(r"\s+", " ", f"{row['reviewer']} {row['verified']} {row['line']}".lower())
    combined = [
        r"0\s*(?:open\s*)?high\s*/\s*0\s*(?:open\s*)?medium",
        r"0\s*high\s*/\s*0\s*medium",
        r"0\s*open\s*high\s*/\s*0?\s*medium",
        r"no\s+open\s+high\s*/\s*medium",
        r"zero\s+open\s+high\s*/\s*medium",
        r"0h\s*/\s*0m",
    ]
    if any(re.search(p, text) for p in combined):
        return True
    high = re.search(r"(0\s*(?:open\s*)?high|no\s+open\s+high|0h\b)", text)
    med = re.search(r"(0\s*(?:open\s*)?medium|no\s+open\s+medium|0m\b)", text)
    return bool(high and med)


def gate_required(row):
    return bool(re.match(r"[\*\s]*verified\b", row["verified"]))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=None)
    ap.add_argument("--design-dir", default=None)
    args = ap.parse_args()

    root = args.root or os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    design = args.design_dir or os.path.join(root, "docs", "design")
    tracker_path = os.path.join(design, "DESIGN_STATUS.md")
    if not os.path.isfile(tracker_path):
        print(f"drift-tripwire: FATAL: {tracker_path} not found (must run on the merged tree)")
        return 2

    rows = parse_tracker(tracker_path)
    failures: list[str] = []

    for num in sorted(rows):
        row = rows[num]
        path = os.path.join(design, row["file"])
        if not os.path.isfile(path):
            failures.append(f"[{num:02d}] tracker row points at a missing spec: {row['file']}")
            continue

        lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
        block = header_block(lines)
        status = field(block, "Status")
        verif = field(block, "Verification status")
        revision = field(block, "Revision")
        combined = f"{status} {verif}".strip()

        if not combined:
            failures.append(f"[{num:02d}] {row['file']}: no Status header (add one)")
            continue

        verified, draft = claim(status, verif)
        state = tracker_state(row)

        if verified and draft:
            failures.append(f"[{num:02d}] {row['file']}: header status carries both verified and draft markers")

        if state != "other":
            if verified != (state == "verified"):
                failures.append(
                    f"[{num:02d}] {row['file']}: header verified={verified} vs tracker verified={state == 'verified'}"
                )
            if draft and state == "verified":
                failures.append(f"[{num:02d}] {row['file']}: header draft vs tracker verified")

        hdr_rev = first_rev(f"{status} {verif}") or first_rev(revision)
        trk_rev = first_rev(row["verified"])
        if hdr_rev and trk_rev and hdr_rev != trk_rev:
            failures.append(f"[{num:02d}] {row['file']}: rev header={hdr_rev} vs tracker={trk_rev}")

        if state == "verified" and gate_required(row):
            if not row["reviewer"] or row["reviewer"] in {"-", "—"}:
                failures.append(f"[{num:02d}] {row['file']}: verified row has no reviewer")
            if not gate_token_ok(row):
                failures.append(f"[{num:02d}] {row['file']}: verified row has no '0 HIGH / 0 MEDIUM' token")

    for path in sorted(glob.glob(os.path.join(design, "[0-9][0-9]-*.md"))):
        base = os.path.basename(path)
        if int(base[:2]) not in rows:
            failures.append(f"[{base[:2]}] {base}: spec has no tracker row in DESIGN_STATUS.md")

    if failures:
        print("drift-tripwire: FAIL — design/tracker drift detected (detector, not a proof):")
        for f in failures:
            print("  " + f)
        return 1
    print(f"drift-tripwire: PASS — {len(rows)} tracker rows and their specs agree.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
