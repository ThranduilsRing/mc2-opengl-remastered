// G7b K3: per-frame render clock + render list storage (see render_view.h), and the GameObject defaults.
#include "render_view.h"
#include "gameobj.h"

namespace mc2::render_view {

namespace {
RenderClock g_clock;
RenderList g_list;
}  // namespace

RenderClock& clock() { return g_clock; }
RenderList& list() { return g_list; }

}  // namespace mc2::render_view

void GameObject::extractRenderView(mc2::render_view::RenderView& view)
{
	view.position = position;
	view.watchId = getWatchID(false);
	view.objClass = static_cast<uint8_t>(getObjectClass());
}

void GameObject::renderView(const mc2::render_view::RenderView&)
{
	render();
}
