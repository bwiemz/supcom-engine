#pragma once

// The integration runner's test modes (tests/integration/runner): the
// engine's run (src/app) with them is osc_integration.

#include "app/app.hpp"
#include "integration_tests.hpp"

#include <optional>
#include <set>
#include <string>

namespace osc::test {

class IntegrationModes : public app::TestModes {
public:
    void print_usage() const override;
    app::TestRequest parse(int argc, char* argv[]) override;
    std::optional<int> before_boot(int argc, char* argv[]) override;
    std::optional<int> before_init(const lua::InitConfig& config) override;
    std::optional<int> front_end(app::Engine& e) override;
    void frame_view(app::Engine& e, app::Frame& frame) override;
    void frame_rendered(app::Engine& e, app::Frame& frame) override;
    bool frames_done() const override;
    std::optional<int> after_window() override;
    std::optional<int> headless_first(app::Engine& e) override;
    void headless(app::Engine& e) override;

private:
    /// Whether the command line gave this mode's flag.
    bool has(const char* flag) const;

    std::set<std::string, std::less<>> given_; // the headless modes asked for
    bool interp_ = false;                      // --interp-test
    std::string render_dump_path_;             // --render-dump <file>
    InterpProbe interp_probe_;
    std::optional<RenderDumpProbe> render_dump_;
};

/// The two-process LAN harnesses (lan_modes.cpp): --mp-host/--mp-join run a
/// lockstep match, --lan-host/--lan-join the lobby handshake and a match.
int run_mp_lan_test(bool is_host, const std::string& address, u16 port, u32 frames,
                    bool inject_desync);
int run_lan_lobby_test(bool is_host, const std::string& address, u16 port, u32 frames, u32 drop_at);

} // namespace osc::test
