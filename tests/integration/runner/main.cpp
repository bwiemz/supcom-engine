// osc_integration: the engine (src/app) with the integration test modes.
// CTest runs it for the data-backed and multiplayer tests; see
// tests/integration/CMakeLists.txt.

#include "test_modes.hpp"

int main(int argc, char* argv[]) {
    osc::test::IntegrationModes modes;
    return osc::app::run(argc, argv, &modes);
}
