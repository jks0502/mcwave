-- Additive migration; existing revisions and page selection remain intact.
ALTER TABLE epaper_control ADD COLUMN photo_id TEXT NOT NULL DEFAULT '';
ALTER TABLE epaper_control ADD COLUMN paused INTEGER NOT NULL DEFAULT 0;
ALTER TABLE epaper_control ADD COLUMN random_order INTEGER NOT NULL DEFAULT 0;
ALTER TABLE epaper_control ADD COLUMN interval_seconds INTEGER NOT NULL DEFAULT 60;
ALTER TABLE epaper_control ADD COLUMN reported_photo TEXT NOT NULL DEFAULT '';
ALTER TABLE epaper_control ADD COLUMN cache_count INTEGER NOT NULL DEFAULT 0;
