# 001 — Serial output drifts right ("staircase") in the Wokwi VS Code terminal

Found: 2026-11-15, Block 6 manual run · Status: fixed 2026-11-15 (output now ends lines with `\r\n`; verified in Wokwi 2026-11-15)

## Observed
Each response line starts where the previous one ended, so the terminal output drifts further right on every line.

## Expected
Each line starts at column 0.

## Cause
The firmware ends lines with `\n` only. The VS Code terminal moves down on `\n` but does not return to
column 0. `"serialMonitor": {"convertEol": true}` in `diagram.json` did not help, so the VS Code extension
appears to ignore that setting.

## Impact
Cosmetic for people; a harness that splits on `\n` is unaffected. It does block a clean README screenshot.

## Proposed fix
End every output line with `\r\n` (the usual serial-protocol convention). Input already accepts `\r`, `\n` and
`\r\n`. Unit-test expectations change from `\n` to `\r\n`; the harness strips `\r`.
