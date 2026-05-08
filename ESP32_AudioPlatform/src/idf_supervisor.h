// Geelen orchestrator (supervisor). See docs/control_plane.md for the
// design — every cross-cutting state machine the firmware owns lives
// here in Phase 1+.
//
// Phase 0: scaffold only. `supervisorBegin` brings up the GEELEN_EVENTS
// loop and registers an ANY_ID logging handler so we can verify event
// posting end-to-end on a flashed device. No subsystems are migrated to
// the bus yet — existing voice / mode / wifi paths still work as-is.

#ifndef GEELEN_IDF_SUPERVISOR_H
#define GEELEN_IDF_SUPERVISOR_H

#ifdef ESP_PLATFORM

namespace geelen_idf {

// Idempotent. Creates the supervisor event loop (via `eventLoop()`) and
// registers Phase 0 handlers. Call exactly once from `app_main` after
// the logs subsystem is up so handler ESP_LOGIs land in the live ring.
//
// Returns false if event loop / handler registration failed (logged at
// ERROR). The firmware should continue booting either way — current
// orchestration paths don't yet depend on the bus.
bool supervisorBegin();

}  // namespace geelen_idf

#endif  // ESP_PLATFORM

#endif  // GEELEN_IDF_SUPERVISOR_H
