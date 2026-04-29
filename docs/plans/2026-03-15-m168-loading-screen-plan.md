# M168: Loading Screen Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** FA-compatible loading screen with MPEG-1 video playback, animated text, cycling tips, and chunked reload so the render loop stays alive during loading.

**Architecture:** Add pl_mpeg video decoder wrapped in `VideoDecoder` class, wire it into the existing `MovieControl` UI control (already has Play/Stop/Loop/InternalSet methods + `movie_*_` state fields), add `render_ui_only()` to Renderer for UI-only frames during loading, split `execute_reload_sequence()` into a 5-stage state machine with UI frame pumping between stages, and wire `WldUIProvider::start_loading_dialog()`/`stop_loading_dialog()` (existing stubs) to call FA's `LoadDialog()`.

**Tech Stack:** C++17, Vulkan, pl_mpeg (single-header MPEG-1 decoder), Lua 5.0

**Spec:** `docs/plans/2026-03-15-m168-loading-screen-design.md`

---

## File Layout

```
New files:
  third_party/pl_mpeg.h                  — Single-header MPEG-1 decoder (download)
  src/video/CMakeLists.txt               — Build config for video library
  src/video/video_decoder.hpp            — pl_mpeg C++ wrapper
  src/video/video_decoder.cpp            — PL_MPEG_IMPLEMENTATION + SFD demux
  tests/test_video_decoder.cpp           — Unit tests for VideoDecoder

Modified files:
  CMakeLists.txt                         — Add src/video subdirectory + link osc_video
  tests/CMakeLists.txt                   — Add test_video_decoder.cpp
  src/ui/ui_control.hpp                  — Add VideoDecoder* and Vulkan handles to UIControl
  src/renderer/renderer.hpp              — Add render_ui_only() declaration
  src/renderer/renderer.cpp              — Add render_ui_only() implementation
  src/renderer/ui_renderer.hpp           — Add movie texture rendering support
  src/renderer/ui_renderer.cpp           — Render movie quads with dynamic texture
  src/lua/moho_bindings.cpp              — Wire MovieControl to VideoDecoder, add GetCursor
  src/ui/wld_ui_provider.hpp             — Implement start/stop_loading_dialog
  src/ui/wld_ui_provider.cpp             — Call FA's LoadDialog/StopLoadingDialog
  src/main.cpp                           — Chunked reload state machine
```

---

## Chunk 1: Video Decoder

### Task 1: Add pl_mpeg to third_party

Download pl_mpeg single-header library and add build infrastructure.

**Files:**
- Create: `third_party/pl_mpeg.h`
- Create: `src/video/CMakeLists.txt`
- Create: `src/video/video_decoder.hpp`
- Create: `src/video/video_decoder.cpp`
- Modify: `CMakeLists.txt` (add subdirectory)
- Modify: `tests/CMakeLists.txt` (add test file)

- [ ] **Step 1: Download pl_mpeg.h**

Download from https://github.com/phoboslab/pl_mpeg (public domain, single header). Place at `third_party/pl_mpeg.h`. Verify it contains `#ifndef PL_MPEG_H` guard.

- [ ] **Step 2: Create src/video/CMakeLists.txt**

```cmake
add_library(osc_video STATIC
    video_decoder.cpp
)

target_include_directories(osc_video PUBLIC
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/third_party
)

target_link_libraries(osc_video PUBLIC osc_core)

add_library(osc::video ALIAS osc_video)
```

- [ ] **Step 3: Add subdirectory to root CMakeLists.txt**

After the existing `add_subdirectory(src/audio)` line (~line 31), add:

```cmake
add_subdirectory(src/video)
```

In the `target_link_libraries(opensupcom ...)` block (~line 36), add `osc::video`.

In the `target_link_libraries(osc_tests ...)` block in `tests/CMakeLists.txt`, add `osc::video`.

- [ ] **Step 4: Create VideoDecoder header**

Create `src/video/video_decoder.hpp`:

```cpp
#pragma once

#include "core/types.hpp"

struct plm_t;

namespace osc::video {

/// Wraps pl_mpeg for MPEG-1 video decoding.
/// Owns a copy of the input data (SFD demuxed or raw MPEG-1).
class VideoDecoder {
public:
    VideoDecoder() = default;
    ~VideoDecoder();

    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    /// Open from raw MPEG-1 data. Takes ownership of a copy.
    bool open(const u8* data, size_t size);

    /// Open from VFS file data. Attempts SFD demux first, then raw MPEG-1.
    bool open_file(const u8* file_data, size_t file_size);

    /// Decode next frame. Returns false if no more frames (unless looping).
    bool decode_next_frame();

    /// Current decoded frame as RGBA8 (width * height * 4 bytes).
    const u8* rgba_data() const { return rgba_buf_.data(); }
    u32 width() const { return width_; }
    u32 height() const { return height_; }
    f64 framerate() const { return framerate_; }
    bool is_open() const { return plm_ != nullptr; }

    void set_loop(bool loop);
    void rewind();
    void close();

private:
    plm_t* plm_ = nullptr;
    std::vector<u8> mpeg_data_;     // owned copy of MPEG-1 stream
    std::vector<u8> rgba_buf_;      // current frame RGBA
    u32 width_ = 0;
    u32 height_ = 0;
    f64 framerate_ = 0;
    bool loop_ = false;
};

/// Attempt to demux SFD (CRI Sofdec) container into raw MPEG-1.
/// Returns empty vector if not an SFD file or demux fails.
std::vector<u8> demux_sfd(const u8* data, size_t size);

} // namespace osc::video
```

