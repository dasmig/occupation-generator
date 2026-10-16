# Occupation Generator for C++

> **Requires C++23** (e.g., `-std=c++23` for GCC/Clang, `/std:c++latest` for MSVC).

[![GitHub license](https://img.shields.io/badge/license-MIT-blue.svg)](https://raw.githubusercontent.com/dasmig/occupation-generator/main/LICENSE.MIT)
[![CI](https://github.com/dasmig/occupation-generator/actions/workflows/ci.yml/badge.svg)](https://github.com/dasmig/occupation-generator/actions/workflows/ci.yml)
[![GitHub Releases](https://img.shields.io/github/release/dasmig/occupation-generator.svg)](https://github.com/dasmig/occupation-generator/releases)
[![C++23](https://img.shields.io/badge/standard-C%2B%2B23-blue.svg)](https://en.cppreference.com/w/cpp/23)
[![Header-only](https://img.shields.io/badge/type-header--only-green.svg)](https://github.com/dasmig/occupation-generator#integration)
[![Documentation](https://img.shields.io/badge/docs-GitHub%20Pages-blue.svg)](https://dasmig.github.io/occupation-generator/)

**[API Reference](https://dasmig.github.io/occupation-generator/)** · **[Usage Guide](doc/usage.md)** · **[Data and Method](doc/data.md)** · **[Releases](https://github.com/dasmig/occupation-generator/releases)**

Employment status, education and occupation by country, age and sex. A call draws a **status** (employed, unemployed, student, retired or inactive), an **education level** and, for anyone who is not a student, a **job** with its ISCO-08 group, skills, gear and tags, from labour force statistics for 213 countries and territories.

## Features

- **Measured by Country, Sex and Age**. Employment status, education and the share of each ISCO-08 occupation group come from the ILO's labour force statistics (ILOSTAT): 139 to 185 countries surveyed since 2010, and a regional mean for the rest. The job catalogue (95 jobs in `lite`, 320 in `full`, with skills, gear and tags) is curated. [Data and Method](doc/data.md) says which rows are which.

- **Batch Generation**. Generate thousands of results from one seed, on several threads with the same output for any thread count, optionally unique by a key, and stream them as JSON Lines or CSV (see the usage guide).

- **Age and Education Shape the Job**. A job's weight is the country's share for the sex, times how common its group is at the person's age and education. Young people do not become judges, and the educated are not often refuse collectors.

- **Request-First Generation**. Every call takes an `occupation_request` with country, age, sex, education and status. No mutable generation defaults; data loading and layers stay on the instance.

- **Deterministic Seeding**. Per-call `get_occupation(request, seed)` for reproducible results and `occupation::seed()` for replaying a previous generation. For deterministic sequences, supply seeds from a caller-owned `dasmig::core::rng`.

- **Identical Everywhere**. A seed gives the same result on every compiler, standard library and CPU (GCC, Clang, MSVC, Emscripten; x86-64 and ARM64), checked by known-answer tests.

- **Extensible Data**. Load your own TSV layers on top of the shipped data to add, change or remove entries (`extend()`), checked before they are applied.

- **JSON and Replay**. `to_json()` on every result, and a `signature()` recording the seed and the data it came from.

- **Compiled-in Data**. Define `DASMIG_OCCUPATION_EMBED` to build the `lite` tier into the program, for WebAssembly and other builds without files.

- **Multi-Instance Support**. Construct independent `ocg` instances with their own data and random engine.

## Integration

[`occupationgen.hpp`](https://github.com/dasmig/occupation-generator/blob/main/dasmig/occupationgen.hpp) is the library [released here](https://github.com/dasmig/occupation-generator/releases). Keep [`core.hpp`](https://github.com/dasmig/occupation-generator/blob/main/dasmig/core.hpp) and the [`core/`](https://github.com/dasmig/occupation-generator/tree/main/dasmig/core) directory ([dasmig-core](https://github.com/dasmig/dasmig-core)) in the same directory. Add

```cpp
#include <dasmig/occupationgen.hpp>
```

to the files you want to use it in, and supply the [`resources`](https://github.com/dasmig/occupation-generator/tree/main/resources) folder (or define `DASMIG_OCCUPATION_EMBED`).

## Usage

```cpp
#include <dasmig/occupationgen.hpp>
#include <iostream>

auto& gen = dasmig::ocg::instance(); // loads the lite tier

// A 35-year-old Brazilian woman: status, education and job are drawn.
const dasmig::occupation_request request{.country = "BR", .age = 35, .person_sex = dasmig::sex::female};
auto v = gen.get_occupation(request);
std::cout << v << '\n';           // the job label, for example "Shop assistant"
std::cout << v.to_json() << '\n'; // {"country":"BR",...,"status":"employed","job":{...},"signature":{...}}
if (v.job) std::cout << v.job->isco_label << '\n'; // "Sales workers"

// The same seed and request give the same result everywhere.
auto again = gen.get_occupation(request, v.seed());
```

The only generation overloads are `get_occupation(request)` and `get_occupation(request, seed)`. Use `get_occupation({})` for a random country at the default age of 40, or `get_occupation({.country = "US"})` to specify only the country. The old no-argument, country-only and seed-only overloads and instance `seed()` / `unseed()` controls have been removed.

See the [usage guide](doc/usage.md) for everything else.

## Building

```bash
make            # example
make test       # tests
make bench      # benchmarks, results in build/bench.tsv
make test-embed # tests with the lite tier compiled in
make coverage   # code coverage
make lint       # clang-tidy
make docs       # API docs
make embed      # regenerate dasmig/occupationgen_data.hpp from resources/lite
```

## Compiler Support

Tested with:
- Clang 18+ (`-std=c++23`)
- GCC 14+ (`-std=c++23`)
- MSVC 19.38+ (`/std:c++latest`)
- Emscripten 3.1.74

## Dependencies

| Dependency | Version | Bundled | Purpose |
|-----------|---------|---------|---------|
| [dasmig-core](https://github.com/dasmig/dasmig-core) | 1.0 | Yes (`core.hpp`, `core/`) | Platform-independent random engine and distributions, data layers, JSON |
| [Catch2](https://github.com/catchorg/Catch2) | 3.x | Yes (amalgamated) | Unit testing |

## License

This library is released under the [MIT License](https://opensource.org/licenses/MIT).

```
MIT License

Copyright (c) 2026 Diego Dasso Migotto
```
