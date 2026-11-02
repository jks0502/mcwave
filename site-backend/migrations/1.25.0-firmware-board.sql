-- Apply once, BEFORE deploying site1.25.0 or using publish-firmware.ps1.
-- Additive only; no image bytes, requests, devices or targets are deleted.
ALTER TABLE md_firmware ADD COLUMN board TEXT NOT NULL DEFAULT '' CHECK(board IN ('','mcwave','edison'));
UPDATE md_firmware SET board=CASE device_id
 WHEN '00000000-0000-0000-0000-00000000mcwave' THEN 'mcwave'
 WHEN '00000000-0000-0000-0000-000000edison' THEN 'edison'
 ELSE COALESCE((SELECT CASE
   WHEN profile_id='mcwave-hink-v1' THEN 'mcwave'
   WHEN profile_id IN ('edison-hink-s3-v1','edison-hink-s3-album-v1') THEN 'edison'
   ELSE '' END FROM md_devices WHERE id=md_firmware.device_id),'') END;