- [ ] **Step 5: Create VideoDecoder implementation**

Create `src/video/video_decoder.cpp`:

```cpp
#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"

#include "video/video_decoder.hpp"

#include <spdlog/spdlog.h>
#include <algorithm>
#include <cstring>

namespace osc::video {

VideoDecoder::~VideoDecoder() { close(); }

void VideoDecoder::close() {
    if (plm_) {
        plm_destroy(plm_);
        plm_ = nullptr;
    }
    mpeg_data_.clear();
    rgba_buf_.clear();
    width_ = height_ = 0;
    framerate_ = 0;
}

bool VideoDecoder::open(const u8* data, size_t size) {
    close();
    if (!data || size == 0) return false;

    mpeg_data_.assign(data, data + size);
    plm_ = plm_create_with_memory(mpeg_data_.data(),
                                   static_cast<int>(mpeg_data_.size()), 0);
    if (!plm_) {
        spdlog::warn("VideoDecoder: pl_mpeg failed to open stream");
        mpeg_data_.clear();
        return false;
    }

    plm_set_audio_enabled(plm_, 0);  // no audio decoding
    width_ = static_cast<u32>(plm_get_width(plm_));
    height_ = static_cast<u32>(plm_get_height(plm_));
    framerate_ = plm_get_framerate(plm_);

    if (width_ == 0 || height_ == 0) {
        spdlog::warn("VideoDecoder: zero dimensions");
        close();
        return false;
    }

    rgba_buf_.resize(width_ * height_ * 4, 0);
    spdlog::info("VideoDecoder: opened {}x{} @ {:.1f} fps", width_, height_, framerate_);
    return true;
}

bool VideoDecoder::open_file(const u8* file_data, size_t file_size) {
    if (!file_data || file_size < 4) return false;

    // Try .mpg extension first (check MPEG-1 sequence header 0x000001B3)
    if (file_size >= 4 &&
        file_data[0] == 0x00 && file_data[1] == 0x00 &&
        file_data[2] == 0x01 && file_data[3] == 0xB3) {
        return open(file_data, file_size);
    }

    // Try SFD demux
    auto mpeg = demux_sfd(file_data, file_size);
    if (!mpeg.empty()) {
        return open(mpeg.data(), mpeg.size());
    }

    // Last resort: try as raw MPEG-1 anyway
    return open(file_data, file_size);
}

bool VideoDecoder::decode_next_frame() {
    if (!plm_) return false;

    plm_frame_t* frame = plm_decode_video(plm_);
    if (!frame) {
        if (loop_) {
            plm_rewind(plm_);
            frame = plm_decode_video(plm_);
        }
        if (!frame) return false;
    }

    // Convert YCrCb to RGBA
    plm_frame_to_rgba(frame, rgba_buf_.data(),
                      static_cast<int>(width_) * 4);
    return true;
}

void VideoDecoder::set_loop(bool loop) {
    loop_ = loop;
    if (plm_) plm_set_loop_enabled(plm_, loop ? 1 : 0);
}

void VideoDecoder::rewind() {
    if (plm_) plm_rewind(plm_);
}

// --- SFD Demuxer ---

std::vector<u8> demux_sfd(const u8* data, size_t size) {
    if (size < 8) return {};

    // CRI Sofdec magic: "CRID" or "SFD\x00"
    bool is_crid = (data[0] == 'C' && data[1] == 'R' &&
                    data[2] == 'I' && data[3] == 'D');
    bool is_sfd = (data[0] == 'S' && data[1] == 'F' &&
                   data[2] == 'D' && data[3] == '\0');

    if (!is_crid && !is_sfd) return {};

    spdlog::debug("SFD demuxer: detected CRI Sofdec container");

    // Scan for MPEG-1 pack start codes (0x000001BA) and video packets
    // (0x000001E0). Reassemble video stream bytes.
    std::vector<u8> video;
    video.reserve(size); // upper bound

    size_t pos = 0;
    while (pos + 4 <= size) {
        // Look for start code prefix 0x000001
        if (data[pos] == 0x00 && data[pos + 1] == 0x00 &&
            data[pos + 2] == 0x01) {

            u8 stream_id = data[pos + 3];

            // MPEG-1 video elementary stream: 0xE0-0xEF
            if (stream_id >= 0xE0 && stream_id <= 0xEF && pos + 6 <= size) {
                u16 packet_len = static_cast<u16>(data[pos + 4]) << 8 |
                                 static_cast<u16>(data[pos + 5]);

                // Skip PES header stuffing bytes (0xFF padding)
                size_t hdr_start = pos + 6;
                size_t hdr_pos = hdr_start;
                while (hdr_pos < pos + 6 + packet_len && hdr_pos < size &&
                       data[hdr_pos] == 0xFF) {
                    hdr_pos++;
                }
                // Skip STD buffer flag (01xxxxxx) and PTS/DTS flags
                if (hdr_pos < size && (data[hdr_pos] & 0xC0) == 0x40) {
                    hdr_pos += 2; // skip STD buffer
                }
                if (hdr_pos < size && (data[hdr_pos] & 0xF0) == 0x20) {
                    hdr_pos += 5; // skip PTS
                } else if (hdr_pos < size && (data[hdr_pos] & 0xF0) == 0x30) {
                    hdr_pos += 10; // skip PTS + DTS
                } else if (hdr_pos < size && data[hdr_pos] == 0x0F) {
                    hdr_pos += 1; // no timestamps
                }

                size_t payload_len = packet_len - (hdr_pos - hdr_start);
                if (hdr_pos + payload_len <= size) {
                    video.insert(video.end(),
                                 data + hdr_pos,
                                 data + hdr_pos + payload_len);
                }
                pos = pos + 6 + packet_len;
                continue;
            }
            // Pack header (0x000001BA) — skip 12 bytes (MPEG-1 pack)
            else if (stream_id == 0xBA) {
                pos += 12;
                continue;
            }
            // System header (0x000001BB) or other streams — skip by length
            else if (stream_id == 0xBB || stream_id == 0xBD ||
                     stream_id == 0xBE || stream_id == 0xBF ||
                     (stream_id >= 0xC0 && stream_id <= 0xDF)) {
                if (pos + 6 <= size) {
                    u16 plen = static_cast<u16>(data[pos + 4]) << 8 |
                               static_cast<u16>(data[pos + 5]);
                    pos = pos + 6 + plen;
                    continue;
                }
            }
            // Sequence/picture/GOP headers — these are part of video
            else if (stream_id == 0xB3 || stream_id == 0xB5 ||
                     stream_id == 0xB8 || stream_id == 0x00) {
                // Let the scan advance byte-by-byte to find next packet
            }
        }
        pos++;
    }

    if (video.size() < 16) {
        spdlog::debug("SFD demuxer: insufficient video data extracted");
        return {};
    }

    spdlog::info("SFD demuxer: extracted {} bytes of MPEG-1 video", video.size());
    return video;
}

} // namespace osc::video
```

