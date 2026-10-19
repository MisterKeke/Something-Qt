-- Append a deterministic 10,000-task dataset to a disposable, closed Qt profile.
-- The seed table derives IDs from the current maximum and this script never deletes rows.
BEGIN IMMEDIATE;

CREATE TEMP TABLE performance_seed (base_id INTEGER NOT NULL);
INSERT INTO performance_seed(base_id) SELECT COALESCE(MAX(id), 0) FROM tasks;

INSERT INTO tags(name, name_normalized) VALUES
    ('perf-alpha', 'perf-alpha'),
    ('perf-beta', 'perf-beta')
ON CONFLICT(name_normalized) DO NOTHING;

WITH RECURSIVE sequence(value) AS (
    VALUES(1)
    UNION ALL
    SELECT value + 1 FROM sequence WHERE value < 10000
)
INSERT INTO tasks(
    id, title, description, is_completed, created_at, updated_at,
    due_date, priority, difficulty, revision
)
SELECT seed.base_id + sequence.value,
       printf('Performance task %05d', sequence.value),
       'Deterministic local responsiveness dataset',
       CASE WHEN sequence.value % 5 = 0 THEN 1 ELSE 0 END,
       '2026-01-01T12:00:00.000Z',
       '2026-01-01T12:00:00.000Z',
       date('2026-01-01', printf('+%d days', sequence.value % 365)),
       CASE sequence.value % 3 WHEN 0 THEN 'low' WHEN 1 THEN 'medium' ELSE 'high' END,
       CASE WHEN sequence.value % 3 = 0 THEN 'hard' ELSE 'medium' END,
       1
FROM sequence CROSS JOIN performance_seed AS seed;

INSERT INTO task_tags(task_id, tag_id)
SELECT task.id, tag.id
FROM tasks AS task
CROSS JOIN performance_seed AS seed
JOIN tags AS tag ON tag.name_normalized =
    CASE WHEN task.id % 2 = 0 THEN 'perf-alpha' ELSE 'perf-beta' END
WHERE task.id > seed.base_id AND task.id <= seed.base_id + 10000;

WITH positions(position) AS (VALUES(0), (1))
INSERT INTO task_subtasks(task_id, title, is_completed, position, created_at)
SELECT task.id,
       printf('Performance subtask %d for %d', positions.position + 1, task.id),
       CASE WHEN (task.id + positions.position) % 2 = 0 THEN 1 ELSE 0 END,
       positions.position,
       '2026-01-01T12:00:00.000Z'
FROM tasks AS task
CROSS JOIN performance_seed AS seed
CROSS JOIN positions
WHERE task.id > seed.base_id AND task.id <= seed.base_id + 10000
  AND task.difficulty = 'hard';

DROP TABLE performance_seed;
COMMIT;
