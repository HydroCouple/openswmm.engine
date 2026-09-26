# Worked-example decks

Small models the simulated-tier figures and the Application Manual run.
This directory is the Doxyfile's `EXAMPLE_PATH`, so chapters quote a deck's
lines verbatim with `\snippet <deck> <marker>` between `;//! [marker]` pairs
instead of copying them into prose.

Each deck lives in its own subdirectory with a `provenance.md` naming where it
came from (a study, an example, a benchmark case, or authored for the manual),
the commit or date, and what was changed. Keep decks runnable in minutes on
CPU; anything longer belongs to the `external` figure tier.
