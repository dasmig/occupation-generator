// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Dasso Migotto

#ifndef DASMIG_OCCUPATIONGEN_HPP
#define DASMIG_OCCUPATIONGEN_HPP

#include "core.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef DASMIG_OCCUPATION_EMBED
#include "occupationgen_data.hpp"
#endif

/// @file occupationgen.hpp
/// @brief Occupation Generator library — employment status, education and occupation by country, age and sex for C++23.
/// @author Diego Dasso Migotto (diegomigotto at hotmail dot com)
/// @see See doc/usage.md for the narrative tutorial.

DASMIG_CORE_REQUIRE(1, 0);

namespace dasmig
{

#ifndef DASMIG_SEX_DEFINED
#define DASMIG_SEX_DEFINED
/// @brief Biological sex.
enum class sex : std::uint8_t
{
    male,  ///< Male.
    female ///< Female.
};
#endif

/// @brief What a person does with their working-age days.
enum class employment_status : std::uint8_t
{
    employed,   ///< In employment.
    unemployed, ///< Without work and looking for it.
    student,    ///< Studying (always, from 6 to 14; among the inactive, by age).
    retired,    ///< Retired (among the inactive, by age).
    inactive    ///< Outside the labour force for another reason: care, illness, discouragement; under 6, always.
};

/// @brief Highest education completed, in the ILO's four aggregate levels.
enum class education_level : std::uint8_t
{
    less_than_basic, ///< Less than basic: not completed primary education, or none.
    basic,           ///< Basic: primary or lower secondary (ISCED 1-2).
    intermediate,    ///< Intermediate: upper secondary and post-secondary non-tertiary (ISCED 3-4).
    advanced         ///< Advanced: tertiary (ISCED 5-8).
};

/// @brief The name of an employment status, as used in the data and in JSON.
/// @param s The status.
/// @return "employed", "unemployed", "student", "retired" or "inactive".
[[nodiscard]] constexpr std::string_view to_string(employment_status s) noexcept
{
    switch (s)
    {
    case employment_status::employed: return "employed";
    case employment_status::unemployed: return "unemployed";
    case employment_status::student: return "student";
    case employment_status::retired: return "retired";
    case employment_status::inactive: return "inactive";
    }
    return "";
}

/// @brief The name of an education level, as used in the data and in JSON.
/// @param e The level.
/// @return "less_than_basic", "basic", "intermediate" or "advanced".
[[nodiscard]] constexpr std::string_view to_string(education_level e) noexcept
{
    switch (e)
    {
    case education_level::less_than_basic: return "less_than_basic";
    case education_level::basic: return "basic";
    case education_level::intermediate: return "intermediate";
    case education_level::advanced: return "advanced";
    }
    return "";
}

/// @brief A job, as drawn.
struct job_info
{
    std::string key;                 ///< Catalogue key (the `key` column of `occupations.tsv`).
    std::string label;               ///< Display label, for example "Plumber".
    std::string isco;                ///< ISCO-08 level-2 group code, for example "71".
    std::string isco_label;          ///< Title of that group.
    std::string unit;                ///< ISCO-08 unit group (four digits); may be empty.
    std::string skill;               ///< ISCO-08 skill level of the group ("1", "2", "3-4"); may be empty.
    std::vector<std::string> skills; ///< Skill keywords.
    std::vector<std::string> gear;   ///< Clothing and gear keywords.
    std::vector<std::string> tags;   ///< Character tags such as "manual" or "caring", for other generators.
};

/// @brief Per-call generation choices. Every field may be omitted.
// NOLINTBEGIN(readability-redundant-member-init)
struct occupation_request
{
    std::string_view country{};                 ///< ISO 3166-1 alpha-2 code; empty draws a country (uniform over those loaded).
    std::uint8_t age{40};                       ///< Age in completed years (default 40, an age at which most people work).
    std::optional<sex> person_sex{};            ///< Sex; unknown draws one (even odds).
    std::optional<education_level> education{}; ///< Education; unknown draws one from the country, sex and age.
    std::optional<employment_status> status{};  ///< Status; unknown draws one from the country, sex and age.
};
// NOLINTEND(readability-redundant-member-init)

/// @brief Return type for occupation generation.
///
/// Supports implicit conversion to std::string (the job label, or the status
/// when there is no job), streaming via operator<< and JSON output via
/// to_json().
class occupation
{
  public:
    std::string country_code;                 ///< ISO 3166-1 alpha-2 code of the country drawn from.
    sex person_sex{sex::male};                ///< Sex (given or drawn).
    std::uint8_t age{0};                      ///< Age in completed years.
    employment_status status{employment_status::employed}; ///< Employment status.
    std::optional<education_level> education; ///< Education; unknown under 15 when not given.
    std::optional<job_info> job;              ///< The job: the current one when employed, the last or usual one
                                              ///< when unemployed, retired or inactive; none for students and
                                              ///< for the inactive under 6.

    /// @brief Retrieve the random seed used to generate this occupation.
    /// @return The call seed; pass it, with the same request, to the seeded
    ///         get_occupation(request, seed) overload to generate the same occupation again.
    [[nodiscard]] std::uint64_t seed() const { return _signature.seed; }

