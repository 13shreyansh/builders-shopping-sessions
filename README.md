# Shopping Sessions

This C++17 pipeline reads the event, click, link and dimension CSVs in DATA_DIR and writes user_daily and campaign_daily Parquet tables in OUTPUT_DIR. The defaults are /data, /output and eight workers. Set NCPU to change the worker count. Set PROFILE to print stage times to standard error.

The pipeline joins aliases, selects the earliest well-formed copy of each event, validates the selected rows and sorts by canonical person and timestamp. It assigns sessions to their UTC starting day. A sliding seven-day click window tracks the winning click and a count of qualifying clicks for each assisting campaign. The output contains one assist per active campaign per session.

Build with `docker build --platform linux/amd64 -t shopping-sessions .`. Run with the input mounted read-only at /data and an empty writable output directory at /output. The process runs as UID 1000 and needs no network.

The entry code was written during the Builders Buildathon with Codex assistance and is released under MIT. Its third-party dependencies are Apache Arrow C++ and Parquet C++ 20.0.0, distributed in the PyArrow wheel under Apache-2.0, the GNU C++ compiler and standard library, and the Debian base system. Python and pip install the wheel during the build. No pre-event application code is included.
