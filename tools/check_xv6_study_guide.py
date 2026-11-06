"""Check the generated mobile PDF for visible geometry/content regressions."""

from __future__ import annotations

import json
import re
import unicodedata
from pathlib import Path

import pdfplumber


ROOT = Path(__file__).resolve().parents[1]
PDF = ROOT / "output/pdf/xv6_pa1_pa3_mobile_exam_guide.pdf"
SOURCE = ROOT / "tools/data/mobile_guide_content.json"


def is_blue(color: object) -> bool:
    if not isinstance(color, tuple) or len(color) != 3:
        return False
    expected = (235 / 255, 243 / 255, 248 / 255)
    return all(abs(observed - target) < 0.008 for observed, target in zip(color, expected))


data = json.loads(SOURCE.read_text(encoding="utf-8"))
problems: list[str] = []

with pdfplumber.open(PDF) as pdf:
    all_text = "\n".join(
        page.crop((0, 45, page.width, page.height - 45)).extract_text() or ""
        for page in pdf.pages
    )
    squash = lambda s: re.sub(r"\s+", "", unicodedata.normalize("NFKC", s))
    normalized_output = squash(all_text)
    for block in data["blocks"]:
        if block["kind"] == "code":
            # The source PDF used >> solely as a visual continuation marker.
            unmarked = [re.sub(r"^\s*>>\s*", "", line) for line in block["lines"]]
            expected_text = "\n".join(unmarked)
        else:
            expected_text = block["text"]
        if squash(expected_text) not in normalized_output:
            problems.append(f"Missing text block #{block['id']} ({block['kind']})")
    for expected in ("01", "02", "03", "04", "05", "06", "07", "08", "09"):
        if f"{expected}  " not in all_text and f"{expected} " not in all_text:
            problems.append(f"Missing section {expected}")

    for block in data["blocks"]:
        if block["kind"] == "code_label":
            label = block["text"].split("|")[-1].strip()
            if squash(label) not in normalized_output:
                problems.append(f"Missing code label #{block['id']}: {label}")

    for page_num, page in enumerate(pdf.pages, 1):
        blues = [r for r in page.rects if is_blue(r.get("non_stroking_color"))]
        for c in page.chars:
            if c["text"] in "\x00\ufffd":
                problems.append(f"Invalid character on p{page_num}")
            if c["x0"] < 17 or c["x1"] > page.width - 17:
                problems.append(f"Horizontal overflow on p{page_num}: {c['text']!r} x={c['x0']:.1f}..{c['x1']:.1f}")
            if c["top"] < 12 or c["bottom"] > page.height - 12:
                problems.append(f"Vertical overflow on p{page_num}: {c['text']!r} y={c['top']:.1f}..{c['bottom']:.1f}")
            if "Consolas" in c.get("fontname", ""):
                continue
            for r in blues:
                x_overlap = min(c["x1"], r["x1"]) - max(c["x0"], r["x0"])
                y_overlap = min(c["bottom"], r["bottom"]) - max(c["top"], r["top"])
                if x_overlap > 0.5 and y_overlap > 0.5:
                    problems.append(
                        f"Code background crosses prose on p{page_num}: "
                        f"{c['text']!r} y={c['top']:.1f}..{c['bottom']:.1f} "
                        f"rect={r['top']:.1f}..{r['bottom']:.1f}"
                    )
                    break

    print(f"Pages: {len(pdf.pages)}")
    print(f"Source blocks: {len(data['blocks'])}")
    print(f"Issues: {len(problems)}")
    for item in problems[:100]:
        print(item)

if problems:
    raise SystemExit(1)
