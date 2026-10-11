# UI Focus Debugger

Runtime UMG/Slate focus debugger for Unreal Engine 5 — an in-viewport overlay for humans, plus a crash-safe JSONL session recorder that AI agents (or humans) can analyze after the fact.

运行时 UMG/Slate 焦点调试器：给人看的画面内叠加层 + 给 agent 读的崩溃安全会话日志。

Tested with **UE 5.8** (editor & PIE, Development config).

## Features

- **In-viewport overlay** (zero cost when off): focus box (cyan dashed) / hover terminal (blue) / mismatch indicator, info bar with owner-widget resolution, CommonUI activation stack & input method, recent-event tail, and a full **navigation decision replay** panel (direction, winning rule + which widget holds it, selection method, destination, per-widget rule chain).
- **Session recorder (flight recorder)**: every focus/navigation/activation event is appended to a JSONL file with immediate flush — the file survives an editor crash (a missing footer line marks a crashed session). Events carry owner names, navigation decisions, per-widget context (focusable/visibility/enabled/geometry) and symbolized call stacks with project-frame filtering.
- **Agent-ready**: self-describing file header (schema + field docs), a [playbook](Docs/FocusPlaybook.md) with anomaly signatures and a step-by-step analysis workflow. Built for the workflow: *QA reproduces → log file → agent diagnoses → fix proposal*.
- **Owner resolution**: focus landing on a leaf widget reports the owning list entry / activatable page (list-row boundary first, then CommonUI Activatable page boundary).

## Install

**Option A — project plugin**: copy this folder into `<YourProject>/Plugins/UiFocusDebugger/`, enable the plugin in the editor.

**Option B — shared plugin directory** (multiple projects): add to `<YourProject>.uproject`:

```json
"AdditionalPluginDirectories": [ "D:/Path/To/SharedPlugins" ],
"Plugins": [ { "Name": "UiFocusDebugger", "Enabled": true } ]
```

CommonUI is optional — the CommonUI bridge (activation stack, input method, activatable boundary detection) loads automatically when CommonUI is present. **Zero compile-time plugin dependencies**: the bridge accesses CommonUI types via `/Script` reflection only, so the plugin compiles in any environment (marketplace compliance) without affecting projects that don't use CommonUI.

## Quick start

1. Enter PIE, click the **"UI Focus"** toggle in the editor toolbar (next to Play) — or run `uifocus.overlay 1` in the console.
2. Reproduce the focus problem. The overlay shows what happens live; the session recorder writes everything to `<Project>/Saved/UiFocusDebugger/sessions/<date>.jsonl` (starting/stopping follows the overlay toggle).
3. Stop the session (toggle off). Hand the JSONL to a human or an AI agent with [Docs/FocusPlaybook.md](Docs/FocusPlaybook.md).

### Console reference

| Command | Meaning |
|---|---|
| `uifocus.overlay 0/1` | Overlay + default session recording (follow mode) |
| `uifocus.record -1/0/1` | Session recording: -1 follow overlay (default), 0 force off, 1 force on (activation events polled via built-in ticker) |
| `uifocus.mark "note"` | Insert a marker line into the active session (tag the repro moment) |
| `uifocus.dump [N]` | Export the last N ring-buffer events to a JSONL file — post-mortem when you forgot to record or after a crash |
| `uifocus.stackcapture 0/1` | Capture call stacks on focus events (sessions auto-enable and restore this) |

## Session log format (short version)

JSONL: first line `header` (metadata + field docs, self-describing), then `event` lines, optionally a final `footer` (missing footer = crashed session). `mark` lines mark repro moments. See the playbook for the full field reference, five anomaly signatures (swallowed navigation, focus-clear coupled with page activation, focus oscillation, silent stream = input never reached Slate, inferred-direction caveats) and the recommended analysis workflow.

## Known limitations

- Focus tracking observes Slate user 0 only.
- `uifocus.dump` exports ring events without per-widget context (`ctx`) — the ring doesn't store it; stacks are preserved.
- Navigation decision replay requires `WITH_SLATE_DEBUGGING` (any Development/Editor build; not Shipping/Test).
- The `InferredExplicit` method marks decisions reconstructed by geometry (the engine does not broadcast the explicit-reply direct path).

## License

[MIT](LICENSE) — free to use in commercial and personal projects.
