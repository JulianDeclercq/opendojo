#pragma once

// ImGui menu surface. Drawn from the render hook each frame the menu is
// visible. State (drill list cache, form buffers, toast) is owned by the
// .cpp — there's only one instance, lifetime equals process lifetime.

namespace opendojo::menu {

// Draw one frame of the menu. Caller must already have called ImGui::NewFrame
// for this frame and will call ImGui::Render afterward.
void draw();

// Force a re-scan of opendojo_drills/ on the next draw. Cheap to call.
void invalidate();

}  // namespace opendojo::menu
