# SWMM6_2

## Active Memory
@.memory/current.md
@.memory/specifications.md

## Reference Memory
Read these when relevant to the current task:
- `.memory/history_decisions.md` — past architectural choices and their outcomes
- `.memory/history_successes.md` — patterns that worked, with transferable insights
- `.memory/history_failures.md` — pitfalls and lessons
- `.memory/pruned.md` — archived directions (scan before re-exploring old ground)

## Memory Maintenance
- Update `.memory/current.md` when state changes or next steps complete.
- Log significant choices to `.memory/history_decisions.md`; update `outcome:` when the result becomes clear.
- After a task completes, record the outcome in successes or failures — write `pattern:` and `lesson:` to transfer across projects.
- Run `python3 ~/Projects/Memory_System/global/ingest.py` to sync the global graph.
