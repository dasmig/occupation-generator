# Usage Guide

This guide covers every feature of the occupation-generator library. For a quick overview, see the [README](../README.md). For the data and the model, see [Data and Method](data.md). For the full API reference, run `doxygen Doxyfile` from the repository root and open `doc/api/html/index.html`.

[TOC]

<!-- GitHub-rendered TOC (Doxygen uses [TOC] above instead) -->
<ul>
<li><a href="#quick-start">Quick Start</a></li>
<li><a href="#installation">Installation</a></li>
<li><a href="#loading-resources">Loading Resources</a></li>
<li><a href="#generating">Generating</a></li>
<li><a href="#the-result">The Result</a></li>
<li><a href="#seeding-and-deterministic-generation">Seeding and Deterministic Generation</a></li>
<li><a href="#extending-data">Extending Data</a></li>
<li><a href="#replay-and-json">Replay and JSON</a></li>
<li><a href="#compiled-in-data">Compiled-in Data</a></li>
<li><a href="#thread-safety">Thread Safety</a></li>
<li><a href="#error-reference">Error Reference</a></li>
</ul>

## Quick Start

```cpp
#include <dasmig/occupationgen.hpp>
#include <iostream>

int main()
{
    auto& gen = dasmig::ocg::instance();
    std::cout << gen.get_occupation({.country = "BR", .age = 35}) << '\n'; // for example "Shop assistant"
}
```

## Installation

Copy `dasmig/occupationgen.hpp`, `dasmig/core.hpp` and the `dasmig/core/` directory into your include path, keeping them together. Supply `resources/` next to the program, or define `DASMIG_OCCUPATION_EMBED` to compile the `lite` tier in.

## Loading Resources

`instance()` loads the `lite` tier on first use (or `full` if there is no `lite`), from the compiled-in copy or from the first of `resources/`, `../resources`, `occupation-generator/resources` and `../occupation-generator/resources` that has it.

```cpp
dasmig::ocg gen;                           // an empty, independent instance
bool ok = gen.load(dasmig::dataset::lite); // false when the tier is not found
gen.load(std::filesystem::path{"path/to/resources/lite"}); // a directory
gen.has_data();
gen.country_count(); // countries that can be drawn from
gen.job_count();     // jobs in the catalogue
```

