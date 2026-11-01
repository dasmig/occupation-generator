// Draw people per country and sex, with ages from a population table, and write
// the income of each employed one with its major group, age and education; for
// scripts/validate_income.py (doc/data.md, "How well it matches").
// Build: clang++ -std=c++23 -O2 -I. -o build/income_sample scripts/income_sample.cpp
// Usage: income_sample TIER_DIR AGES_TSV N OUT_TSV
// AGES_TSV: country_code, sex, age, weight (no header), from validate_income.py ages.
#include "../dasmig/occupationgen.hpp"
#include <cstdio>
#include <fstream>
#include <sstream>

int main(int argc, char** argv)
try
{
    if (argc != 5) return 2;
    dasmig::ocg gen;
    gen.load(std::filesystem::path{argv[1]});
    std::map<std::pair<std::string, std::string>, std::pair<std::vector<int>, std::vector<double>>> ages;
    std::ifstream in{argv[2]};
    std::string c, s;
    int a = 0;
    double w = 0;
    while (in >> c >> s >> a >> w)
    {
        auto& e = ages[{c, s}];
        e.first.push_back(a);
        e.second.push_back(w);
    }
    const int n = std::atoi(argv[3]);
    std::ofstream out{argv[4]};
    out << "country_code\tsex\tmajor\tage\teducation\tincome\n";
    std::uint64_t seed = 1;
    for (const auto& [key, e] : ages)
    {
        if (!gen.income_currency(key.first)) continue;
        dasmig::core::rng r{seed++};
        dasmig::core::discrete pick{e.second};
        const auto sx = key.second == "male" ? dasmig::sex::male : dasmig::sex::female;
        for (int i = 0; i < n; ++i)
        {
            const int age = e.first[pick(r)];
            const auto v = gen.get_occupation(
                {.country = key.first, .age = static_cast<std::uint8_t>(age), .person_sex = sx}, seed * 1000003 + i);
            if (!v.income) continue;
            out << key.first << '\t' << key.second << '\t' << v.job->isco[0] << '\t' << age << '\t'
                << (v.education ? dasmig::to_string(*v.education) : "") << '\t' << v.income->major() << '\n';
        }
    }
}
catch (const std::exception& e)
{
    std::printf("error: %s\n", e.what());
    return 1;
}
