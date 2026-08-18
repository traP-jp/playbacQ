FROM unabc/playbacq-base:v2 AS builder
# キャッシュバスター
ENV FORCE_REBUILD_CACHE_BUST=1
RUN ldconfig
ARG BUILD_JOBS=1
WORKDIR /app
COPY CMakeLists.txt /app/
COPY *.cpp /app/

COPY models/ /app/models/
COPY controllers/ /app/controllers/
COPY filters/ /app/filters/
COPY plugins/ /app/plugins/
COPY config.json config.yaml /app/

RUN mkdir build && cd build && cmake .. -DBUILD_WORKER=OFF && make -j${BUILD_JOBS}

FROM gcc:15.2
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y \
	ca-certificates \
	libcurl4 \
	libhiredis1.1.0 \
	&& rm -rf /var/lib/apt/lists/*

COPY --from=builder /usr/local/lib /usr/local/lib
COPY --from=builder /usr/lib/x86_64-linux-gnu/libjsoncpp.so* /usr/lib/x86_64-linux-gnu/
COPY --from=builder /usr/lib/x86_64-linux-gnu/libbrotli*.so* /usr/lib/x86_64-linux-gnu/
COPY --from=builder /usr/lib/x86_64-linux-gnu/libboost_*.so* /usr/lib/x86_64-linux-gnu/
RUN ldconfig

WORKDIR /app
COPY config.json config.yaml /app/

COPY --from=builder /app/build/playbacq /usr/local/bin/playbacq
RUN chmod +x /usr/local/bin/playbacq

EXPOSE 8080
CMD ["/usr/local/bin/playbacq"]