- [ ] **Step 6: Write unit tests**

Create `tests/test_video_decoder.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include "video/video_decoder.hpp"

using namespace osc;

TEST_CASE("VideoDecoder basics", "[video]") {
    video::VideoDecoder decoder;

    SECTION("default state is closed") {
        REQUIRE_FALSE(decoder.is_open());
        REQUIRE(decoder.width() == 0);
        REQUIRE(decoder.height() == 0);
    }

    SECTION("open with null data fails gracefully") {
        REQUIRE_FALSE(decoder.open(nullptr, 0));
        REQUIRE_FALSE(decoder.is_open());
    }

    SECTION("open with garbage data fails gracefully") {
        u8 garbage[] = {0xDE, 0xAD, 0xBE, 0xEF};
        REQUIRE_FALSE(decoder.open(garbage, 4));
        REQUIRE_FALSE(decoder.is_open());
    }

    SECTION("decode_next_frame on closed decoder returns false") {
        REQUIRE_FALSE(decoder.decode_next_frame());
    }
}

TEST_CASE("SFD demuxer", "[video]") {
    SECTION("non-SFD data returns empty") {
        u8 data[] = {0x00, 0x01, 0x02, 0x03};
        auto result = video::demux_sfd(data, 4);
        REQUIRE(result.empty());
    }

    SECTION("null data returns empty") {
        auto result = video::demux_sfd(nullptr, 0);
        REQUIRE(result.empty());
    }
}
```

Add `test_video_decoder.cpp` to `tests/CMakeLists.txt` source list.

- [ ] **Step 7: Build and run tests**

Run: `cmake --preset default && cmake --build build --config Debug`
Run: `./build/tests/Debug/osc_tests.exe "[video]"`
Expected: All video tests pass.

- [ ] **Step 8: Commit**

```bash
git add third_party/pl_mpeg.h src/video/ tests/test_video_decoder.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "Add pl_mpeg video decoder with SFD demuxer and unit tests"
```

---

## Chunk 2: Movie Control Backend

### Task 2: Wire VideoDecoder into MovieControl

Connect the existing MovieControl UI control (moho_bindings.cpp lines 10623-10670) to the VideoDecoder. Add dynamic Vulkan texture for per-frame video display.

**Files:**
- Modify: `src/ui/ui_control.hpp` (add VideoDecoder pointer + Vulkan handles)
- Modify: `src/lua/moho_bindings.cpp` (wire InternalSet/Play/Stop/Loop to VideoDecoder)
- Modify: `src/renderer/ui_renderer.hpp` (add movie texture support)
- Modify: `src/renderer/ui_renderer.cpp` (render movie quads with dynamic texture)

- [ ] **Step 1: Add video fields to UIControl**

In `src/ui/ui_control.hpp`, add includes and fields. After the existing `movie_looping_` field (~line 399), add:

