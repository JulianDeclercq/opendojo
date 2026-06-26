#pragma once

// Renames the practice-menu "CPU Opponent Action N" rows to
// "OpenDojo Action N" by intercepting the Polaris/Gryphon text pipeline.
//
// Why a hook and not a memory poke: the row labels are not compiled
// literals — they are decoded fresh from the encoded Gryphon localization
// asset on every menu refresh, and Slate caches the resolved text. Writing
// the UTF-16 buffers therefore neither holds (re-decoded) nor repaints
// (cached). The only durable interception point is the text setter
// (UPolarisTextBlock::SetRawText / SetTextID). See docs/NATIVE_MENU_FINDINGS.md.

namespace opendojo::practice_rename {

// Per-frame pump. Call from inside the practice-mode gate. Cheap after the
// menu rows are captured (event-driven re-apply via the SetTextID patch).
void tick();

// Call on the not-in-practice -> in-practice edge. Invalidates cached UObjects
// (the practice-menu Blueprint class + Gryphon CDO) that the engine reloads
// across a match, so the rename re-resolves instead of silently stopping.
void on_practice_reentry();

}  // namespace opendojo::practice_rename
