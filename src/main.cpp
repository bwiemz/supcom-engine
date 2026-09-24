// opensupcom: the game. The engine's run is src/app; its test modes are in
// the integration runner (tests/integration/runner).

#include "app/app.hpp"

int main(int argc, char* argv[]) {
    return osc::app::run(argc, argv, nullptr);
}
