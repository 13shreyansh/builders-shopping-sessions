# Shopping Sessions

This C++17 pipeline reads the event, click, link and dimension CSVs in DATA_DIR and writes user_daily and campaign_daily Parquet tables in OUTPUT_DIR. The defaults are /data, /output and eight workers. Set NCPU to change the worker count. Set PROFILE to print stage times to standard error.

For each event ID, the earliest well-formed copy determines whether the event survives validation. Events and clicks are grouped by canonical person before sessionization. Each session counts on its UTC starting day. A sliding seven-day click window tracks the winning click and a count of qualifying clicks for each assisting campaign. The output contains one assist per active campaign per session.

Build with `docker build --platform linux/amd64 -t shopping-sessions .`. Run with the input mounted read-only at /data and an empty writable output directory at /output. The process runs as UID 1000 and needs no network.

The entry code was written during the Builders Buildathon with Codex assistance and is released under MIT. Its third-party dependencies are Apache Arrow C++ and Parquet C++ 20.0.0, distributed in the PyArrow wheel under Apache-2.0, the GNU C++ compiler and standard library, and the Debian base system. Python and pip install the wheel during the build. No pre-event application code is included.

Alias resolution overlaps the first event-ID scan. A membership bitmap can prove an ID absent from the alias map. Positive probes compare the full string. Short standalone ASCII IDs have a reversible integer representation. Other standalone IDs use an exact sharded map. Events use a 16-byte representation when the observed window and category count fit its fields. Wider inputs retain full millisecond timestamps and category indices. The pipeline groups equal person codes before sorting each person's events by time.

Workers partition structurally valid versions by exact event ID. Each version retains its ingest timestamp and payload validity. Full keys select the earliest copy before any payload contributes to the outputs.

Each worker counts rows by canonical person and scatters them into contiguous ranges. It sorts timestamps within each range, reuses campaign and category counters, and writes typed Parquet columns for the resulting user days. Large campaign/date domains use sparse totals.

Workers parse compact raw user identifiers during link resolution. Each raw partition resolves distinct names by full equality before routing rows to their canonical owner. User-day column pages use LZ4 compression.

Large compact timelines use radix sorting of millisecond timestamps. Short and wide-format timelines use comparison sorting.

Click IDs are compared by full CSV field on timestamp ties. Repeated identical clicks preserve winner and unique-campaign assist semantics.