    /// @brief The seed and the data this occupation was generated from.
    /// @return The replay signature; `data` is ocg::data_hash() at the time
    ///         of generation.
    [[nodiscard]] const core::seed_signature& signature() const { return _signature; }

    /// @brief Human-readable summary string.
    /// @return The job label, or the status when there is no job.
    [[nodiscard]] std::string to_string() const
    {
        return job ? job->label : std::string{dasmig::to_string(status)};
    }

    /// @brief Write this occupation as a JSON object.
    /// @param w Writer positioned where a value is allowed.
    void to_json(core::json_writer& w) const
    {
        w.begin_object()
            .field("country", country_code)
            .field("sex", person_sex == sex::male ? "male" : "female")
            .field("age", static_cast<unsigned>(age))
            .field("status", dasmig::to_string(status));
        w.key("education");
        if (education) w.value(dasmig::to_string(*education));
        else w.value(nullptr);
        w.key("job");
        if (job)
        {
            w.begin_object()
                .field("key", job->key)
                .field("label", job->label)
                .field("isco", job->isco)
                .field("isco_label", job->isco_label)
                .field("unit", job->unit)
                .field("skill", job->skill);
            write_list_(w, "skills", job->skills);
            write_list_(w, "gear", job->gear);
            write_list_(w, "tags", job->tags);
            w.end_object();
        }
        else
        {
            w.value(nullptr);
        }
        w.key("signature");
        _signature.to_json(w);
        w.end_object();
    }

    /// @brief This occupation as a JSON document.
    /// @return Compact JSON, for example
    ///   `{"country":"BR","sex":"male","age":30,"status":"employed","education":"intermediate","job":{...},"signature":{...}}`.
    [[nodiscard]] std::string to_json() const
    {
        core::json_writer w;
        to_json(w);
        return w.str();
    }

    /// @brief Implicit conversion to std::string.
    /// @return Summary string.
    operator std::string() const // NOLINT(hicpp-explicit-conversions)
    {
        return to_string();
    }

    /// @brief Stream the summary to an output stream.
    /// @param os Output stream.
    /// @param v The occupation to stream.
    /// @return The stream.
    friend std::ostream& operator<<(std::ostream& os, const occupation& v)
    {
        os << v.to_string();
        return os;
    }

  private:
    static void write_list_(core::json_writer& w, std::string_view name, const std::vector<std::string>& items)
    {
        w.key(name).begin_array();
        for (const auto& i : items) w.value(i);
        w.end_array();
    }

    core::seed_signature _signature;
    friend class ocg;
};

/// @brief Occupation Generator: draws a person's employment status,
///        education and job.
///
/// The country (when not given) comes from the `"country"` stream of the
/// call seed, the sex from `"sex"`, the status from `"status"`, the education
/// from `"education"`, the ISCO-08 group from `"isco"` and the job within it
/// from `"job"`, so fields added later leave these unchanged. Output is
/// identical on every compiler and platform (dasmig-core).
///
/// The status comes from the country's employment and unemployment rates for
/// the person's sex and age band; the inactive are split into students,
/// retired and others by age. From 6 to 14 a person is a student, and under 6
/// inactive with no job. The job is an
/// ISCO-08 level-2 group drawn by the country's employment share for the
/// person's sex, multiplied by factors for the person's age class and
/// education (how much more or less common the group is at that age or
/// education than in employment as a whole), then a job of the catalogue
/// within the group. Groups with no job open to the person's age are skipped.
///
/// The education comes from the country's shares for the sex and age band. A
/// level the person is too young to have completed (basic under 14,
/// intermediate under 17, advanced under 21) counts as the level below.
///
/// A country chosen at random is uniform over the countries loaded; the data
/// has no population column. Pass a country chosen by country-generator for a
/// population-weighted world.
///
/// Every call takes an occupation_request; generation choices are never
/// stored as instance defaults. Data loading and layers belong to the instance.
/// Can be used as a singleton via instance() or constructed independently.
///
/// @par Thread safety
/// Each instance is independent. Concurrent calls to get_occupation(request) on the
/// **same** instance require external synchronisation.
class ocg
{
  public:
    /// @brief The data tables.
    enum class table : std::uint8_t
    {
        status,      ///< `status.tsv`: key `country_code`, `sex`, `band`; employed, unemployed, inactive (percent), basis, year.
        inactivity,  ///< `inactivity.tsv`: key `band`; student, retired, other (shares of the inactive).
        education,   ///< `education.tsv`: key `country_code`, `sex`, `band`; less_than_basic, basic, intermediate, advanced, basis, year.
        isco,        ///< `isco.tsv`: key `code` (ISCO-08 level 2); label, major, skill.
        shares,      ///< `shares.tsv`: key `country_code`, `sex`, `code`; share (percent of employment), basis, year.
        factors,     ///< `factors.tsv`: key `country_code`, `major`, `dimension`, `value`; factor, basis, year.
        occupations  ///< `occupations.tsv`: key `key`; label, isco, weight, unit, min_age, max_age, tags, skills, gear.
    };

    /// @brief Default constructor — creates an empty generator with no
    ///        data.
    ocg() : _tables{make_tables_()} {}

