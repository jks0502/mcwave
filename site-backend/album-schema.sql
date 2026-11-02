CREATE TABLE IF NOT EXISTS album_photos (
  id TEXT PRIMARY KEY CHECK(length(id)=64),
  name TEXT NOT NULL CHECK(length(name) BETWEEN 1 AND 80),
  bitmap BLOB NOT NULL CHECK(length(bitmap)=15000),
  sha256 TEXT NOT NULL CHECK(sha256=id),
  settings TEXT NOT NULL,
  format_version INTEGER NOT NULL DEFAULT 1 CHECK(format_version=1),
  sort_order INTEGER NOT NULL,
  enabled INTEGER NOT NULL DEFAULT 1 CHECK(enabled IN (0,1)),
  created_at INTEGER NOT NULL,
  deleted_at INTEGER,
  owner TEXT CHECK(owner IS NULL OR length(owner)=36)
) STRICT;
CREATE INDEX IF NOT EXISTS album_order ON album_photos(deleted_at,sort_order,id);
CREATE INDEX IF NOT EXISTS album_playlist ON album_photos(deleted_at,enabled,sort_order,id);
CREATE TABLE IF NOT EXISTS album_state (
  id INTEGER PRIMARY KEY CHECK(id=1),
  revision INTEGER NOT NULL DEFAULT 0,
  stored_count INTEGER NOT NULL DEFAULT 0,
  active_count INTEGER NOT NULL DEFAULT 0,
  current_id TEXT
) STRICT;
INSERT OR IGNORE INTO album_state(id) VALUES(1);
CREATE TRIGGER IF NOT EXISTS album_insert AFTER INSERT ON album_photos BEGIN
  UPDATE album_state SET stored_count=stored_count+1,active_count=active_count+(NEW.deleted_at IS NULL) WHERE id=1;
END;
CREATE TRIGGER IF NOT EXISTS album_delete AFTER DELETE ON album_photos BEGIN
  UPDATE album_state SET stored_count=stored_count-1,active_count=active_count-(OLD.deleted_at IS NULL),current_id=CASE WHEN current_id=OLD.id THEN NULL ELSE current_id END WHERE id=1;
END;
CREATE TRIGGER IF NOT EXISTS album_visibility AFTER UPDATE OF deleted_at,enabled ON album_photos BEGIN
  UPDATE album_state SET active_count=active_count+(NEW.deleted_at IS NULL)-(OLD.deleted_at IS NULL),
    current_id=CASE WHEN current_id=NEW.id AND (NEW.deleted_at IS NOT NULL OR NEW.enabled=0) THEN NULL ELSE current_id END WHERE id=1;
END;
