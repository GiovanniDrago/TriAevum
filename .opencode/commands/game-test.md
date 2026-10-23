---
description: Run one deterministic TriAevum gameplay verification scenario
agent: game-tester
subtask: true
---
Run the scenario described below using the repository gameplay harness.

Scenario or arguments:
$ARGUMENTS

Required behavior:

1. Load `game-test-session` with the `skill` tool.
2. Use `python3 tools/game_test/run_case.py` with the scenario or equivalent
   environment variables.
3. Analyze the artifacts with `python3 tools/game_test/analyze_evidence.py`.
4. Read the final screenshot and relevant runtime/probe output.
5. Return the exact `game-report` format, including PASS, FAIL, or BLOCKED.

When a save seed is loaded through the title/file-select flow, select the
existing save before pressing A. Pressing A on the default empty slot opens the
name-entry screen and never reaches gameplay; that is a setup failure, not a
gameplay result. The shared `title_to_existing_save.json` timeline includes the
first-save selection step.

Do not edit source files, tasks.md, or Git state. Do not commit or push.
