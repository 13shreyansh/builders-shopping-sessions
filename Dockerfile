FROM python:3.11.12-slim-bookworm AS dependencies
RUN python -m pip install --no-cache-dir --no-deps pyarrow==20.0.0

FROM dependencies AS build
RUN apt-get update && apt-get install -y --no-install-recommends g++ && rm -rf /var/lib/apt/lists/*
WORKDIR /build
COPY input.hpp output.hpp pipeline.cpp build.sh ./
RUN ./build.sh /usr/local/bin/shopping-sessions

FROM debian:bookworm-slim
COPY --from=dependencies /usr/local/lib/python3.11/site-packages/pyarrow/libarrow.so.2000 /opt/arrow/
COPY --from=dependencies /usr/local/lib/python3.11/site-packages/pyarrow/libparquet.so.2000 /opt/arrow/
COPY --from=build /usr/local/bin/shopping-sessions /usr/local/bin/
ENV LD_LIBRARY_PATH=/opt/arrow
RUN ldd /usr/local/bin/shopping-sessions > /tmp/libs && ! grep -q 'not found' /tmp/libs && rm /tmp/libs
USER 1000:1000
ENTRYPOINT ["/usr/local/bin/shopping-sessions"]
