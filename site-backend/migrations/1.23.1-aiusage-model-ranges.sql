CREATE TABLE IF NOT EXISTS cyd_usage_model_ranges (
  id INTEGER PRIMARY KEY CHECK (id = 1),
  generated_at INTEGER NOT NULL,
  uploaded_at INTEGER NOT NULL,
  payload TEXT NOT NULL CHECK (length(payload) <= 262144)
);
