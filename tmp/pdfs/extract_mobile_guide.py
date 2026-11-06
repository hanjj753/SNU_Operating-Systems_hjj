"""Recover ordered, styled content from the existing mobile OS study PDF.

This script only reads the PDF and writes an intermediate JSON file. It ignores
the old code-box rectangles because their geometry is the layout defect being
repaired. Each code line is reconstructed from character positions so leading
indentation survives PDF text extraction.
"""

from __future__ import annotations

import collections
import json
import re
from pathlib import Path

import pdfplumber


ROOT = Path(r"D:\Workspace\xv6-riscv-snu")
# The first PDF is backed up before the redesigned version replaces its stable
# output path. Keep extraction reproducible from that immutable source.
INPUT = ROOT / "tmp/pdfs/old_guide.pdf"
OUTPUT = ROOT / "tmp/pdfs/mobile_guide_content.json"
CODE_LEFT = 35.0
CODE_CHAR_WIDTH = 3.906


def dominant_style(chars):
    size = collections.Counter(round(c["size"], 1) for c in chars).most_common(1)[0][0]
    font = collections.Counter(c["fontname"].split("+")[-1] for c in chars).most_common(1)[0][0]
    return size, font


def classify(line):
    size, font = line["size"], line["font"]
    value = line["text"]
    if size >= 20:
        return "title"
    if size >= 15:
        return "section"
    if size >= 10.8 and "Bold" in font:
        return "sub"
    if size == 7.8 and "Bold" in font:
        return "code_label"
    if size == 7.1 and font == "Consolas":
        return "code"
    if size == 9.5 and "Bold" in font:
        return "q"
    if size == 9.2 and value.lstrip().startswith("해설 |"):
        return "answer"
    if size == 9.2 and (value.lstrip().startswith("•") or line["x0"] >= 43):
        return "item_or_answer_continuation"
    if size == 8.9:
        return "note"
    if size <= 8.4:
        return "small"
    return "body"


def normalized_line(line):
    value = line["text"].rstrip()
    if line["kind"] == "code":
        count = max(0, round((line["x0"] - CODE_LEFT) / CODE_CHAR_WIDTH))
        return (" " * count) + value
    return value.strip()


def should_merge(current, line, previous_line):
    if current is None or previous_line is None:
        return False
    kind = line["kind"]
    if current["kind"] != kind:
        return False
    if kind in {"section", "sub", "code_label"}:
        return False
    if kind == "q" and re.match(r"^\d{2}\.\s", line["text"].lstrip()):
        return False
    if kind == "answer" and line["text"].lstrip().startswith("해설 |"):
        return False
    if kind == "item" and line["text"].lstrip().startswith("•"):
        return False
    if line["page"] != previous_line["page"]:
        # Only code may continue on the next page without a fresh label.
        return kind == "code" and line["page"] == previous_line["page"] + 1
    dy = line["top"] - previous_line["top"]
    if kind == "code":
        return dy <= 32
    if kind == "title":
        return dy <= 31
    limits = {"body": 17.0, "note": 15.0, "small": 14.0, "q": 17.0,
              "answer": 16.0, "item": 16.0}
    return dy <= limits.get(kind, 0)


def main():
    lines = []
    with pdfplumber.open(INPUT) as pdf:
        page_count = len(pdf.pages)
        for page_number, page in enumerate(pdf.pages, 1):
            for extracted in page.extract_text_lines(layout=False):
                top = float(extracted["top"])
                if top < 31 or top > 610:  # repeated running header/footer
                    continue
                text = extracted["text"]
                if not text.strip():
                    continue
                size, font = dominant_style(extracted["chars"])
                line = {
                    "page": page_number,
                    "top": round(top, 2),
                    "x0": round(float(extracted["x0"]), 2),
                    "size": size,
                    "font": font,
                    "text": text,
                }
                line["kind"] = classify(line)
                lines.append(line)

    blocks = []
    current = None
    previous_line = None
    for line in lines:
        if line["kind"] == "item_or_answer_continuation":
            if (current is not None and current["kind"] == "answer"
                    and not line["text"].lstrip().startswith("•")):
                line["kind"] = "answer"
            else:
                line["kind"] = "item"
        if should_merge(current, line, previous_line):
            if line["kind"] == "code" and line["page"] == previous_line["page"]:
                # Consecutive code baselines are 10.2 pt apart. Larger gaps
                # represent blank source lines drawn by the original guide.
                gap = line["top"] - previous_line["top"]
                if gap > 15.0:
                    current["lines"].append("")
            current["lines"].append(normalized_line(line))
            if line["page"] not in current["source_pages"]:
                current["source_pages"].append(line["page"])
            current["end_top"] = line["top"]
        else:
            current = {
                "kind": line["kind"],
                "source_page": line["page"],
                "source_pages": [line["page"]],
                "start_top": line["top"],
                "end_top": line["top"],
                "lines": [normalized_line(line)],
            }
            blocks.append(current)
        previous_line = line

    for index, block in enumerate(blocks):
        block["id"] = index + 1
        if block["kind"] in {"code", "title"}:
            block["text"] = "\n".join(block["lines"])
        else:
            block["text"] = " ".join(block["lines"])
        if block["id"] == 12:
            block["text"] = block["text"].replace(
                ">>는 지면용 줄바꿈", "들여쓴 이어지는 줄은 지면용 줄바꿈"
            )
            block["lines"] = [block["text"]]
        if block["id"] == 154 and block["kind"] == "code":
            # The prior layout cut off the final brace of sys_sched_yield().
            # It is the next line in pa3:kernel/sysproc.c after `return 0;`.
            if block["lines"][-1].strip() != "}":
                block["lines"].append("}")
                block["text"] = "\n".join(block["lines"])
    by_page = collections.Counter(p for b in blocks for p in b["source_pages"])
    data = {
        "schema_version": 1,
        "source_pdf": str(INPUT),
        "page_count": page_count,
        "block_count": len(blocks),
        "source_pages_represented": sorted(by_page),
        "blocks_by_kind": dict(collections.Counter(b["kind"] for b in blocks)),
        "blocks": blocks,
    }
    assert sorted(by_page) == list(range(1, page_count + 1))
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"{OUTPUT} | pages={page_count} blocks={len(blocks)}")
    print(data["blocks_by_kind"])


if __name__ == "__main__":
    main()