    ocg(const ocg&) = delete;            ///< Not copyable.
    ocg& operator=(const ocg&) = delete; ///< Not copyable.
    ocg(ocg&&) noexcept = default;       ///< Movable.
    ocg& operator=(ocg&&) noexcept = default; ///< Movable.
    ~ocg() = default;                    ///< Destructor.

    /// @brief Access the global singleton instance.
    ///
    /// Loads the `lite` tier (or `full` if `lite` is missing) on first
    /// access, from compiled-in data or an auto-probed `resources/`.
    /// @return The singleton.
    static ocg& instance()
    {
        static ocg inst{auto_probe_tag{}};
        return inst;
    }

    // -- Generation -------------------------------------------------------

    /// @brief Generate a random occupation for a request.
    /// @param request Country, age, sex, education and status; any may be omitted.
    ///        Pass `{}` to use the request defaults.
    /// @details Draws a call seed from the instance's entropy-seeded engine.
    /// @return The occupation.
    /// @throws std::runtime_error If no data has been loaded.
    /// @throws std::invalid_argument If the country has no data for the
    ///         person (or, with no country, no country has any), or no
    ///         occupation is open to the person.
    [[nodiscard]] occupation get_occupation(const occupation_request& request)
    {
        return generate_(request, draw_seed_());
    }

    /// @brief Generate a deterministic occupation for a request.
    /// @param request Country, age, sex, education and status; any may be omitted.
    /// @param call_seed Seed; the same seed, request and data give the same
    ///        occupation.
    /// @return The occupation.
    /// @throws std::runtime_error If no data has been loaded.
    /// @throws std::invalid_argument As for the unseeded overload.
    [[nodiscard]] occupation get_occupation(const occupation_request& request, std::uint64_t call_seed) const
    {
        return generate_(request, call_seed);
    }

    // -- Queries ----------------------------------------------------------

    /// @brief Check whether status data has been loaded.
    /// @return True when at least one status row is loaded.
    [[nodiscard]] bool has_data() const { return !_tables.at(index_(table::status)).empty(); }

    /// @brief Return the number of countries that can be drawn from.
    /// @return The count: countries with status rows for both sexes.
    [[nodiscard]] std::size_t country_count() const { return _model.codes.size(); }

    /// @brief Return the number of jobs in the catalogue.
    /// @return The count.
    [[nodiscard]] std::size_t job_count() const { return _model.jobs.size(); }

    /// @brief Rows that name something that is not loaded: a share for an
    ///        unknown ISCO group, a job for an unknown ISCO group. They are
    ///        ignored, so removing a group needs no other change; this lists
    ///        them to catch misspelt codes.
    /// @return One description per ignored row, such as
    ///         `occupations.tsv:12: unknown ISCO group '7x'`.
    [[nodiscard]] const std::vector<std::string>& unresolved() const { return _model.unresolved; }

    // -- Data management --------------------------------------------------

