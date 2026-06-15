-- =============================================================
-- drill_summaries: never emit NULL for the optional columns.
--
-- View-only: we do NOT backfill the base table. `difficulty` is an FK to
-- drill_difficulties(id), so '' is not a valid stored value — the coalesce
-- lives in the projection, not the data.
--
-- CREATE OR REPLACE keeps the same column names, order, types, and the existing
-- grants/ownership; only the projecting expressions change.
-- =============================================================

create or replace view drill_summaries
with (security_invoker = false)
as
select
    id,
    name,
    coalesce(description, '')                  as description,
    character,
    coalesce(cpu_side, '')                     as cpu_side,
    recordings_count,
    author_handle,
    categories,
    coalesce(difficulty, '')                   as difficulty,
    size_bytes,
    downloads,
    likes,
    -- NULL when the uploader account was deleted; present it as "not mine".
    coalesce(uploader_id = auth.uid(), false)  as is_mine,
    created_at
from drills
where status = 'public';
