"""Rebuild the mobile xv6/PA study guide from extracted semantic blocks.

The source content is `tmp/pdfs/mobile_guide_content.json`, recovered from the
earlier PDF. ReportLab owns all vertical flow, including each line of every
code table, so shaded code backgrounds cannot overlap neighboring paragraphs.
"""

from __future__ import annotations

import html
import json
import re
from pathlib import Path

from reportlab.lib import colors
from reportlab.lib.enums import TA_LEFT
from reportlab.lib.pagesizes import portrait
from reportlab.lib.styles import ParagraphStyle
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.platypus import (
    CondPageBreak,
    KeepTogether,
    PageBreak,
    Paragraph,
    Preformatted,
    SimpleDocTemplate,
    Table,
    TableStyle,
)


ROOT = Path(r"D:\Workspace\xv6-riscv-snu")
CONTENT = ROOT / "tools/data/mobile_guide_content.json"
OUTPUT = ROOT / "output/pdf/xv6_pa1_pa3_mobile_exam_guide.pdf"
PAGE_W, PAGE_H = 396.0, 744.0
MARGIN_X = 24.0
CONTENT_W = PAGE_W - 2 * MARGIN_X

FONT_DIR = Path(r"C:\Windows\Fonts")
pdfmetrics.registerFont(TTFont("Hancom", str(FONT_DIR / "Hancom Gothic Regular.ttf")))
pdfmetrics.registerFont(TTFont("HancomBold", str(FONT_DIR / "Hancom Gothic Bold.ttf")))
pdfmetrics.registerFont(TTFont("Consolas", str(FONT_DIR / "consola.ttf")))
pdfmetrics.registerFont(TTFont("ConsolasBold", str(FONT_DIR / "consolab.ttf")))

INK = colors.HexColor("#172334")
MUTED = colors.HexColor("#52677B")
ACCENT = colors.HexColor("#155E88")
CODE_BG = colors.HexColor("#EBF3F8")
NOTE_BG = colors.HexColor("#FFF8EC")
ANSWER_BG = colors.HexColor("#F6F5F1")


def style(name, font, size, leading, **kwargs):
    options = {
        "fontName": font,
        "fontSize": size,
        "leading": leading,
        "textColor": INK,
        "alignment": TA_LEFT,
        "allowWidows": 0,
        "allowOrphans": 0,
    }
    options.update(kwargs)
    return ParagraphStyle(name, **options)


STYLES = {
    "title": style("Title", "HancomBold", 22.0, 28.0, spaceBefore=20, spaceAfter=18),
    "section": style("Section", "HancomBold", 18.0, 25.0,
                     textColor=ACCENT, spaceBefore=10, spaceAfter=14, keepWithNext=1),
    "sub": style("Sub", "HancomBold", 13.0, 19.0,
                 textColor=ACCENT, spaceBefore=17, spaceAfter=7, keepWithNext=1),
    "body": style("Body", "Hancom", 11.6, 17.3, spaceAfter=11),
    "item": style("Item", "Hancom", 11.3, 17.0, leftIndent=3,
                  firstLineIndent=-3, spaceAfter=9),
    "note": style("Note", "Hancom", 10.9, 16.5, spaceBefore=6,
                  spaceAfter=13, backColor=NOTE_BG, borderPadding=7),
    "small": style("Small", "Hancom", 10.1, 15.2, textColor=MUTED,
                   spaceAfter=12),
    "code_label": style("CodeLabel", "HancomBold", 10.2, 15.2,
                        textColor=ACCENT, spaceBefore=12, spaceAfter=6,
                        keepWithNext=1),
    "q": style("Question", "HancomBold", 11.7, 17.4,
               spaceBefore=13, spaceAfter=5, keepWithNext=1),
    "answer": style("Answer", "Hancom", 11.3, 17.0,
                    leftIndent=6, spaceAfter=11, backColor=ANSWER_BG,
                    borderPadding=7),
    "code": style("Code", "Consolas", 9.0, 12.3),
}


def wrap_code_line(raw: str, max_width: float) -> list[str]:
    """Wrap long source lines using hanging indentation without extra glyphs."""
    if not raw:
        return [" "]
    result = []
    remaining = raw.expandtabs(4)
    first = True
    while remaining and pdfmetrics.stringWidth(remaining, "Consolas", 9.0) > max_width:
        prefix = "" if first else "    "
        available = max_width - pdfmetrics.stringWidth(prefix, "Consolas", 9.0)
        chars = max(1, int(available / pdfmetrics.stringWidth("M", "Consolas", 9.0)))
        # Prefer a whitespace boundary where it does not reduce the line too
        # much. Otherwise split at the measured maximum.
        split = min(chars, len(remaining) - 1)
        candidates = [i for i in range(max(1, split - 14), split + 1)
                      if remaining[i - 1].isspace()]
        if candidates:
            split = candidates[-1]
            suffix = remaining[split:].strip()
            if (len(suffix) <= 4 or suffix.startswith(("==", "!=", "&&", "||"))):
                if len(candidates) >= 2:
                    split = candidates[-2]
        head = remaining[:split].rstrip()
        result.append(prefix + head)
        remaining = remaining[split:].lstrip()
        first = False
    if remaining:
        result.append(("" if first else "    ") + remaining)
    return result


