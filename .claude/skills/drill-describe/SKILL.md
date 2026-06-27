---
name: drill-describe
description: Update an OpenDojo .drill.txt file's description field and per-recording slot names from a user-supplied move list. Use when the user asks to set/update a drill's description and rename its move slots (like the existing mr_raph_*_drills.drill.txt files).
---

# OpenDojo drill describer

Sets a drill file's `description:` and renames each recording's `name:` to its move name. Drills live in `<TEKKEN 8>\Polaris\Binaries\Win64\opendojo\*.drill.txt`.

## Input

User gives a target `.drill.txt` path and a move list, one move per line:

```
<move notation> - <coaching note>
```

e.g. `df+2,1 - duck 2nd hit and launch`. Line order = slot order (recording 1 = first line).

## Format facts (drill.cpp parser)

- Parser reads `key: value` **one line each**. The `description:` field MUST be a single line — joining multiple moves with spaces. Multi-line breaks parsing (only first line kept).
- Each recording block starts `--- recording N` followed by `name: <text>`. Names are free text, may contain commas/`+`/parentheses (see victor file).
- Recording count must already match the move count; don't add/remove recordings. Just rename.

## Steps

1. Read the target file. Count `--- recording N` blocks.
2. Verify move-list line count == recording count. If mismatch, stop and tell the user which side is off — wrong mapping silently mislabels slots.
3. **Sanity-check the mapping** against live recordings: a `kind: movelist` block is opaque, but a live recording (has `events:`/event lines) reveals its inputs — confirm the move notation matches the recorded buttons (e.g. a line ending in many `1+2` events → a `...1+2` move). Flag any slot where notation and recorded inputs disagree.
4. Set `description:` to all move lines joined into ONE space-separated line, verbatim (keep the `notation - note` text).
5. For each recording in order, replace its `name:` value with that slot's move notation (the part before ` - `).
6. Report the slot→name mapping back so the user can eyeshot it.

## Reference

Mirror the style of `mr_raph_victor_drills.drill.txt` (names = bare move notation; description = run-on of `move -> note` phrases).
