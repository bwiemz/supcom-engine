#pragma once

namespace osc::renderer {

/// A busy draw may read cached render data, but scripts can call into the
/// live simulation. Defer script-dependent cache misses until a settled draw.
class RenderScriptScope {
public:
    explicit RenderScriptScope(bool allowed) : previous_(allowed_) { allowed_ = allowed; }
    ~RenderScriptScope() { allowed_ = previous_; }
    RenderScriptScope(const RenderScriptScope&) = delete;
    RenderScriptScope& operator=(const RenderScriptScope&) = delete;
    static bool allowed() { return allowed_; }

private:
    bool previous_;
    inline static thread_local bool allowed_ = true;
};

} // namespace osc::renderer
