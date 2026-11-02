CREATE TABLE IF NOT EXISTS epaper_control (
  id INTEGER PRIMARY KEY CHECK (id = 1),
  revision INTEGER NOT NULL DEFAULT 0,
  mode TEXT NOT NULL DEFAULT 'weather',
  page INTEGER NOT NULL DEFAULT 0,
  requested_at INTEGER NOT NULL DEFAULT 0,
  applied_revision INTEGER NOT NULL DEFAULT 0,
  reported_mode TEXT NOT NULL DEFAULT 'weather',
  reported_page INTEGER NOT NULL DEFAULT 0,
  page_count INTEGER NOT NULL DEFAULT 1,
  firmware TEXT NOT NULL DEFAULT '',
  last_seen INTEGER NOT NULL DEFAULT 0
);
INSERT OR IGNORE INTO epaper_control (id) VALUES (1);
