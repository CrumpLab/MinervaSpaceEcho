#!/usr/bin/env python3
"""Generates the manual's reference pages from the source, so they never drift:

- reference/_parameters.md  from engine/src/Params.cpp (IDs, names, ranges,
  defaults) and plugin/src/ParamPanel.cpp (which page each is on), with the
  descriptions in param_docs.py
- reference/_presets.md     from presets/factory and presets/examples
- _listening.md             the listening examples (audio/<name>.mp3 if rendered)
- _changelog.md             CHANGELOG.md without its title
- images/screenshot*.png    copied from docs/

Run from the manual directory (Quarto's pre-render does this). Standard
library only.
"""
import pathlib
import re
import shutil
import sys

HERE = pathlib.Path(__file__).resolve().parent
MANUAL = HERE.parent
ROOT = MANUAL.parent
sys.path.insert(0, str(HERE))
from param_docs import DOCS  # noqa: E402

CONSTANTS = {"kGateOffDb": -100.0, "kLevelOffDb": -60.0}


def number(token):
    token = token.strip().rstrip("f")
    return CONSTANTS[token] if token in CONSTANTS else float(token)


def fmt(v):
    return f"{v:g}"


def parse_params():
    src = (ROOT / "engine/src/Params.cpp").read_text()
    choices = {}
    for m in re.finditer(r"const char\* const (k\w+)\[\] = \{([^}]*)\};", src):
        choices[m.group(1)] = re.findall(r'"([^"]*)"', m.group(2))
    specs = []
    pattern = re.compile(
        r'\{\s*"(\w+)",\s*"([^"]*)",\s*ParamType::(\w+),\s*([^,]+),\s*([^,]+),\s*([^,]+),\s*([^,]+),\s*"([^"]*)",\s*'
        r"(CHOICE \((\w+)\)|NOCHOICE),\s*(true|false)\s*\}"
    )
    for m in pattern.finditer(src):
        pid, name, ptype, lo, hi, default, _skew, unit, _c, carr, auto = m.groups()
        specs.append(
            dict(id=pid, name=name, type=ptype, min=number(lo), max=number(hi), default=number(default), unit=unit,
                 choices=choices.get(carr, []), automatable=auto == "true")
        )
    if len(specs) < 80:
        sys.exit(f"generate.py: only parsed {len(specs)} parameters from Params.cpp")
    return specs


def parse_pages():
    src = (ROOT / "plugin/src/ParamPanel.cpp").read_text()
    pages = []
    for m in re.finditer(r'\{\s*"(\w+)",\s*\{([^}]*)\}\s*\}', src):
        pages.append((m.group(1), re.findall(r'"(\w+)"', m.group(2))))
    return pages


def value_text(p, v):
    if p["type"] in ("Choice", "Bool"):
        return p["choices"][int(v)]
    unit = f" {p['unit']}" if p["unit"] else ""
    if p["unit"] == "dB" and v <= -60 and p["min"] <= -60 and v == p["min"]:
        return f"{fmt(v)} dB (off)"
    return f"{fmt(v)}{unit}"


def range_text(p):
    if p["type"] == "Bool":
        return "Off / On"
    if p["type"] == "Choice":
        return " · ".join(p["choices"])
    unit = f" {p['unit']}" if p["unit"] else ""
    return f"{fmt(p['min'])} – {fmt(p['max'])}{unit}"


def cell(text):
    return text.replace("|", "\\|")


def write_parameters(specs, pages):
    by_id = {p["id"]: p for p in specs}
    missing = [p["id"] for p in specs if p["id"] not in DOCS]
    if missing:
        sys.exit("generate.py: no description for " + ", ".join(missing))
    out = []
    for page, ids in pages:
        out.append(f"## {page} page {{#page-{page.lower()}}}\n")
        out.append("| Parameter | Range | Default | What it does |")
        out.append("|---|---|---|---|")
        for pid in ids:
            p = by_id[pid]
            name = f"**{p['name']}**<br>[`{pid}`]{{.param-id}}"
            if not p["automatable"]:
                name += "<br>*not automatable*"
            out.append(f"| {name} | {cell(range_text(p))} | {cell(value_text(p, p['default']))} | {cell(DOCS[pid])} |")
        out.append(": {tbl-colwidths=\"[24,22,12,42]\" .param-table}\n")
    (MANUAL / "reference/_parameters.md").write_text("\n".join(out) + "\n")


