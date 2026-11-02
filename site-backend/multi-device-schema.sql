-- V6.0 additive schema. No legacy table is altered or populated here.
CREATE TABLE IF NOT EXISTS md_devices (
  id TEXT PRIMARY KEY,
  space_id TEXT NOT NULL,
  name TEXT NOT NULL CHECK(length(name) BETWEEN 1 AND 24),
  profile_id TEXT NOT NULL,
  token_hash TEXT NOT NULL UNIQUE CHECK(length(token_hash)=64),
  enabled INTEGER NOT NULL DEFAULT 1 CHECK(enabled IN (0,1)),
  metadata_revision INTEGER NOT NULL DEFAULT 1,
  revision INTEGER NOT NULL DEFAULT 0 CHECK(revision BETWEEN 0 AND 4294967295),
  desired_json TEXT NOT NULL CHECK(json_valid(desired_json)),
  command_id TEXT NOT NULL DEFAULT '',
  applied_revision INTEGER NOT NULL DEFAULT 0,
  report_json TEXT NOT NULL DEFAULT '{}',
  report_hash TEXT NOT NULL DEFAULT '',
  session_id TEXT NOT NULL DEFAULT '',
  confirmed_session TEXT NOT NULL DEFAULT '',
  boot_id TEXT NOT NULL DEFAULT '',
  last_sequence INTEGER NOT NULL DEFAULT 0,
  session_generation INTEGER NOT NULL DEFAULT 0,
  last_seen INTEGER NOT NULL DEFAULT 0,
  next_wake_at INTEGER NOT NULL DEFAULT 0,
  created_at INTEGER NOT NULL,
  requested_at INTEGER NOT NULL DEFAULT 0
) STRICT;
CREATE INDEX IF NOT EXISTS md_space ON md_devices(space_id,id);
-- Screen mirror is a latest-frame snapshot, never an image history.
CREATE TABLE IF NOT EXISTS md_screens (
  device_id TEXT PRIMARY KEY REFERENCES md_devices(id) ON DELETE CASCADE,
  frame BLOB NOT NULL CHECK(length(frame)=15000),
  hash TEXT NOT NULL CHECK(length(hash)=16),
  mode TEXT NOT NULL,
  updated_at INTEGER NOT NULL
) STRICT;
CREATE TABLE IF NOT EXISTS md_commands (
  device_id TEXT NOT NULL REFERENCES md_devices(id) ON DELETE CASCADE,
  revision INTEGER NOT NULL,
  desired_json TEXT NOT NULL CHECK(json_valid(desired_json)),
  created_at INTEGER NOT NULL,
  PRIMARY KEY(device_id,revision)
) STRICT;

-- Operator-selected singleton; never infer identity from nickname or report body.
CREATE TABLE IF NOT EXISTS md_legacy_bridge (
 id INTEGER PRIMARY KEY CHECK(id=1),
 device_id TEXT NOT NULL UNIQUE REFERENCES md_devices(id),
 created_at INTEGER NOT NULL
) STRICT;
