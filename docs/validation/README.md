# Validation records

Each file here records one validation of an exact commit of `main`: what ran, on what, and what came out. A claim about the engine's state should point at one of these rather than at a PR description.

Name a record `YYYY-MM-DD-<short sha>.md`. A record covers:

- **CI on the exact head:** the run, each job's result.
- **Local retail gates:** `ctest -L "gate|mp|golden|arch"` against retail data (`OSC_FA_PATH` pinned), and the unit tests. Count, failures, and why each failure is or isn't a defect.
- **Goldens:** if any were re-baselined, every differing region and the change that explains it.
- **Long games:** four-AI games of 18,000 ticks on retail and on FAF data (seeds named), Release build. Script errors, and what each one is.
- **Flows:** campaign, tutorial, outro, mods, save/load (skirmish, campaign, replay), replay, determinism, lobby and network. They are gate tests, called out by name.
- **FAF client:** the custom-game harness (a scripted GPGNet client hosting a game to a tick).
- **Not covered:** what needs a person, such as a real display, audio, Steam or the Deck.

How to run each step is in the record itself.
