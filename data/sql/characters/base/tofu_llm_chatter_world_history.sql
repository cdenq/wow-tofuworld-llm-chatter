-- Tofu Worldview addition: recent lines of the realm-wide custom World
-- channel, written by LLMChatterTofuWorld.cpp (real-player lines when typed,
-- bot lines when delivered) and read by tools/chatter_tofu_world.py as
-- conversation context. The server keeps the newest HistoryLimit * 4 rows.
-- Schema: acore_characters. Re-runnable (CREATE TABLE IF NOT EXISTS).
-- Check: SHOW TABLES LIKE 'llm_tofu_world_history'; returns one row.
CREATE TABLE IF NOT EXISTS `llm_tofu_world_history` (
    `id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
    `speaker_guid` INT UNSIGNED NOT NULL DEFAULT 0,
    `speaker_name` VARCHAR(32) NOT NULL DEFAULT '',
    `is_bot` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `team` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0 Alliance, 1 Horde',
    `zone_id` INT UNSIGNED NOT NULL DEFAULT 0,
    `message` TEXT NOT NULL,
    `created_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