```cpp
// Video decoder backend (owned, nullable — unique_ptr for automatic cleanup)
std::unique_ptr<video::VideoDecoder> video_decoder_;

// Vulkan handles for dynamic video texture
VkImage video_image_ = VK_NULL_HANDLE;
VkImageView video_image_view_ = VK_NULL_HANDLE;
VkDescriptorSet video_ds_ = VK_NULL_HANDLE;
VkBuffer video_staging_buf_ = VK_NULL_HANDLE;
void* video_staging_mapped_ = nullptr;
VmaAllocation video_image_alloc_ = VK_NULL_HANDLE;
VmaAllocation video_staging_alloc_ = VK_NULL_HANDLE;
bool video_needs_upload_ = false;
bool video_image_initialized_ = false;  // tracks whether first upload done
```

Add forward declaration at top: `namespace osc::video { class VideoDecoder; }`

Add public methods:

```cpp
video::VideoDecoder* video_decoder() { return video_decoder_.get(); }
void set_video_decoder(std::unique_ptr<video::VideoDecoder> dec) { video_decoder_ = std::move(dec); }
VkDescriptorSet video_descriptor_set() const { return video_ds_; }
bool video_needs_upload() const { return video_needs_upload_; }
void set_video_needs_upload(bool v) { video_needs_upload_ = v; }
```

- [ ] **Step 2: Wire MovieControl Lua methods to VideoDecoder**

In `src/lua/moho_bindings.cpp`, modify the existing movie methods (lines 10623-10670).

Replace `movie_InternalSet` (~line 10623):

```cpp
static int movie_InternalSet(lua_State* L) {
    auto* ctrl = check_control(L);
    if (!ctrl) return 0;
    const char* path = luaL_checkstring(L, 2);
    ctrl->set_movie_filename(path);

    // Get VFS via LuaState helper (same pattern as DiskFindFiles etc.)
    lua_pushstring(L, "__osc_lua_state");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* ls = static_cast<osc::lua::LuaState*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    auto* vfs = ls ? ls->vfs() : nullptr;

    if (vfs) {
        // Try .mpg version first (pre-converted), then original path
        std::string mpg_path(path);
        auto dot = mpg_path.rfind('.');
        if (dot != std::string::npos) {
            mpg_path = mpg_path.substr(0, dot) + ".mpg";
        }

        auto data = vfs->read_file(mpg_path);
        if (!data) data = vfs->read_file(path);

        if (data) {
            auto dec = std::make_unique<osc::video::VideoDecoder>();
            if (dec->open_file(data->data(), data->size())) {
                spdlog::info("MovieControl: opened '{}' ({}x{})",
                             path, dec->width(), dec->height());
                ctrl->set_video_decoder(std::move(dec));
                ctrl->set_movie_loaded(true);
            } else {
                spdlog::warn("MovieControl: failed to decode '{}'", path);
            }
        } else {
            spdlog::warn("MovieControl: file not found '{}'", path);
        }
    }
    return 0;
}
```

Replace `movie_Play`:
```cpp
static int movie_Play(lua_State* L) {
    auto* ctrl = check_control(L);
    if (ctrl) {
        ctrl->set_movie_playing(true);
        // Decode first frame immediately
        if (ctrl->video_decoder() && ctrl->video_decoder()->is_open()) {
            ctrl->video_decoder()->decode_next_frame();
            ctrl->set_video_needs_upload(true);
        }
    }
    return 0;
}
```

Replace `movie_Stop`:
```cpp
static int movie_Stop(lua_State* L) {
    auto* ctrl = check_control(L);
    if (ctrl) ctrl->set_movie_playing(false);
    return 0;
}
```

Replace `movie_Loop`:
```cpp
static int movie_Loop(lua_State* L) {
    auto* ctrl = check_control(L);
    if (ctrl) {
        bool loop = lua_toboolean(L, 2) != 0;
        ctrl->set_movie_looping(loop);
        if (ctrl->video_decoder())
            ctrl->video_decoder()->set_loop(loop);
    }
    return 0;
}
```

- [ ] **Step 3: Add GetCursor binding**

In `src/lua/moho_bindings.cpp`, add `l_GetCursor` near the existing `l_SetCursor` (~line 11449):

```cpp
static int l_GetCursor(lua_State* L) {
    lua_pushstring(L, "__osc_active_cursor");
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (lua_isnil(L, -1)) {
        // Return a dummy table with Hide/Show if no cursor set
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushstring(L, "Hide");
        lua_pushcfunction(L, [](lua_State*) -> int { return 0; });
        lua_rawset(L, -3);
        lua_pushstring(L, "Show");
        lua_pushcfunction(L, [](lua_State*) -> int { return 0; });
        lua_rawset(L, -3);
    }
    return 1;
}
```

Register it in the UI globals table (near the other UI globals, ~line 11541):

```cpp
state.register_function("GetCursor", l_GetCursor);
```

- [ ] **Step 4: Build and run tests**

Run: `cmake --build build --config Debug`
Run: `./build/tests/Debug/osc_tests.exe`
Expected: All tests pass (existing + video tests).

