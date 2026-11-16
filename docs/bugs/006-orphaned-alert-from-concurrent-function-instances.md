# 006 — Two Function instances race on a device's state; an alert is left open forever

Found: 2026-11-17, ColdTrace Block 7 (the new dashboard showed two OPEN alerts for one device)
Status: fixed 2026-11-17 (optimistic concurrency in `azure/function_app/pipeline.py` and `table_store.py`)
Severity: medium · Priority: high (an alert that can never clear; in a real cold chain, a false alarm
nobody can silence, or a real one that looks stale)

## Symptom
The dashboard listed two `OPEN` excursion alerts for `coldtrace-01`, from different boots
(`b0f27ff9` and `c8aca622`), raised 3 s apart. A device can have at most one open alert: the second is
only raised if the device's state shows none open. The device's state row pointed at the second alert,
so the first could never be cleared.

## Investigation
- Both alerts were raised at 19:13:51 and 19:13:54 UTC, the minute the Function first started and caught
  up on the hub's backlog.
- In that same catch-up nearly every stored reading had `duplicates = 1`: the backlog was processed twice.
  The Event Hubs trigger is at-least-once, and while partition ownership settles, two instances can
  process the same partition.
- Processed in order, the second boot would have seen the open alert. Only a **lost update** explains
  two: instance B read the device state before instance A wrote its open alert, raised its own, and then
  overwrote A's state (`upsert ... REPLACE`, last writer wins).
- Reproduced in a unit test with the original pipeline and a last-writer-wins store, interleaving a second
  instance between the first one's read and write: **2 OPEN alerts, state pointing at one**.

## Root cause
The per-device state (open alert, excursion start, last seq) was read, changed and written back with no
check that it was still the version read. Alert rows were also written before the state, so a losing
writer left its alert behind.

## Fix
1. **Optimistic concurrency:** `put_state` writes with `If-Match: <etag>` (create-only when the row did
   not exist). A changed row raises `StateConflict`, and that device's step is repeated on fresh state
   (up to 5 attempts).
2. **Side effects after the commit:** alert raises/clears are collected while the state advances and
   written only after the state write succeeded.
3. **Replays decided from state, not from the insert:** a reading moves history forward only if it is
   newer than the state (same boot: higher seq; other boot: later hub enqueue time). Readings are inserted
   once, before the retry loop, so retries do not inflate the duplicate counter.
4. **Deterministic alert ids** (`device-boot-excursion start`): two instances judging the same excursion
   name it alike.

## Verification
- Unit test `test_two_instances_racing_raise_exactly_one_alert`: same interleaving, now 1 alert, one
  refused commit, and the state points at the alert. Fails on the original code.
- Against real Table Storage: a second create and a stale `If-Match` write both raise `StateConflict`;
  the first writer's value survives.
- The orphaned row is kept, marked `ORPHANED` with a note, as evidence.

## Lesson
At-least-once delivery means "the same event twice" and also "two workers at once". De-duplicating the
data was not enough; every read-modify-write of shared state needs a concurrency check.
