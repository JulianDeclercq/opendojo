-- =============================================================
-- drill_summaries: never emit NULL for the optional columns.
--
-- View-only: we do NOT backfill the base table. `difficulty` is an FK to
-- drill_difficulties(id), so '' is not a valid stored value — the coalesce
-- lives in the projection, not the data.
--
-- CREATE OR REPLACE keeps the same column names, order, types, and the existing
-- grants/ownership; only the projecting expressions change. The column list
-- (including the trailing `liked_by_me` added in 20260613000001) must match the
-- current view exactly — OR REPLACE cannot drop or reorder columns (42P16).
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
    -- NULL when the uploader account was deleted (uploader_id ON DELETE SET
    -- NULL); present it as "not mine". Display hint only — write authority is
    -- enforced server-side by matching uploader_id = auth.uid().
    coalesce(uploader_id = auth.uid(), false)  as is_mine,
    created_at,
    -- exists(...) is always boolean, never null — no coalesce needed. Must be
    -- kept (same as 20260613000001) so OR REPLACE doesn't drop the column.
    exists (
        select 1 from likes l
        where l.drill_id = drills.id
          and l.user_id = auth.uid()
    ) as liked_by_me
from drills
where status = 'public';
