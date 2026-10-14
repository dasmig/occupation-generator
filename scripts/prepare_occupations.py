"""Build the tables of resources/{lite,full}/ from the ILOSTAT downloads.

Inputs:
  raw_data/ilostat_*.tsv                  fetch_ilostat.py (not in the repository)
  resources/raw/occupations_curated.tsv   the job catalogue (isco, unit, key, ..., tier)
  resources/raw/isco_curated.tsv          ISCO-08 level-2 groups (code, label, major, skill)
  resources/raw/inactivity_curated.tsv    how the inactive split by age (band, student, retired, other)
and country-generator's full countries.tsv (ISO codes and regions).

Outputs, per tier (the measured tables are the same in both; the catalogue
differs): status, inactivity, education, isco, shares, factors, occupations.

Measured tables (basis `ilostat`, `year` = survey year), all percentages or
factors derived from ILOSTAT counts of one survey (source and year):
  status      employed, unemployed and inactive share of the population by sex
              and five-year age band (employment / unemployment over the
              working-age population)
  education   share of the population by sex and ten-year age band at each
              education level (ILO's four aggregate levels)
  shares      share of employment by sex and ISCO-08 level-2 group
  factors     per ISCO major group: P(age class | group) / P(age class) and
              P(education | group) / P(education), the Bayes factors that
              turn the country-and-sex shares into shares for a given age and
              education
Each country's latest complete survey since 2010 is used. A country or
territory with no survey gets the unweighted mean of the measured countries in
its UN subregion (else its region, else the world), with basis `region` and
year 0.

Usage: python prepare_occupations.py COUNTRIES_TSV [RESOURCES_DIR]
"""
import csv
import sys
from collections import defaultdict
from pathlib import Path

RAW_DIR = Path(__file__).resolve().parent.parent / "raw_data"
MIN_YEAR = 2010
SEXES = ("male", "female")
SEX_CODE = {"SEX_M": "male", "SEX_F": "female"}
MAJORS = "1234567890"  # ISCO major groups ("0" = armed forces)
EDU_LEVELS = (("LTB", "less_than_basic"), ("BAS", "basic"), ("INT", "intermediate"), ("ADV", "advanced"))
EDU_NAME = dict(EDU_LEVELS)
STATUS_BANDS = ["15-19", "20-24", "25-29", "30-34", "35-39", "40-44", "45-49", "50-54", "55-59", "60-64", "65+"]
EDU_BANDS = ["15-24", "25-34", "35-44", "45-54", "55-64", "65+"]
JOB_BANDS = [("15-24", "Y15-24"), ("25-54", "Y25-54"), ("55-64", "Y55-64"), ("65+", "YGE65")]
FACTOR_RANGE = (0.05, 8.0)
FACTOR_QUIET = 0.03  # factors within 3% of 1 are left out


def read_tsv(path):
    with open(path, encoding="utf-8", newline="") as f:
        return list(csv.DictReader(f, delimiter="\t"))


def raw(indicator):
    return read_tsv(RAW_DIR / f"ilostat_{indicator}.tsv")


def band_of(classif, prefix):
    """'AGE_5YRBANDS_Y15-19' -> '15-19', '..._YGE65' -> '65+'."""
    tail = classif[len(prefix):]
    if tail == "YGE65":
        return "65+"
    if tail.startswith("Y") and tail[1:2].isdigit() and "-" in tail:
        return tail[1:]
    return None


def groups(rows):
    """Rows by (area, time, source)."""
    g = defaultdict(list)
    for r in rows:
        g[(r["ref_area"], int(r["time"]), r["source"])].append(r)
    return g


def latest(candidates):
    """Pick the candidate with the latest year (then the most data): (year, size, payload)."""
    best = None
    for year, size, payload in candidates:
        if year >= MIN_YEAR and (best is None or (year, size) > (best[0], best[1])):
            best = (year, size, payload)
    return best


# -- Shares -----------------------------------------------------------------------------------


def build_shares(isco_codes):
    out = {}
    for (area, year, source), rs in groups(raw("EMP_TEMP_SEX_OC2_NB_A")).items():
        cells = {}
        for r in rs:
            sex = SEX_CODE.get(r["sex"])
            code = r["classif1"].rsplit("_", 1)[-1]
            if sex and code in isco_codes:
                cells[(sex, code)] = float(r["value"])
        per_sex = {s: sum(v for (x, _), v in cells.items() if x == s) for s in SEXES}
        n = {s: sum(1 for (x, _) in cells if x == s) for s in SEXES}
        if min(per_sex.values()) <= 0 or min(n.values()) < 15:
            continue
        shares = {k: 100.0 * v / per_sex[k[0]] for k, v in cells.items() if v > 0}
        out.setdefault(area, []).append((year, sum(n.values()), shares))
    return {a: latest(c) for a, c in out.items() if latest(c)}


