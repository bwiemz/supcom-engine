#pragma once

// Build templates (Moho's CWldSession build template, SBuildTemplateInfo):
// structures as the player laid them out, to be built again together.

#include "core/types.hpp"

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace osc::renderer {

/// One structure of a template: its blueprint, where it comes in the build
/// order, and where it stands from the template's first.
struct BuildTemplateEntry {
    std::string blueprint_id;
    i32 build_order = 0;
    f32 x = 0, z = 0;
};

/// The active template: its width and depth (what a drag spaces copies by)
/// and its structures, in build order. Lua sees it as
/// {spanX, spanZ, {bpId, buildOrder, x, z}, ...}.
struct BuildTemplate {
    f32 span_x = 0, span_z = 0;
    std::vector<BuildTemplateEntry> entries;
};

/// A structure a template may be made from.
struct TemplateStructure {
    std::string blueprint_id;
    u32 creation_tick = 0;
    f32 x = 0, z = 0;
    f32 foot_x = 1, foot_z = 1;                                     ///< its blueprint's Footprint
    f32 skirt_x = 0, skirt_z = 0, skirt_off_x = 0, skirt_off_z = 0; ///< Physics.Skirt*
};

/// RUnitBlueprint::GetSkirtRect at the structure's position: x0, z0, x1, z1.
std::array<f32, 4> skirt_rect(const TemplateStructure& s);

/// GenerateBuildTemplates: the structures as a template, in the order they
/// were made, each where it stands from the first, the whole as wide and
/// deep as their skirts. Nothing for no structures.
std::optional<BuildTemplate>
generate_build_template(const std::vector<TemplateStructure>& structures);

/// One structure of a template as placed: its blueprint and centre.
struct TemplateSite {
    std::string blueprint_id;
    f32 x = 0, z = 0;
};

/// A blueprint's footprint (SizeX, SizeZ).
using FootprintOf = std::function<std::array<f32, 2>(const std::string& blueprint_id)>;

/// Where the structures of `t` go for a build drag pressed at (x0, z0) and
/// released at (x1, z1) -- a click is one of no length -- as Moho lays a
/// template (CBuildDragPreview::UpdateDragPreview, IssueBuildDragOrders):
/// copy by copy, then entry by entry in the template's order.
///
/// A copy's anchor is the centre of the 1x1 cell under the lead entry's
/// footprint (its corner cell, lrint(p - size / 2), + 0.5); each entry goes
/// its offset from that, snapped by its own footprint. A drag (`drag`: the
/// lead is DRAGBUILD) lays copies a span apart along its longer axis --
/// spanX if it runs more in x, else spanZ -- the other axis following in
/// proportion, floor(length / span) + 1 of them. (Moho's preview truncates
/// the other axis where its issue rounds; both round here, so a diagonal
/// drag shows what it issues.)
std::vector<TemplateSite> template_sites(const BuildTemplate& t, f32 x0, f32 z0, f32 x1, f32 z1,
                                         bool drag, const FootprintOf& footprint);

} // namespace osc::renderer
