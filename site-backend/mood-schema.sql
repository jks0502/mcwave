-- mcwave 6.0.0 / website 1.18.0: two-person mood. Side 0 = jks, 1 = momosaysay.
CREATE TABLE IF NOT EXISTS md_mood (
  side INTEGER PRIMARY KEY CHECK (side IN (0, 1)),
  mood TEXT NOT NULL,
  note TEXT NOT NULL DEFAULT '',
  bitmap BLOB NOT NULL CHECK (length(bitmap) = 7500),
  version INTEGER NOT NULL,
  updated_at INTEGER NOT NULL
);
