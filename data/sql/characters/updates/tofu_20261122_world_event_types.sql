-- Tofu Worldview: event types of the World channel feature
-- (LLMChatterTofuWorld.cpp): tofu_world_ambient and tofu_world_player_msg.
-- Appends only the missing values to whatever llm_chatter_events.event_type
-- already holds, so this file can run before or after other enum
-- migrations, and again. Without it MySQL's strict mode rejects every
-- World event insert.
-- Schema: acore_characters. Applied by the worldserver updater at start.
-- Check: SELECT COLUMN_TYPE FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = 'acore_characters' AND TABLE_NAME = 'llm_chatter_events' AND COLUMN_NAME = 'event_type'; contains 'tofu_world_ambient','tofu_world_player_msg'.

SET @event_type = (
  SELECT COLUMN_TYPE FROM information_schema.COLUMNS
  WHERE TABLE_SCHEMA = DATABASE()
    AND TABLE_NAME = 'llm_chatter_events'
    AND COLUMN_NAME = 'event_type'
);

SET @add_values = CONCAT_WS(',',
  IF(@event_type LIKE '%''tofu_world_ambient''%', NULL, '''tofu_world_ambient'''),
  IF(@event_type LIKE '%''tofu_world_player_msg''%', NULL, '''tofu_world_player_msg''')
);

SET @sql = IF(
  @event_type IS NULL OR @add_values = '',
  'SELECT 1',
  CONCAT(
    'ALTER TABLE `llm_chatter_events` MODIFY COLUMN `event_type` ',
    LEFT(@event_type, CHAR_LENGTH(@event_type) - 1),
    ',', @add_values, ') NOT NULL'
  )
);
PREPARE stmt FROM @sql;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;