# -- Age and education factors ------------------------------------------------------------------


def bayes(counts, rows, cols):
    """P(col | row) / P(col) for counts[(row, col)]; returns {(row, col): factor}."""
    row_t = {r: sum(counts.get((r, c), 0.0) for c in cols) for r in rows}
    col_t = {c: sum(counts.get((r, c), 0.0) for r in rows) for c in cols}
    total = sum(row_t.values())
    out = {}
    if total <= 0:
        return out
    for r in rows:
        for c in cols:
            if row_t[r] <= 0 or col_t[c] <= 0:
                continue
            f = (counts.get((r, c), 0.0) / row_t[r]) / (col_t[c] / total)
            out[(r, c)] = min(max(f, FACTOR_RANGE[0]), FACTOR_RANGE[1])
    return out


def build_age_factors():
    out = {}
    for (area, year, source), rs in groups(raw("EMP_TEMP_SEX_AGE_OCU_NB_A")).items():
        counts = {}
        for r in rs:
            m = r["classif2"].rsplit("_", 1)[-1]
            cls = r["classif1"].rsplit("_", 1)[-1]
            for band, tag in JOB_BANDS:
                if cls == tag and m in MAJORS:
                    counts[(m, band)] = counts.get((m, band), 0.0) + float(r["value"])  # male + female
        majors = [m for m in MAJORS if sum(v for (x, _), v in counts.items() if x == m) > 0]
        bands = [b for b, _ in JOB_BANDS]
        if len(majors) < 8 or any(sum(v for (_, y), v in counts.items() if y == b) <= 0 for b in bands):
            continue
        f = bayes(counts, majors, bands)
        out.setdefault(area, []).append((year, len(majors), f))
    return {a: latest(c) for a, c in out.items() if latest(c)}


def build_education_factors():
    out = {}
    for (area, year, source), rs in groups(raw("EMP_TEMP_SEX_OCU_EDU_NB_A")).items():
        counts = {}
        for r in rs:
            m = r["classif1"].rsplit("_", 1)[-1]
            e = r["classif2"].rsplit("_", 1)[-1]
            if m in MAJORS and e in EDU_NAME and r["classif1"].startswith("OCU_ISCO08_"):
                counts[(m, EDU_NAME[e])] = float(r["value"])
        majors = [m for m in MAJORS if sum(v for (x, _), v in counts.items() if x == m) > 0]
        levels = [n for _, n in EDU_LEVELS]
        if len(majors) < 8 or any(sum(v for (_, y), v in counts.items() if y == e) <= 0 for e in levels):
            continue
        out.setdefault(area, []).append((year, len(majors), bayes(counts, majors, levels)))
    return {a: latest(c) for a, c in out.items() if latest(c)}


# -- Education of the population --------------------------------------------------------------


def build_education():
    out = {}
    for (area, year, source), rs in groups(raw("POP_XWAP_SEX_AGE_EDU_NB_A")).items():
        cells = {}
        for r in rs:
            sex = SEX_CODE.get(r["sex"])
            band = band_of(r["classif1"], "AGE_10YRBANDS_")
            e = r["classif2"].rsplit("_", 1)[-1]
            if sex and band in EDU_BANDS and e in EDU_NAME:
                cells[(sex, band, EDU_NAME[e])] = float(r["value"])
        shares = {}
        for sex in SEXES:
            for band in EDU_BANDS:
                tot = sum(cells.get((sex, band, n), 0.0) for _, n in EDU_LEVELS)
                if tot > 0:
                    for _, n in EDU_LEVELS:
                        shares[(sex, band, n)] = 100.0 * cells.get((sex, band, n), 0.0) / tot
        if len(shares) == len(SEXES) * len(EDU_BANDS) * len(EDU_LEVELS):
            out.setdefault(area, []).append((year, len(shares), shares))
    return {a: latest(c) for a, c in out.items() if latest(c)}


# -- Employment status --------------------------------------------------------------------------


def status_cells(indicator):
    """{(area, year): {source: {(sex, band): value}}}"""
    d = defaultdict(lambda: defaultdict(dict))
    for r in raw(indicator):
        sex = SEX_CODE.get(r["sex"])
        band = band_of(r["classif1"], "AGE_5YRBANDS_")
        if sex and band in STATUS_BANDS:
            d[(r["ref_area"], int(r["time"]))][r["source"]][(sex, band)] = float(r["value"])
    return d


