-- The website, as tables added to an already-converted executable.
--
-- `routes` is the content: what redbean staples on as a ZIP is, here, just
-- rows next to `segments` and `symbols`. `visits` and `presses` are what the
-- running site writes back into its own file.
CREATE TABLE IF NOT EXISTS routes (
  path TEXT PRIMARY KEY,
  mime TEXT NOT NULL,
  body BLOB NOT NULL
);

CREATE TABLE IF NOT EXISTS visits (
  id   INTEGER PRIMARY KEY,
  at   TEXT NOT NULL,
  ua   TEXT,
  path TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS presses (
  id     INTEGER PRIMARY KEY,
  at     TEXT NOT NULL,
  button TEXT NOT NULL
);

-- `ldd` is a view in every SELF file; here is the same trick for the site.
CREATE VIEW IF NOT EXISTS site AS
  SELECT path, mime, length(body) AS bytes FROM routes ORDER BY path;
