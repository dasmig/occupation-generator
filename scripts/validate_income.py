"""Compare sampled incomes with ILOSTAT's medians and Ginis (doc/data.md, "How well it matches").

Run from scripts/, after prepare_income.py (which writes raw_data/income_targets.tsv):
  python validate_income.py ages ../build/ages.tsv
  ../build/income_sample ../resources/full ../build/ages.tsv 40000 ../build/income_sample.tsv
  python validate_income.py compare ../build/income_sample.tsv ../../country-generator/resources/full
"""
import collections
import math
import statistics as st
import sys

sys.path.insert(0, ".")
import prepare_income as P  # noqa: E402
from prepare_occupations import read_tsv  # noqa: E402


def ages(out):
    countries = read_tsv(P.ROOT.parent / "country-generator" / "resources" / "full" / "countries.tsv")
    iso3 = {c["cca3"]: c["cca2"] for c in countries}
    iso3["KOS"] = iso3.get("UNK", "XK")
    pop = {}
    for (area, year, _), rows in P.surveys("POP_XWAP_SEX_AGE_NB_A").items():
        if area in pop and pop[area][0] >= year:
            continue
        d = {}
        for r in rows:
            b = r["classif1"].removeprefix("AGE_5YRBANDS_")
            if b == "YGE65":
                lo, hi = 65, 79
            elif b[1:2].isdigit():
                lo, hi = (int(x) for x in b[1:].split("-"))
            else:
                continue
            d[(P.SEX_CODE[r["sex"]], lo, hi)] = float(r["value"])
        pop[area] = (year, d)
    with open(out, "w") as f:
        for area, (_, d) in pop.items():
            c = iso3.get(area)
            if not c:
                continue
            for (sex, lo, hi), v in d.items():
                for a in range(lo, hi + 1):
                    f.write(f"{c}\t{sex}\t{a}\t{v / (hi - lo + 1):.6g}\n")


def gini(xs):
    xs = sorted(xs)
    n = len(xs)
    return sum((2 * i - n + 1) * x for i, x in enumerate(xs)) / (n * sum(xs))


def compare(sample, cg):
    by = collections.defaultdict(list)
    with open(sample) as f:
        next(f)
        for line in f:
            c, sex, major, age, edu, inc = line.rstrip("\n").split("\t")
            by[c].append((sex, major, int(age), edu, float(inc)))
    income = {r["country_code"]: r for r in read_tsv(P.ROOT / "resources" / "full" / "income.tsv")}
    # ILOSTAT targets, as prepare_income reads them (survey-currency medians are compared as ratios)
    med = P.latest_complete(P.surveys("EAR_EMTM_SEX_NB_A"), lambda c: all((s, "") in c for s in ("SEX_M", "SEX_F", "SEX_T")))
    occ = P.latest_complete(P.surveys("EAR_EMTM_SEX_OCU_NB_A", lambda r: r["sex"] == "SEX_T"), lambda c: True)
    g = P.latest_complete(P.surveys("EAR_EMTG_SEX_NB_A", lambda r: float(r["value"]) < 1), lambda c: True)
    countries = read_tsv(P.Path(cg) / "countries.tsv")
    to3 = {c["cca2"]: c["cca3"] for c in countries}
    to3["XK"] = "KOS"
    tg = {r["country_code"]: r for r in read_tsv(P.RAW_DIR / "income_targets.tsv")}
    print("cc  basis      med_err(m,f)    sex_gap(s/t)   gini(s/t)   major_err(worst, centred)")
    rows = []
    for c, xs in sorted(by.items()):
        a = to3.get(c, c)
        m = [x[4] for x in xs if x[0] == "male"]
        fm = [x[4] for x in xs if x[0] == "female"]
        if len(m) < 1000 or len(fm) < 1000:
            continue
        gap_s = st.median(fm) / st.median(m)
        em = math.log(st.median(m) / float(tg[c]["male"]))
        ef = math.log(st.median(fm) / float(tg[c]["female"]))
        line = f"{c}  {income[c]['basis']:9}  {em:+.3f} {ef:+.3f}   {gap_s:.3f}"
        if a in med:
            cells = med[a][2]
            gap_t = cells[("SEX_F", "")] / cells[("SEX_M", "")]
            line += f"/{gap_t:.3f}"
        else:
            gap_t = None
            line += "/  -  "
        gs = gini([x[4] for x in xs])
        gt = g[a][2].get(("SEX_T", "")) if a in g else None
        line += f"   {gs:.3f}/{gt:.3f}" if gt else f"   {gs:.3f}/  -  "
        worst = None
        if a in occ:
            cells = occ[a][2]
            tot = cells.get(("SEX_T", "OCU_ISCO08_TOTAL"))
            allmed = st.median([x[4] for x in xs])
            errs = []
            for mj in "123456789":
                t = cells.get(("SEX_T", f"OCU_ISCO08_{mj}"))
                v = [x[4] for x in xs if x[1] == mj]
                if t and tot and len(v) >= 2000:
                    errs.append((len(v), math.log((st.median(v) / allmed) / (t / tot))))
            if errs:
                # only differences between groups are fitted (employees vs everyone employed)
                shift = sum(n * e for n, e in errs) / sum(n for n, _ in errs)
                errs = [e - shift for _, e in errs]
                worst = max(errs, key=abs)
                line += f"   {worst:+.3f} (n={len(errs)})"
        rows.append((c, gap_s, gap_t, gs, gt, worst, em, ef))
        print(line)
    ok = [r for r in rows if r[4]]
    print(f"\ngini: mean |diff| {st.mean(abs(r[3] - r[4]) for r in ok):.3f} over {len(ok)}; "
          f"within 0.03: {sum(abs(r[3] - r[4]) <= 0.03 for r in ok)}")
    ok = [r for r in rows if r[2]]
    print(f"sex gap: mean |log diff| {st.mean(abs(math.log(r[1] / r[2])) for r in ok):.3f} over {len(ok)}")
    errs = [abs(e) for r in rows for e in r[6:8]]
    print(f"sex medians: max |log err| {max(errs):.3f}, within 3%: {sum(e <= 0.03 for e in errs)} of {len(errs)}")
    ok = [r for r in rows if r[5] is not None]
    print(f"major: median |worst log err| {st.median(abs(r[5]) for r in ok):.3f}; within 5%: {sum(abs(r[5]) <= 0.05 for r in ok)} of {len(ok)}")


if __name__ == "__main__":
    if sys.argv[1] == "ages":
        ages(sys.argv[2])
    else:
        compare(sys.argv[2], sys.argv[3])
