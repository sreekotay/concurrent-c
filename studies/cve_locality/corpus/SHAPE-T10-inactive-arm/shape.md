# Ownership / concurrency shape

- **Taxonomy class:** **T10** (wrong representation / inactive-arm use)
- **Actors:** writer sets one arm; reader projects another (or reaches `.u`
  without domination)
- **The mistake in one sentence:** The inactive payload was treated as live
  because the tag was ignored or bypassed via raw union reach-in.
- **CC seams:** `@variant` + protected projection (`!>` / `?>` / kind /
  subject-switch domination); raw `.u` banned on `@variant` values.
