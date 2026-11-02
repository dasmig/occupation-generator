# Data and Method

The data in `resources/` and how occupation-generator uses it (rule 6 of the dasmig standard). For using the library, see the [usage guide](usage.md).

[TOC]

There are ten tables. Five are measured, from the ILO's labour force statistics (ILOSTAT): the employment status of the population by country, sex and age; its education; the ISCO-08 group of the employed by country and sex; and how much the age and education of workers shift those groups. Two are curated: the catalogue of jobs, and how the inactive split into students, retired and others. Three describe income (see [Income](#income)): its level and spread by country, from ILOSTAT's earnings statistics, and pay within an occupation group, from four national surveys.

**What is measured and what is curated.** The *rates and shares* are measured, and each row says whether its country was surveyed or filled in from its region. The *jobs* are curated: no open dataset lists job titles with the skills and gear a game needs. A job is mapped to an ISCO-08 group, and the group is what the measured data speaks about. How the jobs of a group share the group's weight is a judgement.

## Tiers

| Tier | Countries | Jobs | Notes |
| --- | --- | --- | --- |
| `lite` | 213 | 95 (1 to 5 per ISCO group) | The measured tables are the same in both tiers |
| `full` | 213 | 320 (2 to 18 per ISCO group) | The full catalogue |

The 213 countries are every independent country in country-generator's `full` list plus the territories ILOSTAT has a survey for (Hong Kong, Macau, Puerto Rico, Taiwan, Palestine, New Caledonia and 12 others). Antarctica and the other territories without a population or a survey are left out. Because the tiers differ only in the catalogue, a group in `lite` often has a single job that takes the group's whole share: Software developer is every ICT professional there.

## The measured tables

