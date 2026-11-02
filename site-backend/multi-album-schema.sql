-- CONTROL_DB only; the shared photo bytes remain in the existing ALBUM_DB.
CREATE TABLE IF NOT EXISTS md_album_state (
 device_id TEXT PRIMARY KEY REFERENCES md_devices(id),
 revision INTEGER NOT NULL DEFAULT 0,
 playback TEXT NOT NULL DEFAULT 'sequence' CHECK(playback IN ('sequence','random','fixed')),
 interval_seconds INTEGER NOT NULL DEFAULT 300 CHECK(interval_seconds IN (60,300,600)),
 selected_id TEXT NOT NULL DEFAULT '',
 item_count INTEGER NOT NULL DEFAULT 0 CHECK(item_count BETWEEN 0 AND 1000),
 asset_revision INTEGER NOT NULL DEFAULT -1
) STRICT;
CREATE TABLE IF NOT EXISTS md_album_items (
 device_id TEXT NOT NULL REFERENCES md_devices(id),
 photo_id TEXT NOT NULL CHECK(length(photo_id)=64),
 position INTEGER NOT NULL,
 PRIMARY KEY(device_id,photo_id),
 UNIQUE(device_id,position)
) STRICT;
