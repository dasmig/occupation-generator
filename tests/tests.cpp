#include "catch_amalgamated.hpp"
#include "../dasmig/occupationgen.hpp"

#include <concepts>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>

using dasmig::education_level;
using dasmig::employment_status;
using dasmig::sex;
using table = dasmig::ocg::table;
using Catch::Approx;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

template <class Generator, class... Args>
concept accepts_occupation = requires(Generator& gen, const Args&... args) {
    { gen.get_occupation(args...) } -> std::same_as<dasmig::occupation>;
};

template <class Generator>
concept has_seed_getter = requires(Generator& gen) { gen.seed(); };

template <class Generator>
concept has_seed_setter = requires(Generator& gen) { gen.seed(std::uint64_t{7}); };

template <class Generator>
concept has_unseed = requires(Generator& gen) { gen.unseed(); };

static_assert(accepts_occupation<dasmig::ocg, dasmig::occupation_request>);
static_assert(accepts_occupation<const dasmig::ocg, dasmig::occupation_request, std::uint64_t>);
static_assert(!accepts_occupation<dasmig::ocg>);
static_assert(!accepts_occupation<dasmig::ocg, std::uint64_t>);
static_assert(!accepts_occupation<dasmig::ocg, std::string_view>);
static_assert(!accepts_occupation<dasmig::ocg, std::string_view, std::uint64_t>);
static_assert(!accepts_occupation<dasmig::ocg, std::uint64_t, dasmig::occupation_request>);
static_assert(!has_seed_getter<dasmig::ocg>);
static_assert(!has_seed_setter<dasmig::ocg>);
static_assert(!has_unseed<dasmig::ocg>);

static dasmig::ocg make_loaded_generator()
{
    dasmig::ocg gen;
    if (!gen.load(dasmig::dataset::lite))
    {
        for (const auto* candidate : {"resources/lite", "../resources/lite"})
        {
            if (std::filesystem::is_directory(candidate))
            {
                gen.load(std::filesystem::path{candidate});
                break;
            }
        }
    }
    REQUIRE(gen.has_data());
    return gen;
}

// A world of one country, "AA", with two ISCO groups:
//   11 (major 1) and 21 (major 2); shares for men 30:70, for women 80:20.
//   15-64: 60% employed, 10% unemployed, 30% inactive, of whom half study;
//   65+:   10% employed, 90% inactive, all retired.
//   Education 25% each at 15-64; at 65+ everyone has less than basic.
static const char* const tiny_jobs = "key\tlabel\tisco\tweight\tmin_age\tmax_age\ttags\tskills\tgear\n"
                                     "boss\tBoss\t11\t1\t\t\tleadership\tcommand;budgeting\tsuit\n"
                                     "analyst\tAnalyst\t21\t3\t\t\tdesk\tmodelling\tlaptop\n"
                                     "clerk\tClerk\t21\t1\t\t\t\t\t\n";

static dasmig::ocg tiny_world(const std::string& jobs = tiny_jobs, const std::string& factors = {})
{
    dasmig::ocg gen;
    gen.extend_from_string(table::status,
                           "country_code\tsex\tband\temployed\tunemployed\tinactive\n"
                           "AA\tmale\t15-64\t60\t10\t30\n"
                           "AA\tmale\t65+\t10\t0\t90\n"
                           "AA\tfemale\t15-64\t60\t10\t30\n"
                           "AA\tfemale\t65+\t10\t0\t90\n",
                           "status.tsv");
    gen.extend_from_string(table::inactivity,
                           "band\tstudent\tretired\tother\n15-64\t0.5\t0\t0.5\n65+\t0\t1\t0\n", "inactivity.tsv");
    gen.extend_from_string(table::education,
                           "country_code\tsex\tband\tless_than_basic\tbasic\tintermediate\tadvanced\n"
                           "AA\tmale\t15-64\t25\t25\t25\t25\n"
                           "AA\tmale\t65+\t100\t0\t0\t0\n"
                           "AA\tfemale\t15-64\t25\t25\t25\t25\n"
                           "AA\tfemale\t65+\t100\t0\t0\t0\n",
                           "education.tsv");
    gen.extend_from_string(table::isco, "code\tlabel\tmajor\tskill\n11\tManagers\t1\t3-4\n21\tProfessionals\t2\t3-4\n",
                           "isco.tsv");
    gen.extend_from_string(table::shares,
                           "country_code\tsex\tcode\tshare\n"
                           "AA\tmale\t11\t30\nAA\tmale\t21\t70\nAA\tfemale\t11\t80\nAA\tfemale\t21\t20\n",
                           "shares.tsv");
    if (!factors.empty()) gen.extend_from_string(table::factors, factors, "factors.tsv");
    gen.extend_from_string(table::occupations, jobs, "occupations.tsv");
    return gen;
}

static constexpr std::uint64_t draws = 20000;

