-- Review before applying to CONTROL_DB. Not executed remotely by this task.
CREATE TABLE IF NOT EXISTS md_screens (
  device_id TEXT PRIMARY KEY REFERENCES md_devices(id) ON DELETE CASCADE,
  frame BLOB NOT NULL CHECK(length(frame)=15000),
  hash TEXT NOT NULL CHECK(length(hash)=16),
  mode TEXT NOT NULL,
  updated_at INTEGER NOT NULL
) STRICT;
