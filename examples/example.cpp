#include "../dasmig/occupationgen.hpp"
#include <cstdint>
#include <iostream>

namespace
{
void print(const dasmig::occupation& o)
{
    std::cout << "  " << o.country_code << ", " << (o.person_sex == dasmig::sex::male ? "man" : "woman") << ", "
              << static_cast<int>(o.age) << ": " << dasmig::to_string(o.status);
    if (o.education) std::cout << ", " << dasmig::to_string(*o.education) << " education";
    if (o.job) std::cout << ", " << o.job->label << " (ISCO " << o.job->isco << ")";
    std::cout << '\n';
}
} // namespace

int main()
{
    // The singleton loads the lite tier (compiled in, or from resources/).
    auto& gen = dasmig::ocg::instance();
    if (!gen.has_data())
    {
        std::cerr << "Could not load the occupation data.\n";
        return 1;
    }
    std::cout << "Loaded " << gen.country_count() << " countries and " << gen.job_count() << " jobs.\n\n";

    std::cout << "Random, age 40:\n";
    for (int i = 0; i < 5; ++i)
    {
        print(gen.get_occupation());
    }

    std::cout << "\nBrazil, by age:\n";
    for (const std::uint8_t age : {12, 17, 22, 35, 58, 70})
    {
        print(gen.get_occupation({.country = "BR", .age = age}));
    }

    std::cout << "\nA 30-year-old woman in India with advanced education:\n";
    for (int i = 0; i < 3; ++i)
    {
        print(gen.get_occupation({.country = "IN",
                                  .age = 30,
                                  .person_sex = dasmig::sex::female,
                                  .education = dasmig::education_level::advanced}));
    }

    std::cout << "\nSeed 42, twice:\n";
    const auto a = gen.get_occupation("DE", std::uint64_t{42});
    const auto b = gen.get_occupation("DE", std::uint64_t{42});
    std::cout << "  " << a.to_json() << '\n'
              << "  " << b.to_json() << '\n';

    std::cout << "\nWith a layer that adds a job to ISCO group 54 (protective services):\n";
    const auto count = [&gen](const char* job_key) {
        int n = 0;
        for (std::uint64_t seed = 0; seed < 20000; ++seed)
        {
            const auto v = gen.get_occupation(
                {.country = "US", .age = 35, .status = dasmig::employment_status::employed}, seed);
            n += v.job && v.job->key == job_key ? 1 : 0;
        }
        return n;
    };
    std::cout << "  before: " << count("bounty_hunter") << " bounty hunters in 20000 employed 35-year-olds\n";
    gen.extend_from_string(dasmig::ocg::table::occupations,
                           "key\tlabel\tisco\tweight\ttags\tskills\tgear\n"
                           "bounty_hunter\tBounty hunter\t54\t50\trisk;solo\ttracking;negotiation\tvest;handcuffs\n",
                           "example.tsv");
    std::cout << "  after:  " << count("bounty_hunter") << '\n';
    return 0;
}
