#pragma once

// Practice-menu integration — DLL-native.
//
// Responsibilities:
//   1. Auto-insert an "OpenDojo" row into the practice pause menu's
//      left pane every time the menu materializes. Done via reflection:
//      pattern-pin findUnrealClass + GetObjectsOfClass, find the live
//      WBP_UI_PracticeMenu_C instance, ProcessEvent-invoke
//      AddButton1Data("OpenDojo", true) + UpdateListView1().
//   2. Apply UPolarisTextBlock::SetRawText each frame to the inserted
//      row's TB_Menu_OFF / TB_Menu_ON so Gryphon's "err(OpenDojo)@@@@"
//      wrapper is continuously overwritten — survives hover/focus
//      state transitions that re-fire BP UpdateData.
//   3. Intercept the click via a MinHook detour on
//      UObject::ProcessEvent (vtable slot 77). When OnDecideButton1
//      fires with our row's ID, toggle the ImGui menu and let the
//      orig fall through (BP switch no-ops for our index).
//
// Determining our row index without ListView_1 (FProperty not
// resolvable on the menu class for reasons not yet understood):
// count live BP_PracticeMenu_Button_1_Item_C instances before
// AddButton1Data. UListView retains items as UPROPERTY, so the live
// count equals ListItems.Num. The item class is discovered from any
// existing row widget's list_item->ClassPrivate field.

namespace opendojo::practice_menu {

bool ensure_resolved();
void tick();

}  // namespace opendojo::practice_menu
