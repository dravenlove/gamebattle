# Targets:
#   build      the build environment: compiles and tests everything
#   artifacts  just the outputs, for --output type=local,dest=dist
#   runtime    (default) the deploy image: a standalone battle node
#
# The release bundles the ERTS of ERLANG_IMAGE, so RUNTIME_IMAGE must be the
# Debian release that image is built on (bookworm for erlang:26).
ARG ERLANG_IMAGE=erlang:26
ARG RUNTIME_IMAGE=debian:bookworm-slim

# ---------------------------------------------------------------- build
FROM ${ERLANG_IMAGE} AS build

RUN apt-get update \
 && apt-get install -y --no-install-recommends cmake \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# C++: battle core, Port, NIF, config compiler and their tests. The Port and
# NIF are installed into erlang/priv, the compiler into erlang/bin.
COPY CMakeLists.txt CMakePresets.json ./
COPY include include
COPY src src
COPY tools tools
COPY tests tests
COPY config config
RUN ERTS_INCLUDE_DIR="$(erl -noshell -eval 'io:format("~s/erts-~s/include", [code:root_dir(), erlang:system_info(version)]), halt().')" \
 && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
        -DGAMEBATTLE_BUILD_TESTS=ON \
        -DGAMEBATTLE_BUILD_NIF=ON \
        -DGAMEBATTLE_BUILD_CONFIG_COMPILER=ON \
        -DERLANG_ERTS_INCLUDE_DIR="$ERTS_INCLUDE_DIR" \
 && cmake --build build --parallel \
 && ctest --test-dir build --output-on-failure \
 && cmake --install build --prefix erlang --component BattleRuntime \
 && cmake --install build --prefix erlang --component ConfigTools

ENV PATH=/src/erlang/bin:$PATH

# Erlang: fetch the rebar3 plugins in their own layer, then compile, test
# against the Port just built, and assemble the release.
WORKDIR /src/erlang
COPY erlang/rebar.config ./
RUN rebar3 get-deps
COPY proto /src/proto
COPY erlang/config config
COPY erlang/src src
COPY erlang/test test
RUN GAMEBATTLE_PORT=/src/erlang/priv/gamebattle_port \
    GAMEBATTLE_TEST_CONFIG=/src/build/generated-config/example.gbcfg \
    rebar3 eunit \
 && rebar3 as prod tar

# ---------------------------------------------------------------- artifacts
FROM scratch AS artifacts
COPY --from=build /src/erlang/priv/gamebattle_port /src/erlang/priv/gamebattle_nif.so /priv/
COPY --from=build /src/erlang/bin/gamebattle_config_compiler /bin/
COPY --from=build /src/erlang/_build/prod/rel/gamebattle/gamebattle-*.tar.gz /

# ---------------------------------------------------------------- runtime
FROM ${RUNTIME_IMAGE} AS runtime

RUN groupadd --system gamebattle \
 && useradd --system --gid gamebattle --home-dir /opt/gamebattle --no-create-home gamebattle

# Owned by root and read-only to the node: files generated at start-up and
# crash dumps go to /tmp.
COPY --from=build /src/erlang/_build/prod/rel/gamebattle /opt/gamebattle
COPY --chmod=0755 docker/entrypoint.sh /usr/local/bin/gamebattle-entrypoint

ENV RELX_OUT_FILE_PATH=/tmp \
    ERL_CRASH_DUMP=/tmp/erl_crash.dump

USER gamebattle
WORKDIR /opt/gamebattle

# epmd and the distribution port (DIST_PORT in vm.args.src), and the test
# gateway's usual port (only open when GAMEBATTLE_GATEWAY_PORT is set).
EXPOSE 4369 9100 7000

ENTRYPOINT ["gamebattle-entrypoint"]
CMD ["foreground"]