def build_status():
    pop, emp, une = status_cells("POP_XWAP_SEX_AGE_NB_A"), status_cells("EMP_TEMP_SEX_AGE_NB_A"), status_cells("UNE_TUNE_SEX_AGE_NB_A")
    want = {(s, b) for s in SEXES for b in STATUS_BANDS}

    def full(by_source, prefer):
        # A complete source for (area, year): the preferred one, else any.
        order = ([prefer] if prefer in by_source else []) + [s for s in sorted(by_source) if s != prefer]
        for s in order:
            if want <= by_source[s].keys():
                return by_source[s]
        return None

    out = {}
    for (area, year), emp_sources in emp.items():
        for source in emp_sources:
            e = full({source: emp_sources[source]}, source)
            p = full(pop.get((area, year), {}), source)
            u = full(une.get((area, year), {}), source)
            if not (e and p and u):
                continue
            rows = {}
            for sex, band in sorted(want):
                n = p[(sex, band)]
                if n <= 0:
                    break
                ev, uv = 100.0 * e[(sex, band)] / n, 100.0 * u[(sex, band)] / n
                if ev + uv > 100.0:  # inconsistent sources: rescale
                    ev, uv = 100.0 * ev / (ev + uv), 100.0 * uv / (ev + uv)
                rows[(sex, band)] = {"employed": ev, "unemployed": uv, "inactive": 100.0 - ev - uv}
            else:
                out.setdefault(area, []).append((year, len(rows), rows))
    return {a: latest(c) for a, c in out.items() if latest(c)}


# -- Filling the countries without a survey -------------------------------------------------------


def fill(measured, universe, place_of, zero=False):
    """measured: {cca2: payload}; returns {cca2: (payload, basis)} for every country of the universe.

    payload is {key: number} or {key: {name: number}}; a missing country takes
    the mean of the measured countries in its subregion, region or the world.
    With zero, a key a country lacks counts as 0 (a distribution that omits its zero rows)."""
    def mean(payloads):
        acc, cnt = defaultdict(float), defaultdict(int)
        nested = None
        for p in payloads:
            for k, v in p.items():
                if isinstance(v, dict):
                    nested = True
                    for kk, vv in v.items():
                        acc[(k, kk)] += vv
                        cnt[(k, kk)] += 1
                else:
                    nested = False
                    acc[k] += v
                    cnt[k] += 1
        out = {}
        for k, total in acc.items():
            avg = total / (len(payloads) if zero else cnt[k])
            if nested:
                out.setdefault(k[0], {})[k[1]] = avg
            else:
                out[k] = avg
        return out

    by_place = defaultdict(list)
    for c, (year, _, p) in measured.items():
        for level in place_of.get(c, ()):
            by_place[level].append(p)
    result = {}
    for c in sorted(universe):
        if c in measured:
            year, _, p = measured[c]
            result[c] = (p, "ilostat", year)
            continue
        for level in place_of.get(c, ()):
            if by_place.get(level):
                result[c] = (mean(by_place[level]), "region", 0)
                break
    return result


# -- Writing ----------------------------------------------------------------------------------


def num(x, places=3):
    s = f"{x:.{places}f}".rstrip("0").rstrip(".")
    return s if s not in ("", "-0") else "0"


def write(path, header, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="") as f:
        w = csv.writer(f, delimiter="\t", lineterminator="\n")
        w.writerow(header)
        w.writerows(rows)
    print(f"  {path.name}: {len(rows)} rows")


