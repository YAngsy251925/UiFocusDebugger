# Releases

## 1.0.0 — 2026-10-11

First public release. Tested on UE 5.8 (EX & Lyra projects, editor + PIE, Development).

### Marketplace compliance / zero plugin dependencies
- No compile-time dependency on any plugin: the CommonUI bridge module accesses `CommonActivatableWidget`/`CommonInputSubsystem` via `/Script` reflection and `UFUNCTION` calls (`IsActivated`, `GetCurrentInputType`), compiling cleanly in environments without CommonUI. Runtime still auto-activates the bridge when CommonUI is loaded.

### In-viewport overlay (人类可读的实时视图)
- Focus box (cyan dashed) / hover terminal (blue solid) / mismatch red dot; owner-widget labels.
- Info bar: focus, owner resolution, hover, CommonUI activation stack + input method.
- Right-top event tail (last 20 events, relative seconds).
- Left-bottom **navigation decision replay**: direction, winning rule and its holder widget, selection method, destination (leaf + owner), rule chain of every configured widget (inner→outer, winner marked).
- All panels width-adaptive with measured text; zh/en UI follows editor language.

### Session recorder — flight recorder (供 agent/人类事后分析)
- Every focus/navigation/activation event appended to `<Project>/Saved/UiFocusDebugger/sessions/<datetime>.jsonl`, flushed per line: **the file survives an editor crash**; a missing `footer` line marks a crashed session. `latest.txt` points to the newest session.
- Self-describing `header`: project/engine/config/map/viewport/DPI, cvar snapshot, start wall clock (aligns with the project log), schema version, embedded field documentation.
- Events carry: cause, old/new names + owner + path, full navigation decision, per-widget context (`focusable`/`vis`/`enabled`/`rect` for old/new/destination), symbolized stacks with `pstack` project-frame filtering (runtime-derived project directory tokens).
- Raw stream (pre-filter): chained hops and self-loops preserved for analysis; the overlay keeps its folded view.
- `uifocus.record` cvar: -1 follow overlay (default) / 0 force off / 1 force on (activation events via built-in 0.1s poll when overlay off).
- Auto stack capture during sessions, restored on stop (rolled back if the file can't be created).
- `uifocus.mark "note"`: marker lines to pin the repro moment.
- `uifocus.dump [N]`: export the last N ring events post-mortem (forgot to record / after a crash).
- **FocusPlaybook.md**: agent analysis manual — six-step workflow, five anomaly signatures, known limitations.

### 0.1 (2026-10-06 → 2026-10-10, internal iterations)

- v0.1 initial plugin: focus event ring (4096), cause tracking, hover chain, HTTP web panel (since removed for performance), UMG name cache.
- Editor toolbar toggle; CommonUI bridge (activation stack polling, activatable boundary predicate, input method).
- In-viewport overlay replaces web panel; owner resolution (list-row boundary / activatable page boundary); event tail.
- Navigation decision replay via `FSlateDebugging::AttemptNavigationEvent` broadcast + geometric inference for the non-broadcast explicit path; pending-nav flush (navigation that moved nothing).
- Live-Coding guardrails for struct layout changes (ring + overlay state versioned, leak-don't-touch policy).