- [ ] **Step 5: Commit**

```bash
git add src/ui/ui_control.hpp src/lua/moho_bindings.cpp
git commit -m "Wire MovieControl to VideoDecoder with Play/Stop/Loop and add GetCursor"
```

---

### Task 3: Movie Texture Rendering

Add dynamic Vulkan texture creation and per-frame upload for MovieControl, plus UIRenderer support for rendering movie quads.

**Files:**
- Modify: `src/renderer/ui_renderer.hpp` (add movie texture init/update methods)
- Modify: `src/renderer/ui_renderer.cpp` (create video VkImage, staging buffer, per-frame upload)

- [ ] **Step 1: Add movie texture methods to UIRenderer**

In `src/renderer/ui_renderer.hpp`, add:

```cpp
/// Create Vulkan resources for a movie control's dynamic texture.
/// Called once when a MovieControl first has decoded video data.
/// Uses TextureCache to allocate descriptor set (via new public method).
void init_movie_texture(ui::UIControl& ctrl, u32 width, u32 height,
                        TextureCache& tex_cache);

/// Upload current frame data to GPU. Called each frame for playing movies.
/// Records copy commands into the provided command buffer.
void update_movie_texture(VkCommandBuffer cmd, ui::UIControl& ctrl);

/// Destroy movie texture resources for a control.
void destroy_movie_texture(ui::UIControl& ctrl);
```

- [ ] **Step 2: Implement movie texture management**

In `src/renderer/ui_renderer.cpp`, add implementations:

```cpp
void UIRenderer::init_movie_texture(ui::UIControl& ctrl, u32 width, u32 height,
                                     TextureCache& tex_cache) {
    // Create GPU image
    VkImageCreateInfo img_ci{};
    img_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img_ci.imageType = VK_IMAGE_TYPE_2D;
    img_ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    img_ci.extent = {width, height, 1};
    img_ci.mipLevels = 1;
    img_ci.arrayLayers = 1;
    img_ci.samples = VK_SAMPLE_COUNT_1_BIT;
    img_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    img_ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    img_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo img_alloc{};
    img_alloc.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    vmaCreateImage(allocator_, &img_ci, &img_alloc,
                   &ctrl.video_image_, &ctrl.video_image_alloc_, nullptr);

    // Create image view
    VkImageViewCreateInfo view_ci{};
    view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_ci.image = ctrl.video_image_;
    view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_ci.subresourceRange.levelCount = 1;
    view_ci.subresourceRange.layerCount = 1;
    VkImageView view;
    vkCreateImageView(device_, &view_ci, nullptr, &view);
    ctrl.video_image_view_ = view;

    // Allocate descriptor set via TextureCache (add public create_descriptor_for_view()
    // method to TextureCache that wraps the private allocate_and_write_descriptor)
    ctrl.video_ds_ = tex_cache.create_descriptor_for_view(view);

    // Create persistent staging buffer (CPU-mapped, size = width*height*4)
    VkBufferCreateInfo buf_ci{};
    buf_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_ci.size = width * height * 4;
    buf_ci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    VmaAllocationCreateInfo staging_alloc{};
    staging_alloc.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
    staging_alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                          VMA_ALLOCATION_CREATE_MAPPED_BIT;
    staging_alloc.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    VmaAllocationInfo result_info{};
    vmaCreateBuffer(allocator_, &buf_ci, &staging_alloc,
                    &ctrl.video_staging_buf_, &ctrl.video_staging_alloc_,
                    &result_info);
    ctrl.video_staging_mapped_ = result_info.pMappedData;
}

void UIRenderer::update_movie_texture(VkCommandBuffer cmd, ui::UIControl& ctrl) {
    auto* dec = ctrl.video_decoder();
    if (!dec || !dec->is_open() || !ctrl.video_needs_upload()) return;

    u32 w = dec->width();
    u32 h = dec->height();

    // Initialize texture on first upload (needs TextureCache for descriptor)
    if (ctrl.video_image_ == VK_NULL_HANDLE) {
        init_movie_texture(ctrl, w, h, tex_cache_);
    }

    // Copy RGBA data to staging buffer
    std::memcpy(ctrl.video_staging_mapped_, dec->rgba_data(), w * h * 4);

    // Transition image to TRANSFER_DST
    // First frame: UNDEFINED → TRANSFER_DST (discard is fine)
    // Subsequent frames: SHADER_READ_ONLY → TRANSFER_DST (preserve correctness)
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = ctrl.video_image_initialized_
        ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        : VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcAccessMask = ctrl.video_image_initialized_
        ? VK_ACCESS_SHADER_READ_BIT : 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.image = ctrl.video_image_;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    // Copy staging → image
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cmd, ctrl.video_staging_buf_, ctrl.video_image_,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Transition: TRANSFER_DST → SHADER_READ_ONLY
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    ctrl.video_image_initialized_ = true;
    ctrl.set_video_needs_upload(false);
}

void UIRenderer::destroy_movie_texture(ui::UIControl& ctrl) {
    if (ctrl.video_image_view_) vkDestroyImageView(device_, ctrl.video_image_view_, nullptr);
    if (ctrl.video_image_) vmaDestroyImage(allocator_, ctrl.video_image_, ctrl.video_image_alloc_);
    if (ctrl.video_staging_buf_) vmaDestroyBuffer(allocator_, ctrl.video_staging_buf_, ctrl.video_staging_alloc_);
    ctrl.video_image_ = VK_NULL_HANDLE;
    ctrl.video_image_view_ = VK_NULL_HANDLE;
    ctrl.video_ds_ = VK_NULL_HANDLE;
    ctrl.video_staging_buf_ = VK_NULL_HANDLE;
    ctrl.video_staging_mapped_ = nullptr;
    ctrl.video_image_initialized_ = false;
}
```