def code_tables(block):
    code_inset = 9.0
    max_width = CONTENT_W - code_inset * 2
    lines = []
    logical_lines = []
    for source_line in block["lines"]:
        marker = re.match(r"^\s*>>\s+(.*)$", source_line)
        if marker and logical_lines:
            # The old guide inserted this marker into a *single* C statement
            # during its own line wrapping. Rejoin before applying the new
            # width measurement, keeping real C shift operators intact.
            logical_lines[-1] = logical_lines[-1].rstrip() + " " + marker.group(1)
        else:
            logical_lines.append(source_line)
    for source_line in logical_lines:
        lines.extend(wrap_code_line(source_line, max_width))
    # One table row per rendered line allows ReportLab to split large excerpts
    # across pages while computing the blue background's exact height.
    def make_table(part: list[str], after: float):
        rows = [[Preformatted(line, STYLES["code"])] for line in part]
        table = Table(rows, colWidths=[CONTENT_W], hAlign="LEFT", splitByRow=1)
        table.setStyle(TableStyle([
            ("BACKGROUND", (0, 0), (-1, -1), CODE_BG),
            ("LEFTPADDING", (0, 0), (-1, -1), code_inset),
            ("RIGHTPADDING", (0, 0), (-1, -1), code_inset),
            ("TOPPADDING", (0, 0), (-1, -1), 1.2),
            ("BOTTOMPADDING", (0, 0), (-1, -1), 1.2),
            ("VALIGN", (0, 0), (-1, -1), "TOP"),
        ]))
        table.spaceAfter = after
        return table

    # Keep the caption with two visible source lines, while allowing the rest
    # of a long excerpt to fill the current page and split naturally.
    if len(lines) > 2:
        return [make_table(lines[:2], 0.0), make_table(lines[2:], 13.0)]
    return [make_table(lines, 13.0)]


def header_footer(canvas, doc):
    canvas.saveState()
    canvas.setStrokeColor(colors.HexColor("#C9D7E3"))
    canvas.setLineWidth(0.5)
    canvas.line(MARGIN_X, PAGE_H - 34, PAGE_W - MARGIN_X, PAGE_H - 34)
    canvas.setFont("ConsolasBold", 7.2)
    canvas.setFillColor(MUTED)
    canvas.drawString(MARGIN_X, PAGE_H - 25, "XV6 / PA1-PA3 / CODE GUIDE")
    canvas.line(MARGIN_X, 33, PAGE_W - MARGIN_X, 33)
    canvas.setFont("Hancom", 7.8)
    canvas.drawString(MARGIN_X, 20, "운영체제 중간고사 · 이동 중 복습용")
    canvas.setFont("Consolas", 7.8)
    page_number = f"{doc.page:02d}"
    canvas.drawRightString(PAGE_W - MARGIN_X, 20, page_number)
    canvas.restoreState()


def main():
    data = json.loads(CONTENT.read_text(encoding="utf-8"))
    blocks = data["blocks"]
    assert data["page_count"] == 31 and len(blocks) == 234
    assert sum(b["kind"] == "q" for b in blocks) == 22
    assert sum(b["kind"] == "answer" for b in blocks) == 22
    assert sum(b["kind"] == "code_label" for b in blocks) == 34
    assert sum(b["kind"] == "code" for b in blocks) == 34

    story = []
    rendered_blocks = 0
    index = 0
    while index < len(blocks):
        block = blocks[index]
        kind = block["kind"]
        following = blocks[index + 1] if index + 1 < len(blocks) else None
        # The source PDF cut a Korean sentence at a page boundary. Restore it
        # as one paragraph without dropping either source block.
        if block["id"] == 155 and following and following["id"] == 156:
            merged = html.escape(block["text"] + " " + following["text"])
            story.append(KeepTogether([Paragraph(merged, STYLES["body"])]))
            rendered_blocks += 2
            index += 2
            continue
        if (kind == "sub" and block["text"] == "차례" and following
                and following["kind"] == "body"):
            heading = Paragraph(html.escape(block["text"]), STYLES["sub"])
            contents = Paragraph(html.escape(following["text"]), STYLES["body"])
            story.append(KeepTogether([heading, contents]))
            rendered_blocks += 2
            index += 2
            continue
        if kind == "q" and following and following["kind"] == "answer":
            question = Paragraph(html.escape(block["text"]), STYLES["q"])
            answer = Paragraph(html.escape(following["text"]), STYLES["answer"])
            story.append(KeepTogether([question, answer]))
            rendered_blocks += 2
            index += 2
            continue
        if kind == "section":
            if block["text"].startswith(("05", "06", "07")):
                story.append(PageBreak())
            else:
                story.append(CondPageBreak(180))
        if kind == "code":
            story.extend(code_tables(block))
        else:
            text = html.escape(block["text"]).replace("\n", "<br/>")
            paragraph = Paragraph(text, STYLES[kind])
            if kind in {"body", "item", "note", "answer"}:
                story.append(KeepTogether([paragraph]))
            else:
                story.append(paragraph)
        rendered_blocks += 1
        index += 1
    assert rendered_blocks == 234

    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    pending = OUTPUT.with_suffix(".building.pdf")
    doc = SimpleDocTemplate(
        str(pending),
        pagesize=(PAGE_W, PAGE_H),
        leftMargin=MARGIN_X,
        rightMargin=MARGIN_X,
        topMargin=48,
        bottomMargin=45,
        title="xv6 + PA1-PA3 코드 읽기: 이동 중 복습용",
        author="Codex",
        subject="운영체제 중간고사 xv6 및 PA1-PA3 코드 학습 노트",
        pageCompression=1,
    )
    doc.build(story, onFirstPage=header_footer, onLaterPages=header_footer)
    pending.replace(OUTPUT)
    print(f"PDF built: {OUTPUT}; input blocks={rendered_blocks}")


if __name__ == "__main__":
    main()
