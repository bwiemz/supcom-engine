#pragma once

// Build templates (Moho's CWldSession build template, SBuildTemplateInfo):
// structures as the player laid them out, to be built again together.

#include "core/types.hpp"

#include <array>
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

} // namespace osc::renderer