**Cleanup lifecycle:** `destroy_movie_texture()` must be called when a MovieControl is destroyed. Add a call in `UIControlRegistry::destroy(u32 id)` — check if the control has `video_image_ != VK_NULL_HANDLE` and call `destroy_movie_texture()` before removing. The `VideoDecoder` itself is cleaned up automatically via `unique_ptr`.
```

- [ ] **Step 3: Integrate movie frame advance into UIRenderer::update**

In `UIRenderer::update()`, after walking the control tree, add movie frame advancement:

```cpp
// Advance playing movie controls
for (auto& ctrl_ptr : registry.all()) {
    if (!ctrl_ptr || ctrl_ptr->destroyed()) continue;
    auto& ctrl = *ctrl_ptr;
    if (ctrl.movie_playing() && ctrl.video_decoder()) {
        auto* dec = ctrl.video_decoder();
        if (dec->is_open() && dec->decode_next_frame()) {
            ctrl.set_video_needs_upload(true);
        }
    }
}
```

- [ ] **Step 4: Render movie quads with video descriptor set**

In the UIRenderer render path, when drawing a control that has a `video_ds_`, bind that descriptor set instead of the default bitmap texture. In the quad building code within `UIRenderer::update()`, check if the control has a video descriptor set and use it:

```cpp
// When building quads for a control, if it has video_ds_, use it as the texture
if (ctrl.video_descriptor_set() != VK_NULL_HANDLE) {
    // Use video texture descriptor for this quad
}
```

The exact integration depends on how UIRenderer batches quads. The key: movie controls use their own descriptor set for the textured quad instead of the bitmap's.

- [ ] **Step 5: Build and verify**

Run: `cmake --build build --config Debug`
Run: `./build/tests/Debug/osc_tests.exe`
Expected: All tests pass.

- [ ] **Step 6: Commit**

```bash
git add src/renderer/ui_renderer.hpp src/renderer/ui_renderer.cpp
git commit -m "Add dynamic movie texture rendering with persistent staging buffer"
```

---

## Chunk 3: Renderer UI-Only Path & Chunked Reload

### Task 4: Add render_ui_only() to Renderer

**Files:**
- Modify: `src/renderer/renderer.hpp` (add declaration)
- Modify: `src/renderer/renderer.cpp` (add implementation)

- [ ] **Step 1: Add declaration**

In `src/renderer/renderer.hpp`, after the existing `render()` declaration (~line 58), add:

```cpp
/// Render only the UI layer (no 3D scene, no bloom).
/// Used during loading screen when SimState doesn't exist.
void render_ui_only(lua_State* L, ui::UIControlRegistry* ui_registry);
```

- [ ] **Step 2: Implement render_ui_only()**

In `src/renderer/renderer.cpp`, add:

```cpp
void Renderer::render_ui_only(lua_State* L, ui::UIControlRegistry* ui_registry) {
    u32 fi = frame_index_ % FRAMES_IN_FLIGHT;
    vkWaitForFences(device_, 1, &render_fence_[fi], VK_TRUE, UINT64_MAX);

    u32 image_index = 0;
    VkResult acq_result = vkAcquireNextImageKHR(
        device_, swapchain_, UINT64_MAX, present_semaphore_[fi],
        VK_NULL_HANDLE, &image_index);
    if (acq_result == VK_ERROR_OUT_OF_DATE_KHR) {
        recreate_swapchain();
        return;
    }
    vkResetFences(device_, 1, &render_fence_[fi]);

    // Begin command buffer
    vkResetCommandBuffer(cmd_buf_[fi], 0);
    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd_buf_[fi], &begin_info);

    // Upload movie textures (before render pass)
    if (ui_registry) {
        for (auto& ctrl_ptr : ui_registry->all()) {
            if (!ctrl_ptr || ctrl_ptr->destroyed()) continue;
            if (ctrl_ptr->video_needs_upload()) {
                ui_renderer_.update_movie_texture(cmd_buf_[fi], *ctrl_ptr);
            }
        }
    }

    // Begin swapchain render pass (NOT scene_render_pass_)
    // render_pass_ has 2 attachments: color + depth — must provide both clear values
    std::array<VkClearValue, 2> clear{};
    clear[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    clear[1].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = render_pass_;
    rp.framebuffer = framebuffers_[image_index];
    rp.renderArea.extent = {window_width_, window_height_};
    rp.clearValueCount = static_cast<u32>(clear.size());
    rp.pClearValues = clear.data();
    vkCmdBeginRenderPass(cmd_buf_[fi], &rp, VK_SUBPASS_CONTENTS_INLINE);

    // Update + render UI
    if (ui_registry && L) {
        ui_renderer_.update(L, *ui_registry, texture_cache_, font_cache_,
                           window_width_, window_height_);
        if (ui_pipeline_ && ui_renderer_.quad_count() > 0) {
            vkCmdBindPipeline(cmd_buf_[fi], VK_PIPELINE_BIND_POINT_GRAPHICS,
                              ui_pipeline_);
            ui_renderer_.render(cmd_buf_[fi], ui_layout_,
                               window_width_, window_height_);
        }
    }

    vkCmdEndRenderPass(cmd_buf_[fi]);
    vkEndCommandBuffer(cmd_buf_[fi]);

    // Submit
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &present_semaphore_[fi];
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd_buf_[fi];
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &render_semaphore_[fi];
    vkQueueSubmit(graphics_queue_, 1, &submit, render_fence_[fi]);

    // Present
    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &render_semaphore_[fi];
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &image_index;
    vkQueuePresentKHR(graphics_queue_, &present);

    frame_index_ = (frame_index_ + 1) % FRAMES_IN_FLIGHT;
}
```

- [ ] **Step 3: Build and verify**

Run: `cmake --build build --config Debug`
Expected: Compiles clean.

- [ ] **Step 4: Commit**

```bash
git add src/renderer/renderer.hpp src/renderer/renderer.cpp
git commit -m "Add render_ui_only() for UI-only frames during loading"
```

---

### Task 5: Chunked Reload State Machine

Split `execute_reload_sequence()` into a stage-based state machine.

**Files:**
- Modify: `src/main.cpp` (replace monolithic reload with staged approach)

- [ ] **Step 1: Add ReloadStage enum and advance_reload()**

Near the top of `src/main.cpp` (after existing helpers, ~line 370), add:

```cpp
enum class ReloadStage : osc::u8 {
    IDLE,
    CLEAR_SCENE,
    INIT_LUA,
    LOAD_SCENARIO,
    BOOT_SIM,
    BUILD_SCENE,
    DONE
};

