---
name: game-report
description: Produce a deterministic gameplay verification report with PASS, FAIL, or BLOCKED status and exact evidence paths.
license: GPL-3.0-only
compatibility: opencode
metadata:
  audience: game-tester
  workflow: runtime-verification
---
Return exactly this structure:

```text
STATUS: PASS | FAIL | BLOCKED
SCENARIO: <name and scenario path>
COMMANDS: <harness and analysis commands>
ARTIFACTS:
- result: <path>
- log: <path>
- screenshot: <path>
- runtime: <path>
- probe: <path>
EXPECTED: <scenario expectations>
OBSERVED: <visual and runtime observations>
MEMORY: <probe/runtime fields with frames>
SCREENSHOTS: <what was visible and why it supports the verdict>
ERRORS: <none, or exact errors>
NEXT ACTION: <none, retry, or required user input>
```

Rules:

- `PASS` requires a completed run, a readable screenshot, and the expected
  runtime/probe evidence.
- `FAIL` means the scenario ran but the expected result was not observed.
- `BLOCKED` means the environment could not run the scenario; list the missing
  package, display, state, tool, or permission.
- Never omit artifact paths.
- Never claim a subagent, screenshot, or memory check that did not happen.
