"""Put the A540 power-and-clock probe into a checked-out chiron kernel source tree.

Runs inside the CI job, after ci/hackios/chiron.py prepare has placed the kernel at
$PORT_WORK/kernel. Three edits, all inside xnu/files/bsd/dev/redwood_adreno.c (a file the build
already compiles, so no file list has to be touched):

  1. a prototype for the probe, before redwood_adreno_init;
  2. the one call that runs it, at the top of redwood_adreno_init;
  3. the probe itself, appended to the end of the file.

Idempotent: a file that already carries the probe is left as it is.
"""

import argparse
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
INC = HERE / "a540_probe.inc"

PROTO = (
    "static uint32_t redwood_adreno_a540_probe(uint32_t through);\n"
    "static bool redwood_adreno_a540_asked(uint32_t *through);\n"
)

ANCHOR_DEF = "void\nredwood_adreno_init(void)\n"
ANCHOR_CALL = '\tif (!PE_parse_boot_argn("adreno", &last_step, sizeof(last_step))) {\n'
CALL = (
    '\t{\n'
    '\t\tuint32_t a540_through = 0;\n'
    '\n'
    '\t\tif (redwood_adreno_a540_asked(&a540_through)) {\n'
    '\t\t\tredwood_adreno_a540_probe(a540_through);\n'
    '\t\t\treturn;\n'
    '\t\t}\n'
    '\t}\n'
)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", required=True, help="$PORT_WORK/kernel, or the xnu tree itself")
    args = ap.parse_args()

    root = pathlib.Path(args.source)
    targets = list(root.glob("**/files/bsd/dev/redwood_adreno.c")) or list(root.glob("**/redwood_adreno.c"))
    if not targets:
        print("a540: no redwood_adreno.c under", root, file=sys.stderr)
        return 2
    for target in targets:
        text = target.read_text(encoding="utf-8")
        if "redwood_adreno_a540_probe" in text:
            print("a540: already present in", target)
            continue
        if ANCHOR_DEF not in text:
            print("a540: init not found in", target, file=sys.stderr)
            return 3
        if ANCHOR_CALL not in text:
            print("a540: call anchor not found in", target, file=sys.stderr)
            return 4
        text = text.replace(ANCHOR_DEF, PROTO + "\n" + ANCHOR_DEF, 1)
        text = text.replace(ANCHOR_CALL, CALL + ANCHOR_CALL, 1)
        text = text.rstrip("\n") + "\n\n" + INC.read_text(encoding="utf-8").rstrip("\n") + "\n"
        target.write_text(text, encoding="utf-8")
        print("a540: patched", target, len(text), "bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
