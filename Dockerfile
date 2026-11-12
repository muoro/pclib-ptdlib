# syntax=docker/dockerfile:1
ARG UBUNTU_IMAGE=ubuntu:24.04@sha256:2198e4c65076e9cb0b3d695219df667c0e3b0072da64ca3c2648aca17dcecaf5

FROM ${UBUNTU_IMAGE} AS llvm-build
ARG LLVM_REVISION=c33c978d766a6bbaec28fce7638354c549a75111
ARG BUILD_JOBS=4

RUN test "$(dpkg --print-architecture)" = amd64 || \
    { echo 'This image currently supports linux/amd64; build with --platform=linux/amd64.' >&2; exit 1; }
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential ca-certificates cmake git ninja-build python3 \
      libz3-dev zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

# Cache the expensive toolchain separately from changes to the analyzer.
RUN git init /opt/llvm-src \
    && git -C /opt/llvm-src remote add origin https://github.com/llvm/llvm-project.git \
    && git -C /opt/llvm-src fetch --depth 1 origin "${LLVM_REVISION}" \
    && git -C /opt/llvm-src checkout --detach FETCH_HEAD \
    && test "$(git -C /opt/llvm-src rev-parse HEAD)" = "${LLVM_REVISION}"
RUN cmake -S /opt/llvm-src/llvm -B /opt/llvm-build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DLLVM_ENABLE_PROJECTS=clang \
      -DLLVM_TARGETS_TO_BUILD=X86 \
      -DLLVM_ENABLE_ASSERTIONS=ON \
      -DLLVM_ENABLE_RTTI=ON \
      -DLLVM_ENABLE_Z3_SOLVER=ON \
      -DLLVM_INCLUDE_TESTS=OFF \
      -DLLVM_INCLUDE_BENCHMARKS=OFF \
      -DLLVM_INCLUDE_EXAMPLES=OFF \
      -DLLVM_ENABLE_LIBEDIT=OFF \
      -DLLVM_ENABLE_LIBXML2=OFF \
      -DLLVM_ENABLE_ZSTD=OFF \
      -DLLVM_PARALLEL_LINK_JOBS=1 \
    && cmake --build /opt/llvm-build \
      --target clangTooling clang-resource-headers --parallel "${BUILD_JOBS}"

FROM llvm-build AS analyzer-build
WORKDIR /src
COPY CMakeLists.txt ./
COPY include/ include/
COPY src/ src/
RUN cmake -S . -B build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
      -DLLVM_DIR=/opt/llvm-build/lib/cmake/llvm \
      -DClang_DIR=/opt/llvm-build/lib/cmake/clang \
    && cmake --build build --target tu_pccscc --parallel "${BUILD_JOBS}" \
    && install -Dm755 build/bin/tu_pccscc /opt/pccscc/bin/tu_pccscc \
    && strip /opt/pccscc/bin/tu_pccscc \
    && /opt/pccscc/bin/tu_pccscc --build-info > /opt/pccscc/build-info.json

FROM ${UBUNTU_IMAGE} AS runtime
LABEL org.opencontainers.image.title="PClib and PTDlib" \
      org.opencontainers.image.description="PCCSCC path analysis and Z3 test-input generation" \
      org.opencontainers.image.source="https://github.com/muoro/pclib-ptdlib"

RUN apt-get update && apt-get install -y --no-install-recommends \
      libz3-4 zlib1g libstdc++6 libc6-dev \
    && rm -rf /var/lib/apt/lists/* \
    && mkdir -p /reports /opt/pccscc \
    && chmod 1777 /reports \
    && dpkg-query -W > /opt/pccscc/runtime-packages.txt
COPY --from=analyzer-build /opt/pccscc/bin/tu_pccscc /usr/local/bin/tu_pccscc
COPY --from=analyzer-build /opt/pccscc/build-info.json /opt/pccscc/build-info.json
# Clang finds its builtin headers relative to the analyzer executable.
COPY --from=llvm-build /opt/llvm-build/lib/clang/22/ /usr/local/lib/clang/22/
COPY --from=llvm-build /opt/llvm-src/llvm/LICENSE.TXT /opt/pccscc/licenses/LLVM.txt
COPY --from=llvm-build /opt/llvm-src/clang/LICENSE.TXT /opt/pccscc/licenses/Clang.txt
COPY examples/thesis/ /opt/pccscc/examples/
ENV HOME=/tmp
WORKDIR /reports
USER 1000:1000
ENTRYPOINT ["/usr/local/bin/tu_pccscc"]
CMD ["/opt/pccscc/examples/nested_if.c", "--report-dir=/reports", "--", "-std=c11"]