Each `load` replaces the data and every layer loaded before. The tiers have the same measured data and differ in the catalogue: `lite` has 95 jobs, `full` 320 (see [Data and Method](data.md#tiers)).

## Generating

Every call takes an `occupation_request`; every field is optional:

```cpp
dasmig::occupation_request req{
    .country = "JP",                           // ISO 3166-1 alpha-2; empty draws one
    .age = 28,                                 // default 40
    .person_sex = dasmig::sex::female,         // unknown draws one (even odds)
    .education = dasmig::education_level::advanced, // unknown draws one
    .status = dasmig::employment_status::employed,  // unknown draws one
};
auto v = gen.get_occupation(req);      // draws a seed from the instance engine
auto w = gen.get_occupation(req, 1234); // deterministic; does not touch the engine

auto a = gen.get_occupation("US");     // age 40, everything else drawn
auto b = gen.get_occupation();         // a random country as well
```

- **Status.** From the country's employment and unemployment rates for the person's sex and five-year age band; the inactive are split into students, retired and others by age. Under 15 a person is always a student.
- **Education.** From the country's shares for the sex and age band, unless given. A person too young for a level has the one below.
- **Job.** An ISCO-08 group is drawn by the country's employment shares for the sex, shifted by how common the group is at the person's age and education, then a job of the catalogue within the group. A job with an age limit is not given to someone outside it. A student has no job; an unemployed, retired or inactive person has the job the data would give a worker like them (their usual or last occupation).

```cpp
// The worker and their working conditions, with a forced status:
auto nurse_or_so = gen.get_occupation({.country = "DE", .age = 45,
                                       .status = dasmig::employment_status::employed});
```

A country chosen at random is uniform over the countries loaded; the data has no population column. Pass a country chosen by country-generator for a population-weighted world.

## The Result

```cpp
v.country_code;   // "JP"
v.person_sex;     // dasmig::sex::female
v.age;            // 28
v.status;         // employed, unemployed, student, retired or inactive
v.education;      // std::optional<education_level>; empty under 15 unless given
v.job;            // std::optional<job_info>; empty for students

if (v.job)
{
    v.job->key;        // "software_developer"
    v.job->label;      // "Software developer"
    v.job->isco;       // "25", the ISCO-08 group
    v.job->isco_label; // "Information and communications technology professionals"
    v.job->unit;       // "2512", the ISCO-08 unit group
    v.job->skill;      // "3-4", the ISCO-08 skill level
    v.job->skills;     // {"programming", "debugging", "algorithms"}
    v.job->gear;       // {"laptop", "headphones"}
    v.job->tags;       // {"technical", "desk"}: for other generators
}
dasmig::to_string(v.status);  // "employed"
dasmig::to_string(*v.education); // "advanced"
```

A result converts to `std::string` (the job label, or the status when there is no job) and streams with `<<`.

## Seeding and Deterministic Generation

- `get_occupation(request, seed)` gives the same result for the same seed, request and data, on every platform.
- `seed(s)` makes the instance engine deterministic, so a sequence of `get_occupation()` calls repeats; `unseed()` reseeds it from the system.
- `v.seed()` is the seed a result was generated from: `get_occupation(request, v.seed())` replays it.
- Each field has a stream of its own (country, sex, status, education, ISCO group, job), so forcing one in the request never changes the others. The same seed with `.education` forced gives the same sex and status.

## Extending Data

The shipped data is a base layer. Files with the same columns as the shipped tables can be loaded on top of it to add rows, change them or remove them, without editing the shipped files. The tables and their keys:

| Table (`ocg::table`) | File | Key |
| --- | --- | --- |
| `status` | `status.tsv` | `country_code`, `sex`, `band` |
| `inactivity` | `inactivity.tsv` | `band` |
| `education` | `education.tsv` | `country_code`, `sex`, `band` |
| `isco` | `isco.tsv` | `code` |
| `shares` | `shares.tsv` | `country_code`, `sex`, `code` |
| `factors` | `factors.tsv` | `country_code`, `major`, `dimension`, `value` |
| `occupations` | `occupations.tsv` | `key` |

```cpp
using table = dasmig::ocg::table;

// Add a job to a group (here 54, protective services), from text; for
// example supplied by a WebAssembly host. In override mode (the default) a
// row replaces the loaded row with its key and new keys are added.
gen.extend_from_string(table::occupations,
    "key\tlabel\tisco\tweight\ttags\tskills\tgear\n"
    "bounty_hunter\tBounty hunter\t54\t5\trisk;solo\ttracking\tvest;handcuffs\n", "scenario");

// Remove jobs: a layer needs only the key and a disabled column.
gen.extend_from_string(table::occupations, "key\tdisabled\nbounty_hunter\t1\n", "cleanup");

// A directory with any of the files, applied together or not at all.
gen.extend("mods/post-collapse");

// One file; add mode refuses to touch keys that are already loaded.
gen.extend("mods/jobs.tsv", dasmig::core::layer_mode::add);
```

- A group's jobs share the group's weight by their `weight`, so a job added to a group takes a share of what the group already has. Give a new group (`isco.tsv`) rows in `shares.tsv`, or it is never drawn.
- A band is `15-19` or `65+` (no upper limit). The bands of `status.tsv` and `education.tsv` need not match the shipped ones; a person is looked up by age in the rows of their country and sex.
- A row that names an ISCO group which is not loaded is ignored and listed in `unresolved()`, so removing a group needs no other change.
- Every value in a layer is checked before anything changes. A bad layer throws `dasmig::core::tsv_error` with the file, line and column, and the data stays as it was.
- `layers(table)` lists what is loaded: source, mode, rows added, replaced and removed, and warnings such as unknown columns.
- `load(...)` replaces the base and drops every layer.

## Replay and JSON

Every result records the seed and the data it came from:

```cpp
auto v = gen.get_occupation();
v.signature().seed; // the call seed
v.signature().data; // gen.data_hash() when it was generated
```

`to_json()` gives the result as JSON, the form the WebAssembly layer and the game consume. Seeds are strings because JavaScript numbers cannot hold every 64-bit value. `to_json(writer)` writes into an existing `dasmig::core::json_writer`, for composing larger documents.

```json
{"country":"DE","sex":"female","age":40,"status":"unemployed","education":"advanced","job":{"key":"accountant","label":"Accountant","isco":"24","isco_label":"Business and administration professionals","unit":"2411","skill":"3-4","skills":["accounting","auditing","spreadsheets"],"gear":["suit","calculator"],"tags":["desk","academic"]},"signature":{"seed":"000000000000002a","data":"1810ef13288a98e8"}}
```

`education` and `job` are `null` when there are none.

## Compiled-in Data

Define `DASMIG_OCCUPATION_EMBED` before including the header to compile the `lite` tier into the program. `load(dataset::lite)` and `instance()` then use it and need no files. The compiled-in data has the same `data_hash()` as the file, so seeds replay the same either way. The header is about 2 MB.

`dasmig/occupationgen_data.hpp` is generated from `resources/lite` by `make embed`; CI checks it is up to date.

## Thread Safety

Each `ocg` instance is independent. Concurrent calls to `get_occupation()` on the **same** instance require external synchronisation (e.g. a `std::mutex`).

## Error Reference

| Exception | When |
| --- | --- |
| `std::runtime_error` | Any generation call before data is loaded |
| `std::invalid_argument` | An unknown country code; a country with no status row for the person's sex and age; no job open to the person (a country with no shares for the sex, or a job age limit that excludes everyone); with no country, no country has status rows for both sexes |
| `std::invalid_argument` | `extend()` of a file whose name is not a table's |
| `dasmig::core::tsv_error` | A malformed table file or layer (message gives file, line and column); the data is unchanged |