// Calls f(occupation) for the first `draws` seeds.
template <class F>
static void sample(const dasmig::ocg& gen, const dasmig::occupation_request& req, F&& f, std::uint64_t n = draws)
{
    for (std::uint64_t s = 0; s < n; ++s) f(gen.get_occupation(req, s));
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

TEST_CASE("load(dataset::lite) loads every table", "[loading]")
{
    const auto gen = make_loaded_generator();
    REQUIRE(gen.country_count() > 200);
    REQUIRE(gen.job_count() > 80);
    REQUIRE(gen.unresolved().empty());
    for (const auto t : {table::status, table::inactivity, table::education, table::isco, table::shares,
                         table::factors, table::occupations})
    {
        REQUIRE(gen.layers(t).size() == 1);
    }
    REQUIRE(gen.data_hash() != 0);
}

TEST_CASE("every lite country has shares that add up to about 100", "[loading][data]")
{
    const auto gen = make_loaded_generator();
    // The shipped shares are written to four decimals; 213 countries x 2 sexes.
    std::map<std::string, double> total;
    const auto dir = std::filesystem::path{"resources/lite"};
    const auto path = std::filesystem::is_directory(dir) ? dir : std::filesystem::path{"../resources/lite"};
    std::ifstream in{path / "shares.tsv"};
    REQUIRE(in.good());
    std::string line;
    std::getline(in, line);
    while (std::getline(in, line))
    {
        std::istringstream row{line};
        std::string country, sx, code, share;
        std::getline(row, country, '\t');
        std::getline(row, sx, '\t');
        std::getline(row, code, '\t');
        std::getline(row, share, '\t');
        total[country + sx] += std::stod(share);
    }
    REQUIRE(total.size() == gen.country_count() * 2);
    for (const auto& [key, sum] : total)
    {
        INFO(key);
        REQUIRE(sum == Approx(100.0).margin(0.01));
    }
}

TEST_CASE("load - non-existent path is a no-op", "[loading]")
{
    dasmig::ocg gen;
    gen.load(std::filesystem::path{"no/such/directory"});
    REQUIRE_FALSE(gen.has_data());
    REQUIRE(gen.data_hash() == 0);
}

#ifndef DASMIG_OCCUPATION_EMBED // compiled-in data is never missing
TEST_CASE("load(dataset) returns false for a missing tier", "[loading]")
{
    const auto old_cwd = std::filesystem::current_path();
    const auto tmp = std::filesystem::temp_directory_path() / "ocg_empty_dir";
    std::filesystem::create_directories(tmp);
    std::filesystem::current_path(tmp);
    dasmig::ocg gen;
    const bool loaded = gen.load(dasmig::dataset::full);
    std::filesystem::current_path(old_cwd);
    std::filesystem::remove_all(tmp);
    REQUIRE_FALSE(loaded);
    REQUIRE_FALSE(gen.has_data());
}
#endif

TEST_CASE("the full tier has a larger catalogue and the same measured data", "[loading]")
{
    const auto lite = make_loaded_generator();
    dasmig::ocg full;
    if (!full.load(dasmig::dataset::full))
    {
        for (const auto* candidate : {"resources/full", "../resources/full"})
        {
            if (std::filesystem::is_directory(candidate)) full.load(std::filesystem::path{candidate});
        }
    }
    if (!full.has_data()) SKIP("the full tier is not available");
    REQUIRE(full.job_count() > lite.job_count());
    REQUIRE(full.country_count() == lite.country_count());
    REQUIRE(full.unresolved().empty());
}

TEST_CASE("the singleton loads data on first use", "[loading]")
{
    REQUIRE(dasmig::ocg::instance().has_data());
}

// ---------------------------------------------------------------------------
// Generation
// ---------------------------------------------------------------------------

TEST_CASE("get_occupation throws when no data is loaded", "[generation]")
{
    dasmig::ocg gen;
    REQUIRE_THROWS_AS(gen.get_occupation({}), std::runtime_error);
    REQUIRE_THROWS_AS(gen.get_occupation({.country = "BR"}, std::uint64_t{1}), std::runtime_error);
}

TEST_CASE("occupation_request preserves the default generation choices", "[generation][request]")
{
    const dasmig::occupation_request req{};
    REQUIRE(req.country.empty());
    REQUIRE(req.age == 40);
    REQUIRE_FALSE(req.person_sex);
    REQUIRE_FALSE(req.education);
    REQUIRE_FALSE(req.status);

    auto gen = tiny_world();
    REQUIRE(gen.get_occupation(req).age == 40);
    REQUIRE(gen.get_occupation(req, 0).age == 40);
}

TEST_CASE("both request overloads respect every supplied field", "[generation][request]")
{
    auto gen = tiny_world();
    const auto hash = gen.data_hash();
    const dasmig::occupation_request req{
        .country = "AA", .age = 30, .person_sex = sex::female,
        .education = education_level::basic, .status = employment_status::retired};
    for (const auto& v : {gen.get_occupation(req), gen.get_occupation(req, 7)})
    {
        REQUIRE(v.country_code == req.country);
        REQUIRE(v.age == req.age);
        REQUIRE(v.person_sex == req.person_sex);
        REQUIRE(v.education == req.education);
        REQUIRE(v.status == req.status);
        REQUIRE(v.job);
        REQUIRE(v.signature().data == hash);
        REQUIRE(gen.get_occupation(req, v.seed()).to_json() == v.to_json());
    }
    REQUIRE(gen.data_hash() == hash);
}

TEST_CASE("an unknown country is rejected", "[generation][errors]")
{
    const auto gen = tiny_world();
    REQUIRE_THROWS_AS(gen.get_occupation({.country = "ZZ"}, std::uint64_t{1}), std::invalid_argument);
    REQUIRE_THROWS_AS(gen.get_occupation({.country = "aa"}, std::uint64_t{1}), std::invalid_argument);
}

TEST_CASE("the country, sex and age are reported", "[generation]")
{
    const auto gen = tiny_world();
    const auto v = gen.get_occupation({.country = "AA", .age = 33, .person_sex = sex::female}, 5);
    REQUIRE(v.country_code == "AA");
    REQUIRE(v.person_sex == sex::female);
    REQUIRE(v.age == 33);
    REQUIRE(gen.get_occupation({.country = "AA"}, std::uint64_t{5}).age == 40); // the default
}

TEST_CASE("the status follows the country's rates for the age band", "[generation][stats]")
{
    const auto gen = tiny_world();
    std::map<employment_status, int> at40;
    std::map<employment_status, int> at70;
    sample(gen, {.country = "AA", .age = 40}, [&](const auto& v) { ++at40[v.status]; });
    sample(gen, {.country = "AA", .age = 70}, [&](const auto& v) { ++at70[v.status]; });
    const auto share = [](auto& m, employment_status s) { return static_cast<double>(m[s]) / static_cast<double>(draws); };
    REQUIRE(share(at40, employment_status::employed) == Approx(0.60).margin(0.02));
    REQUIRE(share(at40, employment_status::unemployed) == Approx(0.10).margin(0.02));
    REQUIRE(share(at40, employment_status::student) == Approx(0.15).margin(0.02)); // half the inactive
    REQUIRE(share(at40, employment_status::inactive) == Approx(0.15).margin(0.02));
    REQUIRE(at40[employment_status::retired] == 0);
    REQUIRE(share(at70, employment_status::employed) == Approx(0.10).margin(0.02));
    REQUIRE(share(at70, employment_status::retired) == Approx(0.90).margin(0.02));
    REQUIRE(at70[employment_status::student] == 0);
}

TEST_CASE("a child is a student with no job and no education", "[generation]")
{
    const auto gen = tiny_world();
    sample(gen, {.country = "AA", .age = 14}, [](const auto& v) {
        REQUIRE(v.status == employment_status::student);
        REQUIRE_FALSE(v.job);
        REQUIRE_FALSE(v.education);
    }, 200);
}

TEST_CASE("a child under school age is inactive with no job and no education", "[generation]")
{
    const auto gen = tiny_world();
    for (const std::uint8_t age : {0, 3, 5})
    {
        sample(gen, {.country = "AA", .age = age}, [](const auto& v) {
            REQUIRE(v.status == employment_status::inactive);
            REQUIRE_FALSE(v.job);
            REQUIRE_FALSE(v.education);
        }, 100);
    }
    sample(gen, {.country = "AA", .age = 6}, [](const auto& v) { REQUIRE(v.status == employment_status::student); }, 100);
}

TEST_CASE("a student has no job and every other status has one", "[generation]")
{
    const auto gen = tiny_world();
    int students = 0;
    sample(gen, {.country = "AA", .age = 30}, [&](const auto& v) {
        REQUIRE(v.job.has_value() == (v.status != employment_status::student));
        students += v.status == employment_status::student ? 1 : 0;
    }, 2000);
    REQUIRE(students > 0);
}

TEST_CASE("a forced status, sex and education are kept", "[generation]")
{
    const auto gen = tiny_world();
    sample(gen,
           {.country = "AA", .age = 30, .person_sex = sex::female, .education = education_level::basic,
            .status = employment_status::retired},
           [](const auto& v) {
               REQUIRE(v.status == employment_status::retired);
               REQUIRE(v.person_sex == sex::female);
               REQUIRE(v.education == education_level::basic);
               REQUIRE(v.job);
           },
           500);
}

TEST_CASE("the ISCO group follows the country's shares for the sex", "[generation][stats]")
{
    const auto gen = tiny_world();
    for (const auto& [person_sex, expected] : {std::pair{sex::male, 0.70}, std::pair{sex::female, 0.20}})
    {
        int jobs = 0;
        int group21 = 0;
        sample(gen, {.country = "AA", .age = 40, .person_sex = person_sex}, [&](const auto& v) {
            if (!v.job) return;
            ++jobs;
            group21 += v.job->isco == "21" ? 1 : 0;
        });
        REQUIRE(static_cast<double>(group21) / jobs == Approx(expected).margin(0.02));
    }
}

TEST_CASE("the job within a group follows the catalogue weights", "[generation][stats]")
{
    const auto gen = tiny_world();
    int group21 = 0;
    int analysts = 0;
    sample(gen, {.country = "AA", .age = 40, .person_sex = sex::male}, [&](const auto& v) {
        if (!v.job || v.job->isco != "21") return;
        ++group21;
        analysts += v.job->key == "analyst" ? 1 : 0;
    });
    REQUIRE(static_cast<double>(analysts) / group21 == Approx(0.75).margin(0.02));
}

TEST_CASE("a job carries its group, skills, gear and tags", "[generation]")
{
    const auto gen = tiny_world();
    bool seen = false;
    sample(gen, {.country = "AA", .age = 40, .person_sex = sex::female}, [&](const auto& v) {
        if (!v.job || v.job->key != "boss") return;
        seen = true;
        REQUIRE(v.job->label == "Boss");
        REQUIRE(v.job->isco == "11");
        REQUIRE(v.job->isco_label == "Managers");
        REQUIRE(v.job->skill == "3-4");
        REQUIRE(v.job->skills == std::vector<std::string>{"command", "budgeting"});
        REQUIRE(v.job->gear == std::vector<std::string>{"suit"});
        REQUIRE(v.job->tags == std::vector<std::string>{"leadership"});
    }, 100);
    REQUIRE(seen);
}

TEST_CASE("an unknown sex is drawn with even odds", "[generation][stats]")
{
    const auto gen = tiny_world();
    int women = 0;
    sample(gen, {.country = "AA"}, [&](const auto& v) { women += v.person_sex == sex::female ? 1 : 0; });
    REQUIRE(static_cast<double>(women) / draws == Approx(0.5).margin(0.02));
}

TEST_CASE("a random country is uniform over those with status rows for both sexes", "[generation][stats]")
{
    auto gen = tiny_world();
    gen.extend_from_string(table::status,
                           "country_code\tsex\tband\temployed\tunemployed\tinactive\n"
                           "AB\tmale\t15-64\t50\t0\t50\nAB\tfemale\t15-64\t50\t0\t50\n"
                           "AC\tmale\t15-64\t50\t0\t50\n", // no women: not drawn
                           "more.tsv");
    REQUIRE(gen.country_count() == 2);
    std::map<std::string, int> seen;
    for (std::uint64_t s = 0; s < draws; ++s)
    {
        ++seen[gen.get_occupation({.status = employment_status::student}, s).country_code];
    }
    REQUIRE(seen.size() == 2);
    REQUIRE(static_cast<double>(seen["AA"]) / draws == Approx(0.5).margin(0.02));
}

TEST_CASE("a country with no status band for the age or no shares is rejected", "[generation][errors]")
{
    auto gen = tiny_world();
    gen.extend_from_string(table::status,
                           "country_code\tsex\tband\temployed\tunemployed\tinactive\n"
                           "AD\tmale\t15-30\t50\t0\t50\nAD\tfemale\t15-30\t50\t0\t50\n"
                           "AE\tmale\t15-64\t50\t0\t50\nAE\tfemale\t15-64\t50\t0\t50\n",
                           "more.tsv");
    REQUIRE_THROWS_AS(gen.get_occupation({.country = "AD", .age = 40}, 1), std::invalid_argument);
    REQUIRE(gen.get_occupation({.country = "AD", .age = 20, .status = employment_status::student}, 1).status ==
            employment_status::student);
    // AE has status rows but no shares: nobody can be given a job.
    REQUIRE_THROWS_AS(gen.get_occupation({.country = "AE", .age = 40, .status = employment_status::employed}, 1),
                      std::invalid_argument);
}

// ---------------------------------------------------------------------------
// Age and education
// ---------------------------------------------------------------------------

TEST_CASE("a job with an age limit is closed to people outside it", "[age]")
{
    const auto gen = tiny_world("key\tlabel\tisco\tweight\tmin_age\tmax_age\n"
                                "boss\tBoss\t11\t1\t\t\n"
                                "judge\tJudge\t21\t1\t30\t60\n");
    sample(gen, {.country = "AA", .age = 25, .person_sex = sex::male}, [](const auto& v) {
        if (v.job) REQUIRE(v.job->key == "boss"); // group 21 has no job open at 25
    }, 2000);
    sample(gen, {.country = "AA", .age = 61, .person_sex = sex::male}, [](const auto& v) {
        if (v.job) REQUIRE(v.job->key == "boss");
    }, 2000);
    int judges = 0;
    int jobs = 0;
    sample(gen, {.country = "AA", .age = 40, .person_sex = sex::male}, [&](const auto& v) {
        if (!v.job) return;
        ++jobs;
        judges += v.job->key == "judge" ? 1 : 0;
    });
    REQUIRE(static_cast<double>(judges) / jobs == Approx(0.70).margin(0.02));
}

TEST_CASE("age factors multiply the shares", "[age][stats]")
{
    // Group 21 is half as likely under 30.
    const auto gen = tiny_world(tiny_jobs,
                                "country_code\tmajor\tdimension\tvalue\tfactor\n"
                                "AA\t2\tage_band\t15-29\t0.5\n");
    int jobs = 0;
    int group21 = 0;
    sample(gen, {.country = "AA", .age = 20, .person_sex = sex::male}, [&](const auto& v) {
        if (!v.job) return;
        ++jobs;
        group21 += v.job->isco == "21" ? 1 : 0;
    });
    REQUIRE(static_cast<double>(group21) / jobs == Approx(35.0 / 65.0).margin(0.02));
    jobs = group21 = 0;
    sample(gen, {.country = "AA", .age = 40, .person_sex = sex::male}, [&](const auto& v) {
        if (!v.job) return;
        ++jobs;
        group21 += v.job->isco == "21" ? 1 : 0;
    });
    REQUIRE(static_cast<double>(group21) / jobs == Approx(0.70).margin(0.02));
}

TEST_CASE("education factors multiply the shares", "[education][stats]")
{
    const auto gen = tiny_world(tiny_jobs,
                                "country_code\tmajor\tdimension\tvalue\tfactor\n"
                                "AA\t1\teducation\tadvanced\t4\n");
    const auto group11 = [&](education_level level) {
        int jobs = 0;
        int n = 0;
        sample(gen, {.country = "AA", .age = 40, .person_sex = sex::male, .education = level}, [&](const auto& v) {
            if (!v.job) return;
            ++jobs;
            n += v.job->isco == "11" ? 1 : 0;
        });
        return static_cast<double>(n) / jobs;
    };
    REQUIRE(group11(education_level::advanced) == Approx(120.0 / 190.0).margin(0.02));
    REQUIRE(group11(education_level::basic) == Approx(0.30).margin(0.02));
}

TEST_CASE("the education is drawn from the country, sex and age band", "[education][stats]")
{
    const auto gen = tiny_world();
    std::map<education_level, int> at40;
    sample(gen, {.country = "AA", .age = 40}, [&](const auto& v) { ++at40[*v.education]; });
    for (const auto level : {education_level::less_than_basic, education_level::basic, education_level::intermediate,
                             education_level::advanced})
    {
        REQUIRE(static_cast<double>(at40[level]) / draws == Approx(0.25).margin(0.02));
    }
    sample(gen, {.country = "AA", .age = 70}, [](const auto& v) { REQUIRE(v.education == education_level::less_than_basic); },
           500);
}

TEST_CASE("a level is not given to someone too young to have completed it", "[education]")
{
    const auto gen = tiny_world();
    std::set<education_level> at15;
    std::set<education_level> at19;
    sample(gen, {.country = "AA", .age = 15}, [&](const auto& v) { at15.insert(*v.education); }, 2000);
    sample(gen, {.country = "AA", .age = 19}, [&](const auto& v) { at19.insert(*v.education); }, 2000);
    REQUIRE(at15 == std::set{education_level::less_than_basic, education_level::basic});
    REQUIRE(at19 == std::set{education_level::less_than_basic, education_level::basic, education_level::intermediate});
}

// ---------------------------------------------------------------------------
// Seeds
// ---------------------------------------------------------------------------

TEST_CASE("get_occupation with a seed is deterministic", "[generation][seed]")
{
    const auto gen = make_loaded_generator();
    for (std::uint64_t s = 0; s < 50; ++s)
    {
        REQUIRE(gen.get_occupation({}, s).to_json() == gen.get_occupation({}, s).to_json());
    }
    REQUIRE(gen.get_occupation({.country = "BR"}, std::uint64_t{3}).to_json() ==
            gen.get_occupation({.country = "BR"}, std::uint64_t{3}).to_json());
}

TEST_CASE("a result replays from its seed", "[generation][seed]")
{
    auto gen = make_loaded_generator();
    const dasmig::occupation_request req{.country = "JP", .age = 28, .person_sex = sex::female};
    const auto first = gen.get_occupation(req);
    REQUIRE(gen.get_occupation(req, first.seed()).to_json() == first.to_json());
}

TEST_CASE("caller-owned seeds give deterministic sequences", "[generation][seed]")
{
    auto a = make_loaded_generator();
    const auto b = make_loaded_generator();
    dasmig::core::rng seeds_a{7};
    dasmig::core::rng seeds_b{7};
    const dasmig::occupation_request req{.country = "BR", .age = 35};
    for (int i = 0; i < 20; ++i)
    {
        (void)a.get_occupation({});
        REQUIRE(a.get_occupation(req, seeds_a()).to_json() == b.get_occupation(req, seeds_b()).to_json());
    }
}

TEST_CASE("requests do not become hidden instance defaults", "[generation][request][seed]")
{
    auto a = make_loaded_generator();
    const auto b = make_loaded_generator();
    const auto baseline = b.get_occupation({}, 7).to_json();
    const dasmig::occupation_request req{
        .country = "JP", .age = 28, .person_sex = sex::female,
        .education = education_level::advanced, .status = employment_status::employed};
    for (std::uint64_t s = 0; s < 20; ++s)
    {
        (void)a.get_occupation(req);
        (void)a.get_occupation(req, s);
        REQUIRE(a.get_occupation({}, 7).to_json() == baseline);
        REQUIRE(a.get_occupation({}).age == 40);
        REQUIRE(a.get_occupation(req, s).to_json() == b.get_occupation(req, s).to_json());
    }
}

TEST_CASE("each field has a stream of its own", "[generation][seed]")
{
    const auto gen = tiny_world();
    for (std::uint64_t s = 0; s < 300; ++s)
    {
        const auto plain = gen.get_occupation({.country = "AA", .age = 40}, s);
        // Forcing the education changes nothing drawn before or beside it.
        const auto edu = gen.get_occupation({.country = "AA", .age = 40, .education = education_level::basic}, s);
        REQUIRE(edu.person_sex == plain.person_sex);
        REQUIRE(edu.status == plain.status);
        // Forcing the status leaves the sex and education as they were.
        const auto st = gen.get_occupation({.country = "AA", .age = 40, .status = employment_status::employed}, s);
        REQUIRE(st.person_sex == plain.person_sex);
        REQUIRE(st.education == plain.education);
        // Forcing the sex leaves the status and education drawn from the same values.
        const auto sx = gen.get_occupation({.country = "AA", .age = 40, .person_sex = plain.person_sex}, s);
        REQUIRE(sx.to_json() == plain.to_json());
    }
}

// ---------------------------------------------------------------------------
// The shipped data
// ---------------------------------------------------------------------------

TEST_CASE("the shipped data shows the expected differences between countries", "[data][stats]")
{
    const auto gen = make_loaded_generator();
    const auto farming = [&](std::string_view country) {
        int jobs = 0;
        int farm = 0;
        sample(gen, {.country = country, .age = 40, .person_sex = sex::male, .status = employment_status::employed},
               [&](const auto& v) {
                   ++jobs;
                   farm += v.job->isco.starts_with('6') || v.job->isco == "92" ? 1 : 0;
               },
               4000);
        return static_cast<double>(farm) / jobs;
    };
    REQUIRE(farming("NG") > 0.30);
    REQUIRE(farming("DE") < 0.08);

    const auto employed = [&](std::string_view country, sex who, std::uint8_t age) {
        int n = 0;
        sample(gen, {.country = country, .age = age, .person_sex = who},
               [&](const auto& v) { n += v.status == employment_status::employed ? 1 : 0; }, 4000);
        return static_cast<double>(n) / 4000.0;
    };
    REQUIRE(employed("IN", sex::female, 42) < 0.62);
    REQUIRE(employed("NG", sex::female, 42) > 0.80);
    REQUIRE(employed("JP", sex::male, 70) < 0.40);
}

TEST_CASE("children and the retired follow age in the shipped data", "[data][stats]")
{
    const auto gen = make_loaded_generator();
    sample(gen, {.country = "BR", .age = 12}, [](const auto& v) {
        REQUIRE(v.status == employment_status::student);
        REQUIRE_FALSE(v.job);
    }, 300);
    int retired = 0;
    sample(gen, {.country = "JP", .age = 72},
           [&](const auto& v) { retired += v.status == employment_status::retired ? 1 : 0; }, 2000);
    REQUIRE(retired > 1000);
}

TEST_CASE("nobody is given a job that needs more years than they have", "[data]")
{
    const auto gen = make_loaded_generator();
    const std::set<std::string> adult_only{"surgeon", "general_practitioner", "judge", "army_officer", "airline_pilot",
                                           "lawyer",  "managing_director",    "police_detective"};
    sample(gen, {.country = "US", .age = 19, .status = employment_status::employed}, [&](const auto& v) {
        REQUIRE_FALSE(adult_only.contains(v.job->key));
    }, 5000);
}

TEST_CASE("education sorts people into jobs in the shipped data", "[data][stats]")
{
    const auto gen = make_loaded_generator();
    const auto share_advanced = [&](char major) {
        int n = 0;
        int advanced = 0;
        for (std::uint64_t s = 0; s < 12000; ++s)
        {
            const auto v = gen.get_occupation({.country = "US", .age = 40, .status = employment_status::employed}, s);
            if (v.job->isco.front() != major) continue;
            ++n;
            advanced += v.education == education_level::advanced ? 1 : 0;
        }
        return static_cast<double>(advanced) / n;
    };
    REQUIRE(share_advanced('2') > share_advanced('9') + 0.3);
}

// ---------------------------------------------------------------------------
// Conversion, streaming and JSON
// ---------------------------------------------------------------------------

TEST_CASE("string conversion and streaming give the job or the status", "[conversion]")
{
    const auto gen = tiny_world();
    const auto worker = gen.get_occupation({.country = "AA", .age = 40, .status = employment_status::employed}, 1);
    REQUIRE(worker.job);
    const std::string s = worker;
    REQUIRE(s == worker.job->label);
    std::ostringstream os;
    os << worker;
    REQUIRE(os.str() == s);
    const auto child = gen.get_occupation({.country = "AA", .age = 9}, 1);
    REQUIRE(child.to_string() == "student");
}

TEST_CASE("to_json and signature", "[json]")
{
    const auto gen = tiny_world();
    const auto v = gen.get_occupation({.country = "AA", .age = 40, .person_sex = sex::male,
                                       .education = education_level::advanced, .status = employment_status::employed},
                                      99);
    REQUIRE(v.signature().seed == 99);
    REQUIRE(v.seed() == 99);
    REQUIRE(v.signature().data == gen.data_hash());
    const auto json = v.to_json();
    REQUIRE(json.starts_with(R"({"country_code":"AA","sex":"male","age":40,"status":"employed","education":"advanced","job":{"key":")"));
    REQUIRE(json.find(R"("skills":[)") != std::string::npos);
    REQUIRE(json.find(R"("gear":[)") != std::string::npos);
    REQUIRE(json.find(R"("tags":[)") != std::string::npos);
    REQUIRE(json.find(R"("signature":{"seed":"0000000000000063")") != std::string::npos);

    const auto child = gen.get_occupation({.country = "AA", .age = 9, .person_sex = sex::female}, 5);
    REQUIRE(child.to_json().starts_with(R"({"country_code":"AA","sex":"female","age":9,"status":"student","education":null,"job":null,"signature":)"));

    dasmig::core::json_writer w;
    w.begin_array();
    v.to_json(w);
    w.end_array();
    REQUIRE(w.str() == "[" + json + "]");
}

// ---------------------------------------------------------------------------
// Known answers (rule 2): the same seed and data give these exact results on
// every compiler and platform. A change here is a change of output and needs
// a major version.
// ---------------------------------------------------------------------------

TEST_CASE("known answers for fixed seeds", "[kat]")
{
    dasmig::ocg gen;
    gen.load(std::filesystem::path{"resources/lite"});
    if (!gen.has_data()) gen.load(std::filesystem::path{"../resources/lite"});
    REQUIRE(gen.has_data());
    REQUIRE(gen.data_hash() == 0x1810ef13288a98e8ULL);

    REQUIRE(gen.get_occupation({.country = "US", .age = 30, .person_sex = sex::male}, 1).to_json() == R"({"country_code":"US","sex":"male","age":30,"status":"employed","education":"advanced","job":{"key":"sales_representative","label":"Sales representative","isco":"33","isco_label":"Business and administration associate professionals","unit":"3322","skill":"3-4","skills":["sales","negotiation","driving"],"gear":["business_casual","briefcase","car"],"tags":["social"]},"signature":{"seed":"0000000000000001","data":"1810ef13288a98e8"}})");
    REQUIRE(gen.get_occupation({.country = "JP", .age = 16, .person_sex = sex::female}, 2).to_json() == R"({"country_code":"JP","sex":"female","age":16,"status":"student","education":"basic","job":null,"signature":{"seed":"0000000000000002","data":"1810ef13288a98e8"}})");
    REQUIRE(gen.get_occupation({.country = "BR", .age = 70, .person_sex = sex::female}, 3).to_json() == R"({"country_code":"BR","sex":"female","age":70,"status":"retired","education":"less_than_basic","job":{"key":"crop_farmer","label":"Crop farmer","isco":"61","isco_label":"Market-oriented skilled agricultural workers","unit":"6111","skill":"2","skills":["crop_growing","machinery"],"gear":["work_boots","tractor"],"tags":["outdoor","manual"]},"signature":{"seed":"0000000000000003","data":"1810ef13288a98e8"}})");
    REQUIRE(gen.get_occupation({}, std::uint64_t{4}).to_json() == R"({"country_code":"NC","sex":"male","age":40,"status":"employed","education":"intermediate","job":{"key":"shop_assistant","label":"Shop assistant","isco":"52","isco_label":"Sales workers","unit":"5223","skill":"2","skills":["selling","customer_service"],"gear":["name_tag","cash_register"],"tags":["social"]},"signature":{"seed":"0000000000000004","data":"1810ef13288a98e8"}})");

    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (std::uint64_t s = 0; s < 10000; ++s)
    {
        for (const char c : gen.get_occupation({}, s).to_json())
        {
            h ^= static_cast<unsigned char>(c);
            h *= 0x100000001b3ULL;
        }
    }
    REQUIRE(h == 0x3a22197f702f0315ULL);
}

// ---------------------------------------------------------------------------
// Extending data (rule 8)
// ---------------------------------------------------------------------------

TEST_CASE("extend overrides, adds and removes jobs", "[extend]")
{
    auto gen = tiny_world();
    const auto base_jobs = gen.job_count();
    const auto base_hash = gen.data_hash();

    // Remove the analyst and add a pilot to group 11; the clerk is then the only job in 21.
    const auto& info = gen.extend_from_string(table::occupations,
                                              "key\tlabel\tisco\tweight\tdisabled\n"
                                              "analyst\t\t\t\t1\n"
                                              "pilot\tPilot\t11\t3\t\n",
                                              "mod.tsv");
    REQUIRE(info.added == 1);
    REQUIRE(info.removed == 1);
    REQUIRE(gen.job_count() == base_jobs);
    REQUIRE(gen.data_hash() != base_hash);
    REQUIRE(gen.layers(table::occupations).size() == 2);
    REQUIRE(gen.layers(table::status).size() == 1);

    int pilots = 0;
    int bosses = 0;
    sample(gen, {.country = "AA", .age = 40, .person_sex = sex::female}, [&](const auto& v) {
        REQUIRE(v.signature().data == gen.data_hash());
        if (!v.job) return;
        REQUIRE(v.job->key != "analyst");
        if (v.job->isco == "21") REQUIRE(v.job->key == "clerk");
        pilots += v.job->key == "pilot" ? 1 : 0;
        bosses += v.job->key == "boss" ? 1 : 0;
    });
    REQUIRE(static_cast<double>(pilots) / bosses == Approx(3.0).margin(0.2));

    // Reloading drops every layer.
    gen.extend_from_string(table::occupations, "key\tlabel\tisco\tweight\textra\nzzz\tZzz\t11\t1\tnote\n", "unknown-column.tsv");
    REQUIRE(gen.layers(table::occupations).back().warnings.size() == 1);
}

TEST_CASE("a layer can add a country", "[extend]")
{
    auto gen = tiny_world();
    gen.extend_from_string(table::status,
                           "country_code\tsex\tband\temployed\tunemployed\tinactive\n"
                           "AB\tmale\t15-64\t100\t0\t0\nAB\tfemale\t15-64\t100\t0\t0\n",
                           "ab-status.tsv");
    gen.extend_from_string(table::shares,
                           "country_code\tsex\tcode\tshare\nAB\tmale\t11\t1\nAB\tfemale\t11\t1\n", "ab-shares.tsv");
    REQUIRE(gen.country_count() == 2);
    sample(gen, {.country = "AB", .age = 30}, [](const auto& v) {
        REQUIRE(v.status == employment_status::employed);
        REQUIRE(v.job->isco == "11");
        REQUIRE_FALSE(v.education); // AB has no education rows: unknown
    }, 200);
}

TEST_CASE("rows that name an unknown ISCO group are listed, not fatal", "[extend][errors]")
{
    auto gen = tiny_world();
    gen.extend_from_string(table::occupations, "key\tlabel\tisco\tweight\nghost\tGhost\t99\t5\n", "jobs.tsv");
    gen.extend_from_string(table::shares, "country_code\tsex\tcode\tshare\nAA\tmale\t99\t50\n", "shares.tsv");
    REQUIRE(gen.unresolved().size() == 2);
    REQUIRE(gen.unresolved().front().find("unknown ISCO group '99'") != std::string::npos);
    sample(gen, {.country = "AA", .age = 40}, [](const auto& v) {
        if (v.job) REQUIRE(v.job->key != "ghost");
    }, 2000);
}

TEST_CASE("layer order matters: later layers win", "[extend]")
{
    auto gen = tiny_world();
    gen.extend_from_string(table::shares, "country_code\tsex\tcode\tshare\nAA\tmale\t21\t0\n", "first.tsv");
    sample(gen, {.country = "AA", .age = 40, .person_sex = sex::male}, [](const auto& v) {
        if (v.job) REQUIRE(v.job->isco == "11");
    }, 1000);
    gen.extend_from_string(table::shares, "country_code\tsex\tcode\tshare\nAA\tmale\t21\t70\n", "second.tsv");
    int group21 = 0;
    sample(gen, {.country = "AA", .age = 40, .person_sex = sex::male}, [&](const auto& v) {
        group21 += v.job && v.job->isco == "21" ? 1 : 0;
    }, 1000);
    REQUIRE(group21 > 300);
}

TEST_CASE("a bad layer is rejected and leaves the data unchanged", "[extend][errors]")
{
    auto gen = make_loaded_generator();
    const auto hash = gen.data_hash();
    const auto count = gen.country_count();
    const auto status_header = std::string{"country_code\tsex\tband\temployed\tunemployed\tinactive\n"};
    const auto bad = [&](table t, const std::string& text) {
        REQUIRE_THROWS_AS(gen.extend_from_string(t, text, "bad.tsv"), dasmig::core::tsv_error);
    };

    REQUIRE_THROWS_WITH(gen.extend_from_string(table::status, status_header + "XX\tmale\t15-64\tmany\t0\t0\n", "bad.tsv"),
                        "bad.tsv:2: column 'employed': expected a number, got 'many'");
    bad(table::status, status_header + "XX\tmale\t15-64\t-1\t0\t0\n");   // negative
    bad(table::status, status_header + "XX\tmale\t15-64\t0\t0\t0\n");    // all zero
    bad(table::status, status_header + "XX\tboth\t15-64\t1\t0\t0\n");    // not a sex
    bad(table::status, status_header + "xx\tmale\t15-64\t1\t0\t0\n");    // not two capital letters
    bad(table::status, status_header + "XX\tmale\tyoung\t1\t0\t0\n");    // not a band
    bad(table::status, status_header + "XX\tmale\t30-20\t1\t0\t0\n");    // band reversed
    bad(table::status, status_header + "XX\tmale\t15-\t1\t0\t0\n");      // band incomplete
    bad(table::status, "country_code\tsex\tband\temployed\nXX\tmale\t15-64\t1\n"); // columns missing
    bad(table::education, "country_code\tsex\tband\tless_than_basic\tbasic\tintermediate\tadvanced\nXX\tmale\t15-64\t0\t0\t0\t0\n");
    bad(table::inactivity, "band\tstudent\tretired\tother\n15-64\t0\t0\t0\n");
    bad(table::shares, "country_code\tsex\tcode\tshare\nXX\tmale\t11\t-5\n");
    bad(table::factors, "country_code\tmajor\tdimension\tvalue\tfactor\nXX\t1\tmood\thappy\t2\n");
    bad(table::factors, "country_code\tmajor\tdimension\tvalue\tfactor\nXX\t1\teducation\tphd\t2\n");
    bad(table::factors, "country_code\tmajor\tdimension\tvalue\tfactor\nXX\t1\tage_band\told\t2\n");
    bad(table::factors, "country_code\tmajor\tdimension\tvalue\tfactor\nXX\t1\tage_band\t15-24\t-2\n");
    bad(table::occupations, "key\tlabel\tisco\tweight\tmin_age\tmax_age\nx\tX\t11\t1\t30\t20\n");
    bad(table::occupations, "key\tlabel\tisco\tweight\nx\tX\t11\t-1\n");
    REQUIRE_THROWS_AS(gen.extend_from_string(table::occupations, "key\tlabel\tisco\tweight\nplumber\tP\t71\t1\n", "clash.tsv",
                                             dasmig::core::layer_mode::add),
                      dasmig::core::tsv_error); // plumber is already loaded

    REQUIRE(gen.data_hash() == hash);
    REQUIRE(gen.country_count() == count);
    for (const auto t : {table::status, table::inactivity, table::education, table::isco, table::shares,
                         table::factors, table::occupations})
    {
        REQUIRE(gen.layers(t).size() == 1);
    }
}

TEST_CASE("a directory layer is applied to all tables or none", "[extend][errors]")
{
    const auto dir = std::filesystem::temp_directory_path() / "ocg_layer_dir";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto write = [&](const char* name, const char* text) { std::ofstream{dir / name, std::ios::binary} << text; };
    write("occupations.tsv", "key\tlabel\tisco\tweight\nbounty_hunter\tBounty hunter\t54\t5\n");
    write("status.tsv", "country_code\tsex\tband\temployed\tunemployed\tinactive\nXX\tmale\tyoung\t1\t0\t0\n"); // bad

    auto gen = make_loaded_generator();
    const auto hash = gen.data_hash();
    const auto jobs = gen.job_count();
    REQUIRE_THROWS_AS(gen.extend(dir), dasmig::core::tsv_error);
    REQUIRE(gen.data_hash() == hash); // the valid occupations file was not applied either
    REQUIRE(gen.job_count() == jobs);

    write("status.tsv", "country_code\tsex\tband\temployed\tunemployed\tinactive\nXX\tmale\t15-64\t1\t0\t0\nXX\tfemale\t15-64\t1\t0\t0\n"); // fixed
    gen.extend(dir);
    REQUIRE(gen.job_count() == jobs + 1);
    REQUIRE(gen.layers(table::occupations).size() == 2);
    REQUIRE(gen.layers(table::status).size() == 2);
    REQUIRE(gen.layers(table::shares).size() == 1);

    REQUIRE_THROWS_AS(gen.extend(dir / "other.tsv"), std::invalid_argument); // not a table name

    // A single file is applied as one table.
    gen.extend(dir / "occupations.tsv");
    REQUIRE(gen.layers(table::occupations).size() == 3);
    std::filesystem::remove_all(dir);
}

#ifdef DASMIG_OCCUPATION_EMBED
TEST_CASE("compiled-in data needs no files and matches them", "[embed]")
{
    const auto old_cwd = std::filesystem::current_path();
    const auto tmp = std::filesystem::temp_directory_path() / "ocg_embed_dir";
    std::filesystem::create_directories(tmp);
    std::filesystem::current_path(tmp);
    dasmig::ocg gen;
    const bool loaded = gen.load(dasmig::dataset::lite);
    std::filesystem::current_path(old_cwd);
    std::filesystem::remove_all(tmp);

    REQUIRE(loaded);
    REQUIRE(gen.layers(table::status).front().source == "embedded:lite/status.tsv");
    REQUIRE(gen.data_hash() == 0x1810ef13288a98e8ULL); // same as the file
    REQUIRE(gen.get_occupation({.country = "US", .age = 30, .person_sex = sex::male}, 1).to_json() ==
            R"({"country_code":"US","sex":"male","age":30,"status":"employed","education":"advanced","job":{"key":"sales_representative","label":"Sales representative","isco":"33","isco_label":"Business and administration associate professionals","unit":"3322","skill":"3-4","skills":["sales","negotiation","driving"],"gear":["business_casual","briefcase","car"],"tags":["social"]},"signature":{"seed":"0000000000000001","data":"1810ef13288a98e8"}})");
}
#endif

// ---------------------------------------------------------------------------
// Batch generation
// ---------------------------------------------------------------------------

TEST_CASE("occupation batch: items equal the single calls for any thread count", "[batch]")
{
    dasmig::ocg gen;
    gen.load(std::filesystem::path{"resources/lite"});
    if (!gen.has_data()) gen.load(std::filesystem::path{"../resources/lite"});
    REQUIRE(gen.has_data());
    const auto request = dasmig::occupation_request{};
    const auto batch = gen.get_occupation_batch(request, {.count = 200, .threads = 1}, 77);
    REQUIRE(batch.size() == 200);
    for (std::uint64_t i = 0; i < batch.size(); ++i)
    {
        REQUIRE(batch[i].to_json() == gen.get_occupation(request, dasmig::core::item_seed(77, i)).to_json());
    }

    // A range is computed directly, on any number of threads.
    const auto tail = gen.get_occupation_batch(request, {.count = 100, .first = 100, .threads = 4}, 77);
    REQUIRE(tail.size() == 100);
    for (std::size_t i = 0; i < tail.size(); ++i)
    {
        REQUIRE(tail[i].to_json() == batch[100 + i].to_json());
    }
    for (const std::size_t threads : {2U, 7U, 0U})
    {
        const auto other = gen.get_occupation_batch(request, {.count = 200, .threads = threads}, 77);
        REQUIRE(other.size() == batch.size());
        for (std::size_t i = 0; i < other.size(); ++i)
        {
            REQUIRE(other[i].to_json() == batch[i].to_json());
        }
    }

    // Without a seed the batch still has the requested size.
    REQUIRE(gen.get_occupation_batch(request, {.count = 5}).size() == 5);
}

TEST_CASE("occupation batch: streaming gives the same items in order", "[batch]")
{
    dasmig::ocg gen;
    gen.load(std::filesystem::path{"resources/lite"});
    if (!gen.has_data()) gen.load(std::filesystem::path{"../resources/lite"});
    REQUIRE(gen.has_data());
    const auto request = dasmig::occupation_request{};
    const auto batch = gen.get_occupation_batch(request, {.count = 300, .first = 10, .threads = 3}, 5);
    std::uint64_t next = 10;
    std::size_t seen = 0;
    gen.for_each_occupation(request, {.count = 300, .first = 10, .threads = 3}, 5,
                        [&](std::uint64_t index, const dasmig::occupation& item)
                        {
                            REQUIRE(index == next);
                            REQUIRE(item.to_json() == batch[seen].to_json());
                            ++next;
                            ++seen;
                        });
    REQUIRE(seen == batch.size());
}

TEST_CASE("occupation batch: known answer of 10,000 items", "[batch][kat]")
{
    dasmig::ocg gen;
    gen.load(std::filesystem::path{"resources/lite"});
    if (!gen.has_data()) gen.load(std::filesystem::path{"../resources/lite"});
    REQUIRE(gen.has_data());
    const auto request = dasmig::occupation_request{};
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    gen.for_each_occupation(request, {.count = 10000, .threads = 4}, 1,
                        [&](std::uint64_t, const dasmig::occupation& item)
                        { hash = dasmig::core::hash_combine(hash, dasmig::core::fnv1a64(item.to_json())); });
    REQUIRE(hash == 0x3f6cf73364859529ULL);
}
