#pragma once

#include "renderer/vk_types.hpp"
#include "renderer/ui_renderer.hpp" // UIInstance, UIDrawGroup, ClipRect
#include "renderer/frustum.hpp"
#include "core/types.hpp"
#include "sim/build_placement.hpp"

#include <array>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace osc::sim {
class FrameView;
struct WorldEvents;
struct EntityRecord;
struct Vector3;
}

namespace osc::renderer {

struct BuildGhost;

class BeamRenderer;
class TrailRenderer;
class ParticleSystem;
class Camera;
class ReconView;
class TextureCache;

/// Every intel type a selected unit can show a range ring for.
inline const std::unordered_set<std::string> kAllIntelRingTypes{"Radar", "Sonar", "Omni",
                                                                 "Vision"};

/// A screen line `thick` either side of the segment from (x0, y0) to
/// (x1, y1), as the axis-aligned rects (x, y, w, h) the overlay draws: one
/// along the line's main axis per step of its other, no more than
/// `max_runs` of them.
std::vector<std::array<f32, 4>> line_runs(f32 x0, f32 y0, f32 x1, f32 y1, f32 thick,
                                          u32 max_runs = 256);

/// Intel types to show rings for, given FA's active range-overlay filters:
/// the RangeOverlayParams names multifunction.lua passes to
/// SetOverlayFilters ("Radar", "Sonar", "Omni", or "AllIntel" for all
/// three). Military and counter-intel filters have no ring here.
std::unordered_set<std::string> intel_ring_types_for_filters(
    const std::vector<std::string>& filters);

/// A convex quad on screen (corners in order) as rows `row` high, each
/// {x, y, w, h}
std::vector<std::array<f32, 4>> convex_rows(const std::array<f32, 4>& xs,
                                            const std::array<f32, 4>& ys, f32 row);

/// The pads (skirts) of the structures standing or under way that the
/// player sees, each with its height, outlined while a structure is placed
std::vector<std::pair<sim::StructureSite, f32>> structure_pads(const sim::FrameView& view,
                                                               const ReconView* recon);

/// Which of `pads` a valid ghost on `ghost` lights up: those it touches
std::vector<bool> adjacency_lit(const std::vector<sim::StructureSite>& pads,
                                const sim::StructureSite& ghost);

/// The outline of a quad on screen (corners in order), `thickness` wide, as
/// convex_rows' rows of a pixel
std::vector<std::array<f32, 4>> outline_rows(const std::array<f32, 4>& xs,
                                             const std::array<f32, 4>& ys, f32 thickness);

/// Renders game overlays: health bars, selection rings, command lines.
/// Uses the same UI pipeline (UIInstance quads, pixel coords, fallback white texture).
class OverlayRenderer {
public:
    void init(VkDevice device, VmaAllocator allocator);

    /// Build overlay quads from the world as `view` draws it (between the
    /// last two ticks), the selection and the camera. Death flashes come
    /// from `events`, which this takes.
    /// game_result: 0=in progress, 1=victory, 2=defeat, 3=draw.
    void update(const sim::FrameView& view, sim::WorldEvents& events, const Camera& camera,
                const std::array<f32, 16>& vp_matrix, const std::unordered_set<u32>* selected_ids,
                TextureCache& tex_cache, u32 viewport_w, u32 viewport_h, i32 game_result = 0,
                f32 dt = 0.0f, const Frustum* frustum = nullptr, const BuildGhost* ghost = nullptr);

    /// Issue draw calls. Caller must have the UI pipeline bound.
    void render(VkCommandBuffer cmd, VkPipelineLayout layout,
                u32 viewport_w, u32 viewport_h);

    void destroy(VkDevice device, VmaAllocator allocator);

    void set_frame_index(u32 fi) { fi_ = fi; }
    /// This frame's quads (the render-state dump reads them).
    const std::vector<UIInstance>& quads() const { return quads_; }

    /// Intel types whose range rings selected units show ("Radar", "Sonar",
    /// "Omni", "Vision"). FA shows them per its range-overlay filters; the
    /// C++ HUD (no FA game UI, or --legacy-hud) shows them all.
    void set_intel_ring_types(std::unordered_set<std::string> types) {
        intel_ring_types_ = std::move(types);
    }