def main(countries_tsv, out="resources"):
    out = Path(out)
    countries = read_tsv(countries_tsv)
    iso3 = {c["cca3"]: c["cca2"] for c in countries}
    iso3["KOS"] = iso3.get("UNK", "XK")  # ILO's code for Kosovo
    place_of = {}
    for c in countries:
        place_of[c["cca2"]] = tuple(x for x in (c["subregion"], c["region"], "world") if x)
    universe_all = {c["cca2"] for c in countries}

    isco = read_tsv("resources/raw/isco_curated.tsv")
    codes = {r["code"] for r in isco}
    catalogue = read_tsv("resources/raw/occupations_curated.tsv")
    inactivity = read_tsv("resources/raw/inactivity_curated.tsv")

    def to_cca2(measured):
        res, unmapped = {}, set()
        for a, v in measured.items():
            if a in iso3:
                res[iso3[a]] = v
            else:
                unmapped.add(a)
        return res, unmapped

    shares, un1 = to_cca2(build_shares(codes))
    age_f, un2 = to_cca2(build_age_factors())
    edu_f, un3 = to_cca2(build_education_factors())
    edu, un4 = to_cca2(build_education())
    status, un5 = to_cca2(build_status())
    print("measured countries: shares", len(shares), "age factors", len(age_f), "education factors", len(edu_f),
          "education", len(edu), "status", len(status))
    print("unmapped ILO areas (not in country-generator):", sorted(un1 | un2 | un3 | un4 | un5))

    # The library covers independent countries and every country or territory that has a survey.
    independent = {c["cca2"] for c in countries if c.get("independent") == "1"}
    universe = independent | set(shares) | set(status)
    universe &= universe_all

    # Catalogue check: every group that has employment must have a job.
    job_codes = {r["isco"] for r in catalogue}
    present = {k[1] for _, _, s in shares.values() for k in s}
    missing = sorted(present - job_codes)
    if missing:
        sys.exit(f"ISCO groups with employment but no job in the catalogue: {missing}")
    unknown = sorted(job_codes - codes)
    if unknown:
        sys.exit(f"catalogue jobs with an unknown ISCO group: {unknown}")

    shares_f = fill(shares, universe, place_of, zero=True)
    age_ff = fill(age_f, universe, place_of)
    edu_ff = fill(edu_f, universe, place_of)
    educ_f = fill(edu, universe, place_of)
    status_f = fill(status, universe, place_of)
    for name, t in (("shares", shares_f), ("age factors", age_ff), ("education factors", edu_ff),
                    ("education", educ_f), ("status", status_f)):
        gone = sorted(universe - set(t))
        print(f"{name}: {len(t)} countries" + (f"; none for {gone}" if gone else ""))
    universe = universe & set(shares_f) & set(age_ff) & set(edu_ff) & set(educ_f) & set(status_f)
    print("countries in the library:", len(universe))

    rows_status = [
        [c, sex, band, num(p[(sex, band)]["employed"], 2), num(p[(sex, band)]["unemployed"], 2),
         num(p[(sex, band)]["inactive"], 2), basis, year or ""]
        for c, (p, basis, year) in sorted(status_f.items()) if c in universe
        for sex in SEXES for band in STATUS_BANDS
    ]
    rows_edu = [
        [c, sex, band] + [num(p[(sex, band, n)], 2) for _, n in EDU_LEVELS] + [basis, year or ""]
        for c, (p, basis, year) in sorted(educ_f.items()) if c in universe
        for sex in SEXES for band in EDU_BANDS
    ]
    rows_shares = [
        [c, sex, code, num(v, 4), basis, year or ""]
        for c, (p, basis, year) in sorted(shares_f.items()) if c in universe
        for (sex, code), v in sorted(p.items())
    ]
    rows_factors = []
    for c in sorted(universe):
        p, basis, year = age_ff[c]
        for (m, band), f in sorted(p.items()):
            if abs(f - 1.0) >= FACTOR_QUIET:
                rows_factors.append([c, m, "age_band", band, num(f, 3), basis, year or ""])
        p, basis, year = edu_ff[c]
        for (m, level), f in sorted(p.items()):
            if abs(f - 1.0) >= FACTOR_QUIET:
                rows_factors.append([c, m, "education", level, num(f, 3), basis, year or ""])
    rows_inactivity = [[r["band"], r["student"], r["retired"], r["other"]] for r in inactivity]
    rows_isco = [[r["code"], r["label"], r["major"], r["skill"]] for r in isco]

    for tier in ("lite", "full"):
        print(tier)
        d = out / tier
        write(d / "status.tsv", ["country_code", "sex", "band", "employed", "unemployed", "inactive", "basis", "year"], rows_status)
        write(d / "inactivity.tsv", ["band", "student", "retired", "other"], rows_inactivity)
        write(d / "education.tsv", ["country_code", "sex", "band", "less_than_basic", "basic", "intermediate", "advanced", "basis", "year"], rows_edu)
        write(d / "isco.tsv", ["code", "label", "major", "skill"], rows_isco)
        write(d / "shares.tsv", ["country_code", "sex", "code", "share", "basis", "year"], rows_shares)
        write(d / "factors.tsv", ["country_code", "major", "dimension", "value", "factor", "basis", "year"], rows_factors)
        jobs = [r for r in catalogue if tier == "full" or r["tier"] == "lite"]
        write(d / "occupations.tsv", ["key", "label", "isco", "unit", "weight", "min_age", "max_age", "tags", "skills", "gear"],
              [[r["key"], r["label"], r["isco"], r["unit"], r["weight"], r["min_age"], r["max_age"], r["tags"], r["skills"], r["gear"]]
               for r in jobs])
        lite_codes = {r["isco"] for r in jobs}
        gone = sorted(present - lite_codes)
        if gone:
            sys.exit(f"{tier}: no job for ISCO groups {gone}")


if __name__ == "__main__":
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    main(sys.argv[1], *(sys.argv[2:] or []))
