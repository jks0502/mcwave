-- Additive controller authentication only (cooldown uses existing device requested_at).
-- Apply locally first. No formal controller credential is included or generated.
CREATE TABLE IF NOT EXISTS md_controllers (
  id TEXT PRIMARY KEY,
  token_hash TEXT NOT NULL UNIQUE CHECK(length(token_hash)=64),
  allowed_device_ids_json TEXT NOT NULL CHECK(json_valid(allowed_device_ids_json)),
  created_at INTEGER NOT NULL,
  enabled INTEGER NOT NULL DEFAULT 1 CHECK(enabled IN (0,1))
) STRICT;