    /// @brief Load a resource directory, replacing all data and layers
    ///        loaded before.
    ///
    /// The directory must contain `status.tsv`; the other tables are read
    /// when present (a missing one leaves its table empty). A path without
    /// `status.tsv` is ignored.
    ///
    /// @param dir Directory such as `resources/lite`.
    /// @throws core::tsv_error If a file is malformed; the data is then
    ///         unchanged.
    void load(const std::filesystem::path& dir)
    {
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec) ||
            !std::filesystem::is_regular_file(dir / file_name_(table::status), ec))
        {
            return;
        }
        apply_(read_directory_(dir), core::layer_mode::replace, true);
    }

    /// @brief Load a dataset tier, replacing all data and layers loaded
    ///        before.
    ///
    /// Uses the compiled-in copy when the library is built with
    /// `DASMIG_OCCUPATION_EMBED`, otherwise searches `resources/`,
    /// `../resources`, `occupation-generator/resources` and
    /// `../occupation-generator/resources`.
    ///
    /// @param tier The tier.
    /// @return False when the tier was not found.
    /// @throws core::tsv_error If a file is malformed.
    [[nodiscard]] bool load(dataset tier)
    {
        const std::string tier_dir{core::tier_name(tier)};
        const auto marker = tier_dir + "/" + std::string{file_name_(table::status)};
        const auto root = core::find_resources(library_name_, marker);
        documents docs;
        for (const auto t : all_tables_)
        {
            docs.at(index_(t)) = core::load_resource(tier_dir + "/" + std::string{file_name_(t)},
                                                     embedded_files_(), root);
        }
        if (!docs.at(index_(table::status))) return false;
        apply_(std::move(docs), core::layer_mode::replace, true);
        return true;
    }

    /// @brief Add data layers from a directory or a file.
    ///
    /// A directory may hold any of the table files (`status.tsv`,
    /// `inactivity.tsv`, `education.tsv`, `isco.tsv`, `shares.tsv`,
    /// `factors.tsv`, `occupations.tsv`); all are applied together or, if any
    /// fails, none. A file must have one of those names.
    ///
    /// @param path Directory or file.
    /// @param mode How the layers combine with the data loaded before.
    /// @throws core::tsv_error If a file cannot be read or a row is
    ///         invalid; the data is then unchanged.
    /// @throws std::invalid_argument If @p path is a file with another name.
    void extend(const std::filesystem::path& path, core::layer_mode mode = core::layer_mode::override)
    {
        std::error_code ec;
        if (std::filesystem::is_directory(path, ec))
        {
            apply_(read_directory_(path), mode, false);
            return;
        }
        for (const auto t : all_tables_)
        {
            if (path.filename() == file_name_(t))
            {
                (void)extend(t, core::tsv_document::load(path), mode);
                return;
            }
        }
        throw std::invalid_argument("extend: expected status.tsv, inactivity.tsv, education.tsv, isco.tsv, "
                                    "shares.tsv, factors.tsv or occupations.tsv, got " + path.filename().string());
    }

    /// @brief Add a data layer to one table.
    ///
    /// Rows are keyed by the table's key columns (see table). In the default
    /// override mode a row replaces the loaded row with the same key and new
    /// keys are added; a row with `disabled` set to 1 removes its key.
    ///
    /// @param which The table.
    /// @param doc TSV with that table's columns.
    /// @param mode How the layer combines with the data loaded before.
    /// @return What the layer did (rows added, replaced, removed, warnings).
    /// @throws core::tsv_error If a row is invalid; the data is unchanged.
    const core::layer_info& extend(table which, core::tsv_document doc,
                                   core::layer_mode mode = core::layer_mode::override)
    {
        documents docs;
        docs.at(index_(which)).emplace(std::move(doc));
        apply_(std::move(docs), mode, false);
        return _tables.at(index_(which)).layers().back();
    }

    /// @brief Add a data layer to one table from TSV text (for example
    ///        from a WebAssembly host).
    /// @param which The table.
    /// @param text TSV with that table's columns.
    /// @param source Name used in errors.
    /// @param mode How the layer combines with the data loaded before.
    /// @return What the layer did.
    /// @throws core::tsv_error If a row is invalid; the data is unchanged.
    const core::layer_info& extend_from_string(table which, std::string_view text, std::string source,
                                               core::layer_mode mode = core::layer_mode::override)
    {
        return extend(which, core::tsv_document::parse(text, std::move(source)), mode);
    }

    /// @brief The layers in effect for one table, in order.
    /// @param which The table.
    /// @return The layer records.
    [[nodiscard]] const std::vector<core::layer_info>& layers(table which) const
    {
        return _tables.at(index_(which)).layers();
    }

    /// @brief Hash of the loaded data (all tables), recorded in each
    ///        result's signature().
    /// @return The hash; 0 when nothing is loaded.
    [[nodiscard]] std::uint64_t data_hash() const
    {
        std::uint64_t h = 0;
        bool any = false;
        for (const auto& t : _tables)
        {
            any = any || !t.layers().empty();
            h = core::hash_combine(h, t.data_hash());
        }
        return any ? h : 0;
    }

  private:
    // -- Tables -------------------------------------------------------------

    static constexpr std::string_view library_name_ = "occupation-generator";
    static constexpr std::size_t table_count_ = 7;
    static constexpr std::array<table, table_count_> all_tables_{
        table::status, table::inactivity, table::education, table::isco,
        table::shares, table::factors,    table::occupations};

    using documents = std::array<std::optional<core::tsv_document>, table_count_>;
    using tables = std::array<core::layered_table, table_count_>;

    static constexpr std::size_t index_(table t) { return static_cast<std::size_t>(t); }

    static constexpr std::string_view file_name_(table t)
    {
        switch (t)
        {
        case table::status: return "status.tsv";
        case table::inactivity: return "inactivity.tsv";
        case table::education: return "education.tsv";
        case table::isco: return "isco.tsv";
        case table::shares: return "shares.tsv";
        case table::factors: return "factors.tsv";
        case table::occupations: return "occupations.tsv";
        }
        return "";
    }

    static core::table_schema schema_(table t)
    {
        switch (t)
        {
        case table::status:
            return {.key = {"country_code", "sex", "band"},
                    .required = {"employed", "unemployed", "inactive"},
                    .optional = {"basis", "year"}};
        case table::inactivity:
            return {.key = {"band"}, .required = {"student", "retired", "other"}, .optional = {}};
        case table::education:
            return {.key = {"country_code", "sex", "band"},
                    .required = {"less_than_basic", "basic", "intermediate", "advanced"},
                    .optional = {"basis", "year"}};
        case table::isco: return {.key = {"code"}, .required = {"label", "major"}, .optional = {"skill"}};
        case table::shares:
            return {.key = {"country_code", "sex", "code"}, .required = {"share"}, .optional = {"basis", "year"}};
        case table::factors:
            return {.key = {"country_code", "major", "dimension", "value"},
                    .required = {"factor"},
                    .optional = {"basis", "year"}};
        case table::occupations:
            return {.key = {"key"},
                    .required = {"label", "isco", "weight"},
                    .optional = {"unit", "min_age", "max_age", "tags", "skills", "gear"}};
        }
        return {};
    }

    static tables make_tables_()
    {
        return {core::layered_table{schema_(table::status)},     core::layered_table{schema_(table::inactivity)},
                core::layered_table{schema_(table::education)},  core::layered_table{schema_(table::isco)},
                core::layered_table{schema_(table::shares)},     core::layered_table{schema_(table::factors)},
                core::layered_table{schema_(table::occupations)}};
    }

    // -- Parsed rows --------------------------------------------------------

    static constexpr int working_age_ = 15; // under this age a person is a student
    static constexpr int school_age_ = 6;   // under this age a person is inactive, with no job
    static constexpr std::array<education_level, 4> education_levels_{
        education_level::less_than_basic, education_level::basic, education_level::intermediate,
        education_level::advanced};
    // Youngest age at which a level can have been completed (a younger person has the level below).
    static constexpr std::array<int, 4> education_min_age_{0, 14, 17, 21};

    struct band
    {
        int min_age{0};
        int max_age{0};
        [[nodiscard]] bool contains(int age) const { return age >= min_age && age <= max_age; }
    };

    struct status_row
    {
        band ages;
        std::array<double, 3> weights{}; // employed, unemployed, inactive
    };

    struct education_row
    {
        band ages;
        std::array<double, 4> weights{};
    };

    struct inactivity_row
    {
        band ages;
        std::array<double, 3> weights{}; // student, retired, other
    };

    struct isco_data
    {
        std::string code;
        std::string label;
        std::string major;
        std::string skill;
    };

    struct job_data
    {
        std::string key;
        std::string label;
        std::string unit;
        double weight{0.0};
        int min_age{0};
        int max_age{255};
        std::vector<std::string> skills;
        std::vector<std::string> gear;
        std::vector<std::string> tags;
        std::size_t isco_index{0};
    };

    struct factor_set
    {
        std::vector<std::pair<band, double>> age;
        std::array<double, 4> education{1.0, 1.0, 1.0, 1.0};
    };

    struct country_data
    {
        std::array<std::vector<status_row>, 2> status;
        std::array<std::vector<education_row>, 2> education;
        std::array<core::weighted_table<std::size_t>, 2> shares; // ISCO index, by share
    };

    struct model
    {
        std::map<std::string, country_data, std::less<>> countries;
        std::vector<std::string> codes; // countries with status rows for both sexes, sorted
        std::vector<inactivity_row> inactivity;
        std::vector<isco_data> isco;
        std::vector<job_data> jobs;
        std::vector<std::vector<std::size_t>> jobs_of; // per ISCO index
        std::unordered_map<std::string, factor_set> factors; // country, major
        std::vector<std::string> unresolved;
    };

    static std::string factor_key_(std::string_view country, std::string_view major)
    {
        std::string k;
        k.reserve(country.size() + major.size() + 1);
        k.append(country).push_back('\x1f');
        k.append(major);
        return k;
    }

    static std::size_t sex_index_(const core::tsv_row& row)
    {
        const auto s = row["sex"];
        if (s == "male") return 0;
        if (s == "female") return 1;
        throw row.error("sex", std::format("expected male or female, got '{}'", s));
    }

    // A whole number of at most three digits.
    static std::optional<int> parse_age_(std::string_view text)
    {
        if (text.empty() || text.size() > 3) return std::nullopt;
        int value = 0;
        for (const char c : text)
        {
            if (c < '0' || c > '9') return std::nullopt;
            value = value * 10 + (c - '0');
        }
        return value;
    }

    // "15-24" or "65+" (no upper limit).
    static band parse_band_(const core::tsv_row& row, std::string_view column)
    {
        const auto text = row.get<std::string>(column);
        const std::string_view view{text};
        std::optional<band> parsed;
        if (view.ends_with('+'))
        {
            if (const auto lo = parse_age_(view.substr(0, view.size() - 1))) parsed = band{.min_age = *lo, .max_age = 255};
        }
        else if (const auto dash = view.find('-'); dash != std::string_view::npos)
        {
            const auto lo = parse_age_(view.substr(0, dash));
            const auto hi = parse_age_(view.substr(dash + 1));
            if (lo && hi && *hi >= *lo) parsed = band{.min_age = *lo, .max_age = *hi};
        }
        if (!parsed)
        {
            throw row.error(column, std::format("expected an age band such as '15-24' or '65+', got '{}'", text));
        }
        return *parsed;
    }

    static double non_negative_(const core::tsv_row& row, std::string_view column)
    {
        const auto v = row.get<double>(column);
        if (!(v >= 0.0)) throw row.error(column, "must not be negative");
        return v;
    }

    static std::vector<std::string> split_list_(std::string_view text)
    {
        std::vector<std::string> out;
        while (!text.empty())
        {
            const auto cut = text.find(';');
            const auto item = text.substr(0, cut);
            if (!item.empty()) out.emplace_back(item);
            if (cut == std::string_view::npos) break;
            text.remove_prefix(cut + 1);
        }
        return out;
    }

    static std::string check_country_(const core::tsv_row& row)
    {
        auto cca2 = row.get<std::string>("country_code");
        const bool ok = cca2.size() == 2 && cca2[0] >= 'A' && cca2[0] <= 'Z' && cca2[1] >= 'A' && cca2[1] <= 'Z';
        if (!ok) throw row.error("country_code", "expected two capital letters (ISO 3166-1 alpha-2)");
        return cca2;
    }

    // Each parse_ function checks a row and throws core::tsv_error located at
    // it; the same function validates a layer and builds the model.
    static std::tuple<std::string, std::size_t, status_row> parse_status_(const core::tsv_row& row)
    {
        auto cca2 = check_country_(row);
        status_row s;
        s.ages = parse_band_(row, "band");
        s.weights = {non_negative_(row, "employed"), non_negative_(row, "unemployed"), non_negative_(row, "inactive")};
        if (!(s.weights[0] + s.weights[1] + s.weights[2] > 0.0)) throw row.error("employed", "the three shares are all 0");
        return {std::move(cca2), sex_index_(row), s};
    }

    static inactivity_row parse_inactivity_(const core::tsv_row& row)
    {
        inactivity_row r;
        r.ages = parse_band_(row, "band");
        r.weights = {non_negative_(row, "student"), non_negative_(row, "retired"), non_negative_(row, "other")};
        if (!(r.weights[0] + r.weights[1] + r.weights[2] > 0.0)) throw row.error("student", "the three shares are all 0");
        return r;
    }

    static std::tuple<std::string, std::size_t, education_row> parse_education_(const core::tsv_row& row)
    {
        auto cca2 = check_country_(row);
        education_row e;
        e.ages = parse_band_(row, "band");
        double total = 0.0;
        for (std::size_t i = 0; i < education_levels_.size(); ++i)
        {
            e.weights.at(i) = non_negative_(row, to_string(education_levels_.at(i)));
            total += e.weights.at(i);
        }
        if (!(total > 0.0)) throw row.error("less_than_basic", "the four shares are all 0");
        return {std::move(cca2), sex_index_(row), e};
    }

    static isco_data parse_isco_(const core::tsv_row& row)
    {
        isco_data i;
        i.code = row.get<std::string>("code");
        i.label = row.get<std::string>("label");
        i.major = row.get<std::string>("major");
        i.skill = row.get_or<std::string>("skill", "");
        return i;
    }

    struct share_data
    {
        std::string country;
        std::size_t sex_index{0};
        std::string code;
        double share{0.0};
    };

    static share_data parse_share_(const core::tsv_row& row)
    {
        share_data s;
        s.country = check_country_(row);
        s.sex_index = sex_index_(row);
        s.code = row.get<std::string>("code");
        s.share = non_negative_(row, "share");
        return s;
    }

    struct factor_data
    {
        std::string country;
        std::string major;
        bool is_age{true};
        band ages{};
        std::size_t level{0};
        double factor{1.0};
    };

    static factor_data parse_factor_(const core::tsv_row& row)
    {
        factor_data f;
        f.country = check_country_(row);
        f.major = row.get<std::string>("major");
        const auto dimension = row["dimension"];
        if (dimension == "age_band")
        {
            f.is_age = true;
            f.ages = parse_band_(row, "value");
        }
        else if (dimension == "education")
        {
            f.is_age = false;
            const auto value = row["value"];
            const auto it = std::ranges::find_if(education_levels_, [&](education_level l) { return to_string(l) == value; });
            if (it == education_levels_.end())
            {
                throw row.error("value", std::format("expected less_than_basic, basic, intermediate or advanced, got '{}'", value));
            }
            f.level = static_cast<std::size_t>(it - education_levels_.begin());
        }
        else
        {
            throw row.error("dimension", std::format("expected age_band or education, got '{}'", dimension));
        }
        f.factor = non_negative_(row, "factor");
        return f;
    }

    struct occupation_data
    {
        job_data job;
        std::string isco;
    };

    static occupation_data parse_job_(const core::tsv_row& row)
    {
        occupation_data o;
        o.job.key = row.get<std::string>("key");
        o.job.label = row.get<std::string>("label");
        o.isco = row.get<std::string>("isco");
        o.job.weight = non_negative_(row, "weight");
        o.job.unit = row.get_or<std::string>("unit", "");
        o.job.min_age = row.get_or<int>("min_age", 0);
        o.job.max_age = row.get_or<int>("max_age", 255);
        if (o.job.min_age < 0) throw row.error("min_age", "must not be negative");
        if (o.job.max_age < o.job.min_age) throw row.error("max_age", "must not be below min_age");
        o.job.skills = split_list_(row.get_or<std::string>("skills", ""));
        o.job.gear = split_list_(row.get_or<std::string>("gear", ""));
        o.job.tags = split_list_(row.get_or<std::string>("tags", ""));
        return o;
    }

    static void validate_(table t, const core::tsv_row& row)
    {
        switch (t)
        {
        case table::status: (void)parse_status_(row); break;
        case table::inactivity: (void)parse_inactivity_(row); break;
        case table::education: (void)parse_education_(row); break;
        case table::isco: (void)parse_isco_(row); break;
        case table::shares: (void)parse_share_(row); break;
        case table::factors: (void)parse_factor_(row); break;
        case table::occupations: (void)parse_job_(row); break;
        }
    }

    // -- State ----------------------------------------------------------------

    tables _tables;
    model _model;
    core::rng _engine{core::rng::from_entropy()};

    struct auto_probe_tag {};

    explicit ocg(auto_probe_tag /*tag*/) : _tables{make_tables_()}
    {
        if (!load(dataset::lite)) (void)load(dataset::full);
    }

    // -- Helpers ----------------------------------------------------------

    static std::span<const core::embedded_file> embedded_files_()
    {
#ifdef DASMIG_OCCUPATION_EMBED
        return embedded::occupationgen::files;
#else
        return {};
#endif
    }

    std::uint64_t draw_seed_() { return _engine(); }

    static documents read_directory_(const std::filesystem::path& dir)
    {
        documents docs;
        for (const auto t : all_tables_)
        {
            const auto file = dir / file_name_(t);
            std::error_code ec;
            if (std::filesystem::is_regular_file(file, ec))
            {
                docs.at(index_(t)) = core::tsv_document::load(file);
            }
        }
        return docs;
    }

    // Apply layers to copies of the tables, then commit: all or nothing.
    // With @p reset (load), tables without a document are emptied.
    void apply_(documents docs, core::layer_mode mode, bool reset)
    {
        tables next = _tables;
        for (const auto t : all_tables_)
        {
            auto& doc = docs.at(index_(t));
            auto& tbl = next.at(index_(t));
            if (!doc)
            {
                if (reset) tbl.clear();
                continue;
            }
            (void)tbl.extend(std::move(*doc), mode, [t](const core::tsv_row& row) { validate_(t, row); });
        }
        auto built = build_model_(next);
        _tables = std::move(next);
        _model = std::move(built);
    }

    static model build_model_(const tables& tbls)
    {
        model m;
        for (const auto& row : tbls.at(index_(table::status)).rows())
        {
            auto [cca2, si, s] = parse_status_(row);
            m.countries[cca2].status.at(si).push_back(s);
        }
        for (const auto& [cca2, c] : m.countries)
        {
            if (!c.status[0].empty() && !c.status[1].empty()) m.codes.push_back(cca2);
        }
        for (const auto& row : tbls.at(index_(table::inactivity)).rows()) m.inactivity.push_back(parse_inactivity_(row));
        for (const auto& row : tbls.at(index_(table::education)).rows())
        {
            auto [cca2, si, e] = parse_education_(row);
            m.countries[cca2].education.at(si).push_back(e);
        }

        std::unordered_map<std::string, std::size_t> isco_index;
        for (const auto& row : tbls.at(index_(table::isco)).rows())
        {
            auto i = parse_isco_(row);
            isco_index[i.code] = m.isco.size();
            m.isco.push_back(std::move(i));
        }
        m.jobs_of.resize(m.isco.size());

        for (const auto& row : tbls.at(index_(table::shares)).rows())
        {
            const auto s = parse_share_(row);
            const auto it = isco_index.find(s.code);
            if (it == isco_index.end())
            {
                m.unresolved.push_back(std::format("{}:{}: unknown ISCO group '{}'", row.source(), row.line(), s.code));
                continue;
            }
            m.countries[s.country].shares.at(s.sex_index).add(it->second, s.share);
        }
        for (const auto& row : tbls.at(index_(table::factors)).rows())
        {
            const auto f = parse_factor_(row);
            auto& set = m.factors[factor_key_(f.country, f.major)];
            if (f.is_age) set.age.emplace_back(f.ages, f.factor);
            else set.education.at(f.level) = f.factor;
        }
        for (const auto& row : tbls.at(index_(table::occupations)).rows())
        {
            auto o = parse_job_(row);
            const auto it = isco_index.find(o.isco);
            if (it == isco_index.end())
            {
                m.unresolved.push_back(std::format("{}:{}: unknown ISCO group '{}'", row.source(), row.line(), o.isco));
                continue;
            }
            o.job.isco_index = it->second;
            m.jobs_of.at(it->second).push_back(m.jobs.size());
            m.jobs.push_back(std::move(o.job));
        }
        return m;
    }

    void require_data_() const
    {
        if (!has_data())
        {
            throw std::runtime_error("No occupation data loaded. Call load() first.");
        }
    }

    template <class Row>
    static const Row* find_row_(const std::vector<Row>& rows, int age)
    {
        const auto it = std::ranges::find_if(rows, [age](const Row& r) { return r.ages.contains(age); });
        return it == rows.end() ? nullptr : &*it;
    }

    template <class T, std::size_t N>
    static T draw_(core::rng& r, const std::array<double, N>& weights, const std::array<T, N>& values)
    {
        core::weighted_table<T> t;
        for (std::size_t i = 0; i < N; ++i) t.add(values.at(i), weights.at(i));
        return t.pick(r);
    }

    [[nodiscard]] employment_status draw_status_(const country_data& c, std::size_t si, int age, std::string_view cca2,
                                                 core::rng& r) const
    {
        if (age < school_age_) return employment_status::inactive;
        if (age < working_age_) return employment_status::student;
        const auto* row = find_row_(c.status.at(si), age);
        if (row == nullptr)
        {
            throw std::invalid_argument(std::format("No employment status data for age {} in {}.", age, cca2));
        }
        const auto kind = draw_(r, row->weights,
                                std::array{employment_status::employed, employment_status::unemployed,
                                           employment_status::inactive});
        if (kind != employment_status::inactive) return kind;
        const auto* split = find_row_(_model.inactivity, age);
        if (split == nullptr) return employment_status::inactive;
        return draw_(r, split->weights,
                     std::array{employment_status::student, employment_status::retired, employment_status::inactive});
    }

    [[nodiscard]] static std::optional<education_level> draw_education_(const country_data& c, std::size_t si, int age,
                                                                        core::rng& r)
    {
        if (age < working_age_) return std::nullopt;
        const auto* row = find_row_(c.education.at(si), age);
        if (row == nullptr) return std::nullopt;
        // A level the person is too young to have completed counts as the one below it.
        std::array<double, 4> w = row->weights;
        for (std::size_t i = w.size() - 1; i > 0; --i)
        {
            if (age < education_min_age_.at(i))
            {
                w.at(i - 1) += w.at(i);
                w.at(i) = 0.0;
            }
        }
        return draw_(r, w, education_levels_);
    }

    [[nodiscard]] static bool job_open_(const job_data& j, int age)
    {
        return j.weight > 0.0 && age >= j.min_age && age <= j.max_age;
    }

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    [[nodiscard]] job_info draw_job_(const country_data& c, std::string_view cca2, std::size_t si, int age,
                                     std::optional<education_level> education, const core::rng& call) const
    {
        // A group is open when one of its jobs is open to this age; its weight is the
        // country's share for the sex, times the factors for the age class and education.
        const int job_age = std::max(age, working_age_);
        std::vector<double> factor(_model.isco.size(), 0.0);
        for (std::size_t i = 0; i < _model.isco.size(); ++i)
        {
            const bool open = std::ranges::any_of(_model.jobs_of.at(i), [&](std::size_t j) {
                return job_open_(_model.jobs.at(j), age);
            });
            if (!open) continue;
            double f = 1.0;
            const auto it = _model.factors.find(factor_key_(cca2, _model.isco.at(i).major));
            if (it != _model.factors.end())
            {
                for (const auto& [ages, value] : it->second.age)
                {
                    if (ages.contains(job_age)) f *= value;
                }
                if (education) f *= it->second.education.at(static_cast<std::size_t>(*education));
            }
            factor.at(i) = f;
        }
        auto rg = call.split("isco");
        const auto group = c.shares.at(si).try_pick_index(rg, [&factor](const std::size_t& i, double base) {
            return base * factor.at(i);
        });
        if (!group)
        {
            throw std::invalid_argument(std::format("No occupation is open to a person aged {} in {}.", age, cca2));
        }
        const std::size_t isco = c.shares.at(si).entries()[*group].value;

        core::weighted_table<std::size_t> jobs;
        for (const auto j : _model.jobs_of.at(isco))
        {
            if (job_open_(_model.jobs.at(j), age)) jobs.add(j, _model.jobs.at(j).weight);
        }
        auto rj = call.split("job");
        const auto& d = _model.jobs.at(jobs.pick(rj));
        const auto& g = _model.isco.at(isco);
        return {.key = d.key,
                .label = d.label,
                .isco = g.code,
                .isco_label = g.label,
                .unit = d.unit,
                .skill = g.skill,
                .skills = d.skills,
                .gear = d.gear,
                .tags = d.tags};
    }

    [[nodiscard]] occupation generate_(const occupation_request& req, std::uint64_t call_seed) const
    {
        require_data_();
        const core::rng call{call_seed};

        std::string cca2{req.country};
        if (cca2.empty())
        {
            if (_model.codes.empty())
            {
                throw std::invalid_argument("No country has employment status data for both sexes.");
            }
            auto cr = call.split("country");
            cca2 = _model.codes[static_cast<std::size_t>(core::bounded(cr, _model.codes.size()))];
        }
        const auto it = _model.countries.find(cca2);
        if (it == _model.countries.end())
        {
            throw std::invalid_argument(std::format("Unknown country code: {}", cca2));
        }
        const auto& c = it->second;

        occupation v;
        v._signature = {.seed = call_seed, .data = data_hash()};
        v.country_code = cca2;
        v.age = req.age;
        if (req.person_sex)
        {
            v.person_sex = *req.person_sex;
        }
        else
        {
            auto rs = call.split("sex");
            v.person_sex = core::bernoulli(rs, 0.5) ? sex::female : sex::male;
        }
        const std::size_t si = v.person_sex == sex::male ? 0 : 1;
        const int age = req.age;

        if (req.status)
        {
            v.status = *req.status;
        }
        else
        {
            auto rt = call.split("status");
            v.status = draw_status_(c, si, age, cca2, rt);
        }
        if (req.education)
        {
            v.education = req.education;
        }
        else
        {
            auto re = call.split("education");
            v.education = draw_education_(c, si, age, re);
        }
        const bool preschool = age < school_age_ && v.status == employment_status::inactive;
        if (v.status != employment_status::student && !preschool)
        {
            v.job = draw_job_(c, cca2, si, age, v.education, call);
        }
        return v;
    }
};

} // namespace dasmig

#endif // DASMIG_OCCUPATIONGEN_HPP
