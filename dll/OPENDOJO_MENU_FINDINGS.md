OpenDojo native row insertion — research notes
==============================================

Status (2026-05-17)
-------------------
**WORKING**: `WBP_UI_PracticeMenu_C:AddButton1Data("OpenDojo", true)` followed by
`UpdateListView1()` inserts a new row in the practice-pause menu's left pane.
ListItems count goes 17 → 18, the row renders visually.

**OPEN**: row label displays as `err(OpenDojo)@@@@` because Tekken uses a custom
Gryphon localization system; AddButton1Data routes the FString through Gryphon's
GetText, which produces an err-FText when the key doesn't exist.

Architecture summary
--------------------
- Localization system: **Gryphon** (Bandai-internal). `UGryphonFunctionLibrary`
  exposes only: `HasText(textId)`, `GetText(textId)`, `GetString(textId)`,
  `RegisterAsset(category, asset)`, `UnregisterAsset(category)`. No runtime
  AddEntry/Insert/Register-text-id API.
- `UGryphonTextBinaryAsset` (UObject) is the binary blob; data offset opaque,
  no BP-exposed write API. Cannot inject keys at runtime via BP/Lua.
- Ghidra: the err-FText is built at `0x145b99510` (and similarly `0x145b98f00`).
  Template literal `"err({0}) @@@@"` lives at `0x148544120` (in module rdata).
  Lookup goes through method dispatch on a Gryphon-instance object at offset
  +0x18 — not a flat hash map we can scan/patch from outside.

The bypass
----------
**`UPolarisTextBlock::SetRawText(FString raw_text, bool ReplaceUnsupportedChar)`**
— declared at `CXXHeaderDump/Polaris.hpp:12351`. Writes the FText literal,
no Gryphon round-trip. Variant exists for `UPolarisRichTextBlock`,
`UPolarisEditableText`, `UPolarisEditableTextBox`.

The row widget `WBP_UI_PracticeMenu_Button_1_C` exposes:
- `TB_Menu_OFF` (UPolarisTextBlock) — text shown in unhovered state
- `TB_Menu_ON` (UPolarisTextBlock) — text shown in hovered/active state
- `IV_Menu_OFF` / `IV_Menu_ON` (UInvalidationBox) — Slate render cache wrappers

The row's own `UpdateData(BP_PracticeMenu_Button_1_Item_C* item)` BP function
internally calls `UPolarisTextBlock::SetTextID(item.Text)` which goes through
Gryphon. We want to instead force `SetRawText(item.Text)` for our row.

UE4SS 3.0.1 caveats (THIS install)
-----------------------------------
- UE4SS 3.0.1 has [#467](https://github.com/UE4SS-RE/RE-UE4SS/issues/467):
  RegisterHook on ANY `UBlueprintFunctionLibrary` static function freezes the
  game. Fixed in 3.1+. This blocked the direct Gryphon `GetText` hook attempt.
- Per-instance BP UFunctions (`PolarisUMGPracticeMenu::InvokeDecideButton1Callback`,
  `WBP_UI_PracticeMenu_Button_1_C::UpdateData`) work fine on 3.0.1.
- RegisterHook signature: `RegisterHook(path, pre_fn[, post_fn])`. The
  post-callback receives the return-value wrapper as its last arg and can
  call `:set(value)` to mutate it. Path syntax `/Game/<...>/<asset>.<Class>:<Fn>`
  for BP classes is correct.

The cure
--------
Hook the row widget's `UpdateData` POST-call. Identify our row by
`item.Text == "OpenDojo"`. Call:
```
self.TB_Menu_OFF:SetRawText("OpenDojo", false)
self.TB_Menu_ON :SetRawText("OpenDojo", false)
```
This runs after BP wrote the err-text into the TBs, so we overwrite with the
literal string. No Gryphon dependency, no flicker (synchronous), persistent
across hovers (every UpdateData re-applies our override).

Hover/focus state-switches still call UpdateData → our hook re-fires → we
re-apply SetRawText. The `IV_Menu_OFF/ON` invalidation wrappers handle Slate
cache invalidation automatically because `SetRawText` updates the TB's FText
property and Slate marks the IV dirty.

Path for callback ("OpenDojo" click action)
-------------------------------------------
- Menu's `OnDecideButton1(int32 ID)` fires when a row is confirmed in pane 1.
- After our `AddButton1Data` insertion, our row is at index `ListItems.Num - 1`
  (probably 17 if always appended at end, or 0 if prepended — verify at runtime).
- Hook `WBP_UI_PracticeMenu_C:OnDecideButton1` pre-call. If `ID == our_row_idx`,
  suppress the original (return early) and fire our handler.

Cleaner alternative: pre-hook the menu's `OnDecideButton1`, identify our row
by hovering an instance of `BP_PracticeMenu_Button_1_Item_C` whose Text is
"OpenDojo" and capturing its index at insert time.

Open questions
--------------
- Does `OnDecideButton1` pre-hook support short-circuiting the original
  BP function (return without calling)? UE4SS docs are unclear.
- When a row is appended via AddButton1Data, what's the exact index passed
  to `OnDecideButton1`? Verify experimentally — likely Num-1 (= 17 with the
  current 18-item list).
- Is the row appended at top or bottom of the visible list? Empirically the
  "OpenDojo" row appeared in the menu; need to confirm position.

Subagent reports — full text
----------------------------
Saved separately under the same date in the session transcript. Headline:

- Sub A (header mining): SetRawText is the bypass, listed exact .hpp lines.
- Sub B (Ghidra): Gryphon resolver dispatch + err construct location identified.
- Sub C (UE4SS): RegisterHook 3.0.1 BP-lib freeze is the known bug #467,
  per-instance hooks work, post-callback is arg #3.
- Sub D (CXX dump): Confirmed SetRawText path + InvalidationBox auto-handles
  cache invalidation after the FText property changes.