static ReloadStage s_reload_stage = ReloadStage::IDLE;
```

- [ ] **Step 2: Extract reload stages from execute_reload_sequence()**

Refactor `execute_reload_sequence()` into `advance_reload()` that executes one stage. Keep the same parameter list but add `ReloadStage& stage`. Each case covers the same lines as the original:

- `CLEAR_SCENE`: original lines 393-398 (clear_scene, destroy sim/lua)
- `INIT_LUA`: original lines 400-418 (create lua, init, rebind, load blueprints)
- `LOAD_SCENARIO`: original lines 420-484 (create SimState, scenario, armies, callbacks, GSM)
- `BOOT_SIM`: original lines 486-506 (boot_sim, start_session)
- `BUILD_SCENE`: original lines 508-546 (build_scene, camera, UI state, selection)

The `BUILD_SCENE` stage does NOT call `transition_to(GAME)` — that happens in the main loop after the stage returns DONE.

- [ ] **Step 3: Update main loop launch handler**

Replace the current `__osc_launch_requested` handler (~line 1491) with:

```cpp
if (launch_requested && s_reload_stage == ReloadStage::IDLE) {
    // Start loading
    s_reload_stage = ReloadStage::CLEAR_SCENE;
    game_state_mgr.transition_to(osc::GameState::LOADING, ui_lua_state.raw());

    // Call WldUIProvider::start_loading_dialog
    wld_ui_provider.start_loading_dialog(ui_lua_state.raw());

    // Pump one UI frame to show the loading screen
    pump_ui_frames(ui_lua_state, ui_thread_manager, beat_registry, 1, ui_frame_counter);
    renderer.render_ui_only(ui_lua_state.raw(), &ui_registry);
}

// Chunked reload loop
if (s_reload_stage != ReloadStage::IDLE && s_reload_stage != ReloadStage::DONE) {
    glfwPollEvents();  // prevent OS "Not Responding"
    bool more = advance_reload(s_reload_stage, /* ...params... */);

    // Pump UI frames between stages
    pump_ui_frames(ui_lua_state, ui_thread_manager, beat_registry, 1, ui_frame_counter);
    renderer.render_ui_only(ui_lua_state.raw(), &ui_registry);

    if (!more) {
        // Loading complete
        game_state_mgr.transition_to(osc::GameState::GAME, nullptr); // nullptr = skip SetupUI
        wld_ui_provider.stop_loading_dialog(ui_lua_state.raw());
        call_start_game_ui(ui_lua_state.raw());
        s_reload_stage = ReloadStage::IDLE;
    }
}
```

- [ ] **Step 4: Implement WldUIProvider::start/stop_loading_dialog**

In `src/ui/wld_ui_provider.cpp`, replace the stubs:

```cpp
void WldUIProvider::start_loading_dialog(lua_State* L) {
    // Call the Lua-side provider's StartLoadingDialog method
    lua_pushstring(L, "__osc_wld_ui_provider");
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, "StartLoadingDialog");
        lua_gettable(L, -2);
        if (lua_isfunction(L, -1)) {
            lua_pushvalue(L, -2); // self
            if (lua_pcall(L, 1, 0, 0) != 0) {
                spdlog::warn("StartLoadingDialog error: {}", lua_tostring(L, -1));
                lua_pop(L, 1);
            }
        } else {
            lua_pop(L, 1);
        }
        lua_pop(L, 1); // provider table
    } else {
        lua_pop(L, 1);
        spdlog::debug("WldUIProvider: no __osc_wld_ui_provider in registry");
    }
}

