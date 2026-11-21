FROM python:3.11.12-slim-bookworm@sha256:dbf1de478a55d6763afaa39c2f3d7b54b25230614980276de5cacdde79529d0c AS dependencies
RUN python -m pip install --no-cache-dir --no-deps pyarrow==20.0.0

RUN mkdir /arrow-licenses && find /usr/local/lib/python3.11/site-packages/pyarrow* -type f \( -iname LICENSE.txt -o -iname NOTICE.txt \) -exec cp -t /arrow-licenses {} + && test -s /arrow-licenses/LICENSE.txt

FROM dependencies AS build
RUN apt-get update && apt-get install -y --no-install-recommends g++=4:12.2.0-3 && rm -rf /var/lib/apt/lists/*
WORKDIR /build
COPY input.hpp output.hpp pipeline.cpp build.sh ./
RUN ./build.sh /usr/local/bin/shopping-sessions

FROM debian:bookworm-slim@sha256:734a72b3d6142a71993b4bc580ca01d8ca687f77a2c422d96a961d3376b179bc
COPY --from=dependencies /usr/local/lib/python3.11/site-packages/pyarrow/libarrow.so.2000 /opt/arrow/
COPY --from=dependencies /usr/local/lib/python3.11/site-packages/pyarrow/libparquet.so.2000 /opt/arrow/
COPY --from=dependencies /arrow-licenses/ /usr/share/doc/shopping-sessions/arrow/
COPY LICENSE /usr/share/doc/shopping-sessions/
COPY --from=build /usr/local/bin/shopping-sessions /usr/local/bin/
ENV LD_LIBRARY_PATH=/opt/arrow
RUN ldd /usr/local/bin/shopping-sessions > /tmp/libs && ! grep -q 'not found' /tmp/libs && rm /tmp/libs
USER 1000:1000
ENTRYPOINT ["/usr/local/bin/shopping-sessions"]
