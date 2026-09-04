#include "gamebattle/wire.hpp"

#include <erl_nif.h>

#include <span>

namespace {

ERL_NIF_TERM dispatch(ErlNifEnv* env, ERL_NIF_TERM request_term) {
    ErlNifBinary request{};
    if (!enif_term_to_binary(env, request_term, &request)) {
        return enif_make_badarg(env);
    }
    const auto response = gamebattle::wire::handle_etf(
        std::span<const std::uint8_t>(request.data, request.size));
    enif_release_binary(&request);

    ERL_NIF_TERM result;
    if (enif_binary_to_term(env, response.data(), response.size(), &result, 0) == 0) {
        return enif_make_tuple2(env, enif_make_atom(env, "error"),
                                enif_make_atom(env, "response_decode_failed"));
    }
    return result;
}

ERL_NIF_TERM simulate(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]) {
    if (argc != 1) {
        return enif_make_badarg(env);
    }
    return dispatch(env, argv[0]);
}

ERL_NIF_TERM load_config(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]) {
    if (argc != 1 || !enif_is_binary(env, argv[0])) {
        return enif_make_badarg(env);
    }
    const auto command = enif_make_atom(env, "load_config");
    return dispatch(env, enif_make_tuple2(env, command, argv[0]));
}

ErlNifFunc functions[] = {
    {"simulate", 1, simulate, ERL_NIF_DIRTY_JOB_CPU_BOUND},
    {"load_config", 1, load_config, ERL_NIF_DIRTY_JOB_IO_BOUND}
};

} // namespace

ERL_NIF_INIT(gamebattle_nif, functions, nullptr, nullptr, nullptr, nullptr)
