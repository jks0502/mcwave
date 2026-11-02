-- V6 OTA, additive (CONTROL_DB). Image bytes live in the FIRMWARE KV namespace
-- under fw:<sha256>; a release belongs to exactly one device because each image
-- embeds that device's credential.
CREATE TABLE IF NOT EXISTS md_firmware (
 sha256 TEXT PRIMARY KEY CHECK(length(sha256)=64),
 device_id TEXT NOT NULL REFERENCES md_devices(id),
 board TEXT NOT NULL DEFAULT '' CHECK(board IN ('','mcwave','edison')),
 version TEXT NOT NULL CHECK(length(version) BETWEEN 2 AND 32),
 size INTEGER NOT NULL CHECK(size BETWEEN 1 AND 3145728),
 notes TEXT NOT NULL DEFAULT '' CHECK(length(notes)<=200),
 created_at INTEGER NOT NULL
) STRICT;
CREATE INDEX IF NOT EXISTS md_firmware_device ON md_firmware(device_id,created_at);
-- The requested image for a device. attempt increases on every install/retry
-- so a device never re-downloads an attempt that already failed.
CREATE TABLE IF NOT EXISTS md_device_ota (
 device_id TEXT PRIMARY KEY REFERENCES md_devices(id),
 target_sha TEXT NOT NULL DEFAULT '',
 attempt INTEGER NOT NULL DEFAULT 0,
 requested_at INTEGER NOT NULL DEFAULT 0
) STRICT;
