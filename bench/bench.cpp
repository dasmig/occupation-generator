// Benchmarks for occupation-generator; see bench.hpp for the options.

#include "../dasmig/occupationgen.hpp"
#include "bench.hpp"

int main(int argc, char** argv)
{
    dasmig::bench::runner run{"occupation-generator", argc, argv};

    run.load("load", [&] {
        dasmig::ocg gen;
        return gen.load(run.tier());
    });

    dasmig::ocg gen;
    if (!gen.load(run.tier())) return run.missing_tier();

    run.calls("get_occupation", [&](std::uint64_t seed) { return gen.get_occupation({}, seed); });
    run.calls("get_occupation BR female 30", [&](std::uint64_t seed) {
        return gen.get_occupation({.country = "BR", .age = 30, .person_sex = dasmig::sex::female}, seed);
    });
    run.calls("get_occupation to_json", [&](std::uint64_t seed) {
        return gen.get_occupation({}, seed).to_json();
    });

    return run.finish();
}