    u32 quad_count() const { return quad_count_; }

    /// ui_RenderUnitBars: the units' bars (health, progress, and the marks
    /// drawn with them); a campaign's NIS turns them off.
    void set_unit_bars(bool on) { unit_bars_ = on; }
    bool unit_bars() const { return unit_bars_; }
    /// ren_SelectBoxes: the selection's marks; a NIS turns them off too.
    void set_select_boxes(bool on) { select_boxes_ = on; }
    bool select_boxes() const { return select_boxes_; }

    /// The player's intel: a unit's health bar, selection ring and work
    /// beams show only while it is in sight, a death's flash only where the
    /// player's army sees (null: everything seen; M215a).
    void set_recon(const ReconView* recon) { recon_ = recon; }
    /// The unit under the cursor: another army's shows its lifebars then.
    void set_hovered(u32 id) { hovered_ = id; }

    /// FA's beams: a beam they draw needs no placeholder here (M214a).
    void set_beams(const BeamRenderer* beams) { beams_ = beams; }
    /// FA's trails: nor does a trail they draw (M214b).
    void set_trails(const TrailRenderer* trails) { trails_ = trails; }
    /// FA's particles: nor does an emitter they draw (M214c).
    void set_particles(const ParticleSystem* particles) { particles_ = particles; }

    static constexpr u32 MAX_OVERLAY_QUADS = 8192;
    static constexpr u32 FRAMES_IN_FLIGHT = 2;

private:
    std::unordered_set<std::string> intel_ring_types_ = kAllIntelRingTypes;
    bool unit_bars_ = true;
    bool select_boxes_ = true;
    const ReconView* recon_ = nullptr;
    u32 hovered_ = 0;
    const BeamRenderer* beams_ = nullptr;
    const TrailRenderer* trails_ = nullptr;
    const ParticleSystem* particles_ = nullptr;
    /// Project world position to screen pixel coordinates.
    /// Returns false if behind camera.
    static bool world_to_screen(f32 wx, f32 wy, f32 wz, const std::array<f32, 16>& vp, f32 sw,
                                f32 sh, f32& out_x, f32& out_y);

    void emit_quad(f32 x, f32 y, f32 w, f32 h,
                   f32 r, f32 g, f32 b, f32 a);
    /// A quad from (left, top) to (right, bottom) in an 0xAARRGGBB colour.
    void emit_argb(f32 left, f32 top, f32 right, f32 bottom, u32 argb);
    /// A unit's lifebar stack, as Moho's DrawUnitLifebars (faf-re), if it
    /// gets one; `sim_time` in ticks.
    void emit_lifebars(const sim::EntityRecord& e, const sim::Vector3& pos, const Camera& camera,
                       const std::array<f32, 16>& vp, f32 sw, f32 sh, f64 sim_time);
    /// A line, as line_runs lays it
    void emit_line(f32 x0, f32 y0, f32 x1, f32 y1, f32 thick, f32 r, f32 g, f32 b, f32 a);
    /// A placement outline's width, pixels
    static constexpr f32 kOutlineThickness = 2.0f;
    void emit_outline(const std::array<f32, 4>& xs, const std::array<f32, 4>& ys, f32 r, f32 g,
                      f32 b, f32 a);

    AllocatedBuffer instance_buf_[FRAMES_IN_FLIGHT] = {};
    void* instance_mapped_[FRAMES_IN_FLIGHT] = {};
    u32 fi_ = 0;

    std::vector<UIInstance> quads_;
    u32 quad_count_ = 0;

    VkDescriptorSet white_ds_ = VK_NULL_HANDLE;

    // Active explosion effects (from death events)
    struct Explosion {
        f32 x, y, z;     // world position
        f32 scale;        // max radius
        f32 age;          // seconds since death (0..EXPLOSION_DURATION)
        f32 r, g, b;     // flash color
    };
    static constexpr f32 EXPLOSION_DURATION = 0.6f;
    static constexpr u32 MAX_EXPLOSIONS = 64;
    std::vector<Explosion> explosions_;
};

} // namespace osc::renderer