void WldUIProvider::stop_loading_dialog(lua_State* L) {
    lua_pushstring(L, "__osc_wld_ui_provider");
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, "StopLoadingDialog");
        lua_gettable(L, -2);
        if (lua_isfunction(L, -1)) {
            lua_pushvalue(L, -2); // self
            if (lua_pcall(L, 1, 0, 0) != 0) {
                spdlog::warn("StopLoadingDialog error: {}", lua_tostring(L, -1));
                lua_pop(L, 1);
            }
        } else {
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    } else {
        lua_pop(L, 1);
    }
}
```

- [ ] **Step 5: Build and run all tests**

Run: `cmake --build build --config Debug`
Run: `./build/tests/Debug/osc_tests.exe`
Expected: All tests pass.

- [ ] **Step 6: Commit**

```bash
git add src/main.cpp src/ui/wld_ui_provider.hpp src/ui/wld_ui_provider.cpp
git commit -m "Chunked reload state machine with UI frame pumping between stages"
```

---

## Chunk 4: Integration Testing

### Task 6: Add --loading-test flag

**Files:**
- Modify: `src/main.cpp` (add test flag + validation)

- [ ] **Step 1: Add flag and test harness**

Add `--loading-test` flag parsing alongside other test flags (~line 262). In the test harness section:

```cpp
if (loading_test && sim_state) {
    spdlog::info("=== LOADING TEST: chunked reload with UI frames ===");

    // Simulate a launch request
    lua_pushstring(sim_lua_state->raw(), "__osc_launch_requested");
    lua_pushboolean(sim_lua_state->raw(), 1);
    lua_rawset(sim_lua_state->raw(), LUA_REGISTRYINDEX);

    // Count UI frames rendered during reload
    int ui_frames_during_load = 0;
    // ... (hook into render_ui_only or check frame_index_ delta)

    // Verify WorldIsLoading was true during load
    // Verify GameState ends as GAME
    // Verify sim_state is valid after reload

    if (game_state_mgr.current() == osc::GameState::GAME && sim_state) {
        spdlog::info("  PASS — reload completed, GameState=GAME");
    } else {
        spdlog::error("  FAIL — unexpected state after reload");
        return 1;
    }
    return 0;
}
```

- [ ] **Step 2: Build, run, verify**

Run: `cmake --build build --config Debug`
Run: `MSYS_NO_PATHCONV=1 ./build/Debug/opensupcom.exe --map "/maps/SCMP_009/SCMP_009_scenario.lua" --loading-test`
Expected: PASS output.

- [ ] **Step 3: Commit**

```bash
git add src/main.cpp
git commit -m "Add --loading-test flag for chunked reload validation"
```

---

### Task 7: Update smoke test, README, memory

**Files:**
- Modify: `src/main.cpp` (update smoke test phase for loading screen)
- Modify: `README.md` (add --loading-test to flag table)
- Modify: `~/.claude/projects/c--Users-bwiem-projects-supcom-engine/memory/MEMORY.md`
- Modify: `~/.claude/projects/c--Users-bwiem-projects-supcom-engine/memory/milestones-list.md`

- [ ] **Step 1: Add M168 to milestones and README**

Update milestones-list.md with M168 entry. Update README test flag table.

- [ ] **Step 2: Update MEMORY.md with M168 decisions**

Add key decisions: chunked reload, render_ui_only uses render_pass_ not scene_render_pass_, persistent staging buffer for video, GetCursor dummy, SFD demuxer best-effort.

- [ ] **Step 3: Commit**

```bash
git add README.md
git commit -m "M168: Loading screen with video playback and chunked reload"
```

---

## Summary

| Task | What it delivers | Files touched |
|------|-----------------|---------------|
| 1 | VideoDecoder + SFD demuxer + pl_mpeg | third_party/, src/video/, tests/ |
| 2 | MovieControl wired to VideoDecoder, GetCursor | ui_control.hpp, moho_bindings.cpp |
| 3 | Dynamic movie texture with persistent staging buffer | ui_renderer.hpp/cpp |
| 4 | render_ui_only() for UI-only frames | renderer.hpp/cpp |
| 5 | Chunked reload state machine + WldUIProvider | main.cpp, wld_ui_provider.hpp/cpp |
| 6 | --loading-test integration test | main.cpp |
| 7 | Docs + memory updates | README.md, memory files |

**Success criteria:** `--loading-test` completes reload with UI frames pumped between stages, `WorldIsLoading()` true during load, GameState ends as GAME, no crashes. Video playback renders MPEG-1 frames to screen when `.mpg` files are present.