Source: ILO, [ILOSTAT](https://ilostat.ilo.org), indicators downloaded through its bulk API by `scripts/fetch_ilostat.py`. ILOSTAT data is published under CC BY 4.0. The indicators:

| Indicator | Used for |
| --- | --- |
| `EMP_TEMP_SEX_OC2_NB_A`: employment by sex and ISCO-08 level 2 | `shares.tsv` |
| `EMP_TEMP_SEX_AGE_OCU_NB_A`: employment by age class and ISCO-08 level 1 | `factors.tsv`, `age_band` rows |
| `EMP_TEMP_SEX_OCU_EDU_NB_A`: employment by ISCO-08 level 1 and education | `factors.tsv`, `education` rows |
| `POP_XWAP_SEX_AGE_EDU_NB_A`: working-age population by sex, age and education | `education.tsv` |
| `POP_XWAP_SEX_AGE_NB_A`, `EMP_TEMP_SEX_AGE_NB_A`, `UNE_TUNE_SEX_AGE_NB_A`: working-age population, employment and unemployment by sex and five-year age band | `status.tsv` |

Every measured row has a `basis` and a `year`. `basis` is `ilostat` when the country has a survey, and `region` when it does not. For each country and table the script takes the **latest survey since 2010** that has the complete table (the same source and year for every cell), so one table's year can differ from another's.

**Countries with no survey.** A country gets the unweighted mean of the surveyed countries in its UN subregion (else its region, else the world), with `basis` `region` and `year` empty. The number of countries with a survey of their own:

| Table | Surveyed | Filled in |
| --- | --- | --- |
| `status` | 139 | 74 |
| `education` | 185 | 28 |
| `shares` | 149 | 64 |
| `factors`, `age_band` | 166 | 47 |
| `factors`, `education` | 148 | 65 |

Most surveys are from 2022 to 2025 (23 of the 139 status surveys are from 2024, 45 from 2025); the oldest are from 2010. A regional mean is a guess for a country that may be far from the average of its neighbours. Rows with `basis` `region` should not be read as a statement about that country.

### `status.tsv`, keyed by `country_code`, `sex` and `band`

| Column | Meaning |
| --- | --- |
| `country_code` | ISO 3166-1 alpha-2 code (the key used by country-generator) |
| `sex` | `male` or `female` |
| `band` | Five-year age band: `15-19`, `20-24`, ... `60-64`, and `65+` |
| `employed`, `unemployed`, `inactive` | Percent of the *population* of that sex and band: employment divided by the working-age population, unemployment divided by it, and the rest. They add up to 100 |
| `basis`, `year` | As above |

"Employed" and "unemployed" follow the ILO's definitions (a person is unemployed when they have no work, are available and are looking for it); the inactive are everyone else of working age. The `65+` row covers everyone from 65 to the end of life, so an 85-year-old has the employment rate of the average person over 65. Under 15 there is no data: a person is a student from 6 to 14 (school age, which is 4 to 7 depending on the country; 6 is the common one), and inactive under 6.

If a survey has the three indicators from different sources, the script uses the employment source's and takes the others from the same year, else any source with a complete set. Where employment and unemployment together exceed the population (inconsistent sources) they are scaled down to fit.

### `inactivity.tsv` (curated), keyed by `band`

How the inactive of an age band split into `student`, `retired` and `other`. **Curated: no table describes this directly.** The shares follow what the labour-force surveys suggest about reasons for inactivity (education dominates under 25, retirement over 55, care and illness in between), but the numbers are judgements and the same for every country:

| Band | Student | Retired | Other |
| --- | --- | --- | --- |
| `15-19` | 0.85 | 0 | 0.15 |
| `20-24` | 0.55 | 0 | 0.45 |
| `25-29` | 0.12 | 0 | 0.88 |
| `30-34` | 0.04 | 0 | 0.96 |
| `35-54` | 0.01 | 0 | 0.99 |
| `55-59` | 0 | 0.20 | 0.80 |
| `60-64` | 0 | 0.50 | 0.50 |
| `65+` | 0 | 0.92 | 0.08 |

The country matters here: where women stay home the "other" share should be larger and where schooling is long the student share should be, but this table does not know. A person who is employed and studying at once counts as employed.

### `education.tsv`, keyed by `country_code`, `sex` and `band`

| Column | Meaning |
| --- | --- |
| `band` | Ten-year age band: `15-24`, `25-34`, `35-44`, `45-54`, `55-64`, `65+` |
| `less_than_basic`, `basic`, `intermediate`, `advanced` | Percent of the working-age population of that sex and band at each of the ILO's four aggregate education levels, among those whose level is known |

The levels are ILOSTAT's `EDU_AGGREGATE` groups: less than basic (below primary or none), basic (primary and lower secondary, ISCED 1 to 2), intermediate (upper secondary and post-secondary non-tertiary, ISCED 3 to 4) and advanced (tertiary, ISCED 5 to 8). Countries classify their own systems into these, and not all use all four: Japan reports no "basic" level at all, so its shares have 0 there in every band.

**Age limits.** The age band of a young person is wide: the `15-24` row describes ages 15 to 24 together. The generator therefore applies minimum ages (curated): basic needs 14 years, intermediate 17 and advanced 21. A person too young for a level counts as the one below, so a 16-year-old who would be "intermediate" in the band is "basic".

### `isco.tsv` (curated), keyed by `code`

The 43 groups of ISCO-08 at level 2: `code` (`11` to `96`), `label` (the ISCO-08 title), `major` (the first digit, which ILOSTAT uses for the age and education tables; `0` for the armed forces), and `skill`, the ISCO-08 skill level of the major group (`1`, `2` or `3-4`; empty for other ranks of the armed forces, which span levels). Written from the ISCO-08 structure published by the ILO.

### `shares.tsv`, keyed by `country_code`, `sex` and `code`

`share` is the percent of the employed of that sex who work in the ISCO-08 group `code` (omitted when 0). Each country and sex adds up to 100. Source: `EMP_TEMP_SEX_OC2_NB_A`. The cells ILOSTAT flags as unreliable (a small sample) are kept, because dropping them would bias the total. ILOSTAT's "not classified" group is left out and the rest rescaled.

How well a country's occupations map to ISCO-08 varies: some use ISCO-08 directly, others convert from a national scheme, and a few groups are not reported separately (for example group `63`, subsistence farmers, is often folded into `61`). Differences between countries in a group's share are partly real and partly classification, so compare the major groups rather than single level-2 groups.

### `factors.tsv`, keyed by `country_code`, `major`, `dimension` and `value`

Multipliers on a group's share, by the ISCO major group `major` (`1` to `9`, and `0` for the armed forces) of the group, for two dimensions:

| Dimension | Values | Meaning |
| --- | --- | --- |
| `age_band` | `15-24`, `25-54`, `55-64`, `65+` | P(age class | major group) / P(age class), among the employed |
| `education` | the four levels | P(education | major group) / P(education), among the employed |

A factor above 1 means the major group employs more of that age class or level than the labour force as a whole does; below 1 means fewer. Rows within 3% of 1 are left out (a missing row is 1) and factors are limited to 0.05 to 8. They are the Bayes factors that turn the country's shares for a sex into shares for someone of a given age and education: with P(group) the share and the factors above, P(group | age, education) is proportional to share × age factor × education factor.

Sources: `EMP_TEMP_SEX_AGE_OCU_NB_A` (the age classes are the only ones ILOSTAT cross-tabulates with ISCO-08: 15 to 24, 25 to 54, 55 to 64 and 65 and over) and `EMP_TEMP_SEX_OCU_EDU_NB_A`. Both are for men and women together.

**What this assumes.** The factors are the same for men and women, and the age and education factors are multiplied as if age and education were independent given the group. Neither is true: young men and young women differ in the groups they enter, and the young are more educated within every group. Where the data has both, the product over-corrects a little (a young graduate is a little too sure to be a professional). The factors also describe the *employed* and are applied to the unemployed, retired and others as their usual or last job.

## The job catalogue (curated)

### `occupations.tsv`, keyed by `key`

| Column | Meaning | Source |
| --- | --- | --- |
| `key`, `label` | Stable identifier and display name | Curated |
| `isco` | The ISCO-08 level-2 group. The group's measured weight is spread over its jobs | Curated mapping, using the ISCO-08 unit-group definitions |
| `unit` | The ISCO-08 unit group (four digits) the job belongs to, for reference | Curated |
| `weight` | Relative weight within its group (0.1 to 8) | Curated: a judgement of how common the job is within the group, not a measurement |
| `min_age`, `max_age` | Hard limits, such as 29 for a surgeon, 30 for a judge and 40 for a model; blank for none | Curated |
| `tags` | `;`-separated character tags (`manual`, `caring`, `desk`, `risk`, `leadership`, ...) for other generators, such as nickname-generator | Curated |
| `skills` | `;`-separated skill keywords | Curated, with O*NET and ESCO as a guide; no text was copied |
| `gear` | `;`-separated clothing and gear keywords for a game's art kit | Curated |

The catalogue is in `resources/raw/occupations_curated.tsv` with a `tier` column saying which tier a job belongs to. `lite` has between one and five jobs per group and `full` up to 18. Every ISCO group that has employment in any country has at least one job in each tier (the build script checks this), and a group in which no job is open to a person's age is skipped when the group is drawn.

The weights, tags, skills and gear are not from a dataset; they are one person's idea of the job. A layer can replace any of them (see [Extending Data](usage.md#extending-data)).

## Income

An employed person has a gross annual income at 2025 prices in their country's currency, rounded to three significant figures. The method is in [`designs/occupation-income.md`](https://github.com/dasmig/dasmig-generators/blob/main/designs/occupation-income.md); this section describes the result.

**Sources.**

| Source | Used for |
| --- | --- |
| ILOSTAT `EAR_EMTM_SEX_NB_A`: median monthly earnings of employees by sex | the level (`income.tsv`) |
| ILOSTAT `EAR_EMTM_SEX_OCU_NB_A`, `EAR_EMTM_SEX_AGE_NB_A`, `EAR_EMTM_SEX_EDU_NB_A`: the medians by ISCO major group, 10-year age band and education | the effects (`income_effects.tsv`) |
| ILOSTAT `EAR_EMTG_SEX_NB_A`, `EAR_EMTG_SEX_OCU_NB_A`: the Gini of earnings, overall and by major group | the spread (`sigma`) |
| ILOSTAT `EAR_EMTM_SEX_OCU_CUR_NB_A` (USD values), `CPI_NCYR_COI_RT_A` (consumer prices) | currency conversion, uprating to 2025 |
| World Bank WDI `NY.GDP.PCAP.CN`, `PA.NUS.FCRF` (CC BY 4.0) | the level of countries without a survey; the sanity check; exchange rates |
| DGBAS (Taiwan): median annual total earnings of employees by sex, 2024; consumer prices; GDP per capita (`resources/raw/income_level_curated.tsv`, `income_anchor_curated.tsv`) | Taiwan's level and its sanity check |
| ABS *Employee Earnings and Hours* May 2025 (AU), BLS OEWS May 2024 (US), IBGE PNAD Contínua 2025 Q4 (BR), DANE GEIH 2025 (CO) | pay within a major group (`pay.tsv`) |
| country-generator's `locales.tsv` and `currencies.tsv` | the current currency and its minor digits |

**Coverage.** 201 of the 213 countries have an income. 126 have an earnings survey since 2010 (116 in their own currency; 10 converted, mostly EU-LFS rows that ILOSTAT publishes in euros for non-euro countries). 74 have no usable survey, among them China, Japan, Russia and Korea; their level is GDP per capita times the ratio of earnings to GDP per capita in their subregion, and their effects, spread and sex gap are the subregion's. Taiwan, which ILOSTAT does not cover, takes its level from its statistics office instead (`basis` `curated`): DGBAS's median annual total earnings of employees in industry and services by sex for 2024 (NT$585,000 and NT$512,000, regular and irregular pay, so year-end bonuses are in), uprated to 2025 by its CPI (1.66%). Its effects, spread and pay within a group are Eastern Asia's, and it does not enter its subregion's means, so no other country moves. Its earnings are 0.45 times its GDP per capita; the subregion's ratio would have doubled them. A survey is rejected when the CPI from its year to 2025 has gaps or rises more than threefold, or when its annual median is outside 0.2 to 3 times GDP per capita; the build script lists each rejection and its reason. 12 countries have no income: North Korea, South Sudan, Eritrea, Syria, Yemen, Réunion, and six small territories (Cook Islands, Falkland Islands, Montserrat, Niue, Vatican City, Wallis and Futuna). `ocg::income_currency` says whether a country has one.

### `income.tsv`, keyed by `country_code`

| Column | Meaning |
| --- | --- |
| `currency`, `digits` | ISO 4217 code and minor digits |
| `median_male`, `median_female` | e^μ: the baseline annual income of each sex before the effects, solved so that each sex's median over everything the library draws is the measured median |
| `pay_anchor` | the `pay.tsv` anchor the country uses: its own (`AU`, `US`, `BR`, `CO`) or `pooled` |
| `basis`, `year` | `ilostat`, `converted`, `gdp` or `curated` (a national office's median, Taiwan), and the survey year |

### `income_effects.tsv`, keyed by `country_code`, `dimension` and `value`

| `dimension` | `value` | Columns |
| --- | --- | --- |
| `major` | `0` to `9` | `effect`, `sigma` |
| `age_band` | `15-24` ... `65+` | `effect` |
| `education` | the four levels | `effect` |

The effects are multipliers, fitted log-additively (backfitting) to the library's own distribution of the employed, so that the median by major group, age band and education matches ILOSTAT's ratios without counting the overlap of occupation and education twice. ILOSTAT's medians describe employees and the library describes everyone employed, so only the differences between levels are kept. `sigma` is the log-normal spread within the group: from the group's Gini, less the spread that age, education and pay already explain, at least 0.15. A missing row has effect 1; a group without a sigma takes the mean of the country's others.

### `pay.tsv`, keyed by `anchor` and `unit`

Pay relative to the ISCO major group, by ISCO-08 unit group (the `unit` of `occupations.tsv`). Each anchor is a survey that publishes earnings by detailed occupation: Australia (mean weekly earnings, ANZSCO mapped to ISCO-08), the United States (median annual wage, SOC mapped through O*NET's and BLS's crosswalks), Brazil and Colombia (weighted medians from the microdata, cells of at least 150 respondents). `resources/raw/pay_anchors.tsv` keeps the measured values, so a rebuild needs none of those downloads. A country with its own anchor uses it; every other country uses `pooled`, the mean of the anchors' centred log pay times 0.86 (the mean leave-one-out slope: predicting each anchor from the others shrinks towards the group). A unit fewer than two anchors cover has pay 1.

When it loads, the library normalises pay per country and major group, so that the geometric mean over the country's jobs in the group (by its shares and the job weights) is 1. Pay therefore spreads a group's median over its jobs without moving it, and a layer that changes the catalogue or the shares keeps that true.

### How well it matches

Drawing 40,000 people per country and sex in `full`, with ages from the working-age population (`scripts/income_sample.cpp` and `scripts/validate_income.py`):

| Check | Result |
| --- | --- |
| Median of each sex vs. its target | within 3% for 382 of 386 country-sex pairs |
| Ratio of women's to men's median vs. ILOSTAT | mean difference 2.4% |
| Gini vs. ILOSTAT | mean difference 0.033; within 0.03 for 99 of 142 |
| Median by major group vs. ILOSTAT's ratios | the worst group of a country is off by 3.9% (median over countries) |

The Gini is too low where it is highest (South Africa 0.57 against 0.68, Uganda 0.58 against 0.66): a log-normal by group does not reach the top of those distributions.

## Generation Model

For a call with country `c`, sex `s` and age `a`:

1. **Country.** When not given, uniform over the countries that have status rows for both sexes (stream `"country"`). The data has no population column; pass a country chosen by country-generator for a population-weighted world.
2. **Sex.** When not given, even odds (stream `"sex"`).
3. **Status.** Under 6: inactive. From 6 to 14: student. Otherwise one of employed, unemployed and inactive by the `status.tsv` row for `c`, `s` and `a`'s band (stream `"status"`); if inactive, student, retired or other by `inactivity.tsv` (the same stream, next value).
4. **Education.** When not given and `a` is 15 or more: one of the four levels by the `education.tsv` row (stream `"education"`), after moving the weight of levels `a` is too young for down a level. Under 15: none.
5. **Job**, unless the status is student, or inactive under 6. Each ISCO group `g` has the weight `share(c, s, g)` × the age factor for `g`'s major group and `a`'s age class × the education factor for `g`'s major group and the person's level; a group with no job open to `a` has weight 0 (stream `"isco"`). Then a job of the group by catalogue `weight`, among those open to `a` (stream `"job"`). The age class of a person under 15 is `15-24`.

6. **Income**, when the status is employed and the country has income data (stream `"income"`). The median of the person's cell is `median_<sex>` × the effects of the job's major group, the person's age band and education × the job's `pay`, normalised as above; the income is that median × e^(σz), with σ the major group's and z a standard normal clamped to ±3.5, rounded to three significant figures and at least 1. The amount is annual, 12 times the monthly one.

Each field draws from a stream of its own (`call.split(label)`), so forcing one field in the request never changes another. The income comes last and from its own stream: every other field is the same as in 3.0, for the same seed.

## Rebuilding the Data

```bash
python scripts/fetch_ilostat.py                      # raw_data/ilostat_*.tsv (not in the repository)
python scripts/fetch_worldbank.py                    # raw_data/wb_*.tsv
python scripts/prepare_occupations.py ../country-generator/resources/full/countries.tsv
python scripts/prepare_pay.py                        # resources/raw/pay_anchors.tsv; needs raw_data/pay/, openpyxl and xlrd
python scripts/prepare_income.py ../country-generator/resources/full
make embed                                           # regenerate dasmig/occupationgen_data.hpp
```

`prepare_pay.py` needs the four surveys in `raw_data/pay/` (its docstring lists the files; bls.gov refuses scripted downloads, so the US files are fetched by hand). Its output is committed, so `prepare_income.py` runs without them. `prepare_income.py --check` prints the report without writing.

`prepare_occupations.py` reads the downloads and the curated inputs in `resources/raw/` and writes both tiers. ILOSTAT revises its figures and adds years, so a rebuild today gives different numbers from the ones shipped; the committed `resources/` are the reference. They were built on 2026-10-14.

## Known Limitations

- **Countries without a survey.** 74 countries have no employment-status survey of their own, and 64 no occupation survey; they get a regional mean (see above). A survey also describes one year.
- **Resolution of age.** Employment status uses five-year bands, but the effect of age on the group has only four classes (15-24, 25-54, 55-64, 65 and over), so the jobs of a 30-year-old and a 50-year-old differ only through the catalogue's age limits. Everyone over 65 has the same status.
- **Sex and age and education are treated as independent** when the factors are applied (see above).
- **Education is for the working-age population, not the employed**, and is drawn before the job, so the job is chosen given the education (not the reverse).
- **Inactivity is the same in every country** (curated split of the inactive). In particular no country has more students or fewer retired than another, beyond what its inactivity rate implies.
- **Unemployed, retired and inactive people** are given the job the data would give a worker of their age: their usual or last occupation, not a record of it. Unemployed 20-year-olds have a "last job" they may never have had.
- **No region, city size or rural/urban split.** The data does not distinguish a farmer in Kansas from one in Kerala within a country.
- **Self-employment, hours and sector** are not modelled. ILOSTAT has them; they are left for a later version.
- **Income describes employees.** ILOSTAT's earnings are for employees, and the library gives an income to everyone employed: farmers and traders on their own account earn like employees of their group, which puts incomes at the bottom too high where self-employment is common. Income is gross, for all employees including part-timers, and has no tax, benefits or second jobs.
- **Pay within a group comes from four countries.** Every other country uses their pooled pattern, which predicts a country's own pattern about as well as its nearest anchor does but not better; Europe, Africa and Asia have no anchor of their own.
- **Income is not linked to anything else**: it does not depend on region, city or hours, and two people with the same cell differ only by chance.
- **The catalogue is Western-leaning** at the detail level. It has jobs for every ISCO group and many for farming, craft and elementary work, but a job that exists only in one culture (a rickshaw puller, a shotfirer) appears in every country that has the group.