def read_preset(path):
    text = path.read_text()
    desc, settings, timed = [], [], []
    for line in text.splitlines():
        s = line.strip()
        if s.startswith("#"):
            if not settings and not timed:
                desc.append(s.lstrip("#").strip())
            continue
        if not s:
            continue
        (timed if s.startswith("@") else settings).append(s)
    return " ".join(desc), settings, timed


def preset_settings_md(settings, timed):
    parts = [f"`{s}`" for s in settings] + [f"`{t}`" for t in timed]
    return " ".join(parts) if parts else "*(all defaults)*"


def write_presets():
    out = ["## Factory presets {#factory}\n",
           "Built for playing: the dry signal is on and the echo sits under it.\n",
           "| Preset | What it does | Settings |", "|---|---|---|"]
    for f in sorted((ROOT / "presets/factory").glob("*.txt")):
        desc, settings, timed = read_preset(f)
        out.append(f"| **{f.stem}** | {cell(desc)} | {cell(preset_settings_md(settings, timed))} |")
    out.append(": {tbl-colwidths=\"[20,45,35]\" .preset-settings}\n")
    out += ["## Examples {#examples}\n",
            "The listening examples, also in the plug-in's preset menu. Most are *echo only* (dry off) so you hear "
            "just what memory returns. Lines starting with `@` change a setting at a given bar in offline renders; "
            "the plug-in ignores them. Hear them on [Listening examples](../listening.qmd).\n",
            "| Example | What it shows | Settings |", "|---|---|---|"]
    for f in sorted((ROOT / "presets/examples").glob("*.txt")):
        desc, settings, timed = read_preset(f)
        out.append(f"| **{f.stem.replace('_', ' ')}** | {cell(desc)} | {cell(preset_settings_md(settings, timed))} |")
    out.append(": {tbl-colwidths=\"[20,45,35]\" .preset-settings}\n")
    (MANUAL / "reference/_presets.md").write_text("\n".join(out) + "\n")


def write_listening():
    audio = MANUAL / "audio"
    have_audio = audio.is_dir() and any(audio.glob("*.mp3"))
    out = []
    if not have_audio:
        out.append("::: {.callout-note}\nThe audio for these examples is rendered when the site is built on GitHub "
                   "(see `.github/workflows/docs.yml`). This build has none, so only the descriptions are shown.\n:::\n")
    else:
        out.append("### The input {#input}\n")
        for stem, label in (("input_style_change", "Style change (most examples)"), ("input_drums", "Drums (short-trace and drum examples)")):
            if (audio / f"{stem}.mp3").exists():
                out.append(f"**{label}**\n\n<audio controls preload=\"none\" src=\"audio/{stem}.mp3\"></audio>\n")
    for f in sorted((ROOT / "presets/examples").glob("*.txt")):
        desc, settings, timed = read_preset(f)
        out.append(f"### {f.stem.replace('_', ' ')} {{#{f.stem}}}\n")
        out.append(desc + "\n")
        out.append(preset_settings_md(settings, timed) + "\n")
        if have_audio and (audio / f"{f.stem}.mp3").exists():
            out.append(f"<audio controls preload=\"none\" src=\"audio/{f.stem}.mp3\"></audio>\n")
    (MANUAL / "_listening.md").write_text("\n".join(out) + "\n")


def write_changelog():
    text = (ROOT / "CHANGELOG.md").read_text()
    text = re.sub(r"^# Changelog\s*\n", "", text)
    (MANUAL / "_changelog.md").write_text(text)


def copy_images():
    (MANUAL / "images").mkdir(exist_ok=True)
    for png in (ROOT / "docs").glob("screenshot*.png"):
        shutil.copy2(png, MANUAL / "images" / png.name)


def main():
    specs = parse_params()
    write_parameters(specs, parse_pages())
    write_presets()
    write_listening()
    write_changelog()
    copy_images()
    print(f"generate.py: {len(specs)} parameters, presets, listening page, changelog")


if __name__ == "__main__":
    main()
