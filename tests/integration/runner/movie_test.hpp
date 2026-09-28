#pragma once

namespace osc::app {
struct Engine;
}

namespace osc::test {

/// --movie-test (M216a), from the front end's start with the splash asked
/// for: FA's splash screens play their movies and sounds, and leave on
/// Escape for the main menu; movies play on their clock (Moho's CMauiMovie
/// and CMovie), their sounds wait to start, and a playing movie is drawn.
/// Failures are recorded in test_status.
void run_movie_test(app::Engine& e);

} // namespace osc::test
