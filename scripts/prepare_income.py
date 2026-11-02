"""Build the income tables: income.tsv, income_effects.tsv and pay.tsv (both tiers).

Method: designs/occupation-income.md, sections 4, 5 and 7. In short:

  level     ILOSTAT median monthly earnings of employees by sex (EAR_EMTM_SEX_NB_A),
            in the country's current currency (the T30 currency note; other
            currencies through ILOSTAT's USD value and the WDI exchange rate),
            uprated to PRICE_YEAR by the CPI, x 12; checked against GDP per capita
  effects   the medians by ISCO major group, 10-year age band and education,
            fitted log-additively to the library's own joint distribution of
            the employed (backfitting), so the marginals hold without counting
            the overlap of occupation and education twice
  sigma     from the Gini by major group (lognormal), less the variance the
            effects and pay already explain inside the group; then scaled by
            one factor per country (K_RANGE) so the total Gini of the mixture
            the library draws is the measured one (any country with one)
  fill      countries without a usable survey: effects, sigma and the sex gap
            from the subregion (else region, else world); the level from GDP
            per capita times the subregion's ratio of earnings to it
  curated   resources/raw/income_level_curated.tsv: a national office's median
            by sex for a country ILOSTAT lacks; effects and sigma as in fill.
            resources/raw/income_anchor_curated.tsv: a GDP per capita WDI lacks
  pay       resources/raw/pay_anchors.tsv: a country with its own anchor uses
            it; every other country the pooled structure, shrunk by the mean
            leave-one-out slope

Inputs: raw_data/ (fetch_ilostat.py, fetch_worldbank.py), resources/raw/pay_anchors.tsv
(prepare_pay.py), resources/<tier>/ (prepare_occupations.py, run first) and
country-generator's resources/full/{countries,locales,currencies}.tsv.

Usage: python prepare_income.py COUNTRY_GENERATOR_RESOURCES_FULL [--check]
"""
import collections
import csv
import math
import re
import statistics as st
import sys
from pathlib import Path

from prepare_occupations import RAW_DIR, num, raw, read_tsv, write

ROOT = Path(__file__).resolve().parent.parent
PRICE_YEAR = 2025
MIN_YEAR = 2010
MAX_UPRATE = 3.0                    # a larger cumulative CPI factor rejects the survey (4.2)
RATIO_RANGE = (0.2, 3.0)            # annual median / GDP per capita (4.3)
SIGMA_FLOOR, SIGMA_MAX = 0.15, 3.0
REDENOMINATIONS = {("SLL", "SLE"): 1e-3, ("STD", "STN"): 1e-3}   # statutory, 4.1
WB_CODE = {"XKX": "KOS"}            # WDI's Kosovo, keyed like ILOSTAT's
WB_NOT_LCU = {"PSE"}                # WDI reports Palestine in USD, not in the currency the locale names
MAX_GAPS = 2                        # missing CPI years a GDP anchor may fill with the world median
MAJORS = "1234567890"
AGE10 = ["15-24", "25-34", "35-44", "45-54", "55-64", "65+"]
EDU = [("LTB", "less_than_basic"), ("BAS", "basic"), ("INT", "intermediate"), ("ADV", "advanced")]
EDU_NAMES = [n for _, n in EDU]
SEXES = (("SEX_M", "male"), ("SEX_F", "female"))


def read_comment_tsv(path):
    with open(path, encoding="utf-8", newline="") as f:
        return list(csv.DictReader((l for l in f if not l.startswith("#")), delimiter="\t"))


def sigma_of_gini(g):
    return math.sqrt(2) * st.NormalDist().inv_cdf((g + 1) / 2)


def age10_of(band5):
    lo = 65 if band5 == "65+" else int(band5.split("-")[0])
    return AGE10[min((lo - 15) // 10, 5)]


def age_class_of(band10):
    return {"15-24": "15-24", "55-64": "55-64", "65+": "65+"}.get(band10, "25-54")


# -- Raw readers --------------------------------------------------------------------------------


def surveys(indicator, keep=lambda r: True):
    """{(area, year, source): [rows]} for rows with a positive value since MIN_YEAR."""
    g = collections.defaultdict(list)
    for r in raw(indicator):
        if int(r["time"]) >= MIN_YEAR and float(r["value"]) > 0 and keep(r):
            g[(r["ref_area"], int(r["time"]), r["source"])].append(r)
    return g


def latest_complete(groups, need):
    """{area: (year, source, {key: value})}: the latest survey whose rows cover every key in need."""
    best = {}
    for (area, year, source), rows in groups.items():
        cells = {(r["sex"], r["classif1"]): float(r["value"]) for r in rows}
        if need(cells) and (area not in best or (year, len(cells)) > (best[area][0], len(best[area][2]))):
            best[area] = (year, source, cells, rows[0].get("note", ""))
    return best


def wb(series):
    out = collections.defaultdict(dict)
    for r in read_tsv(RAW_DIR / f"wb_{series}.tsv"):
        out[WB_CODE.get(r["ref_area"], r["ref_area"])][int(r["year"])] = float(r["value"])
    return out


# -- Currency and price year (4.1, 4.2) ---------------------------------------------------------


class Money:
    def __init__(self, cg):
        notes = {r["note"]: r["label"] for r in read_tsv(RAW_DIR / "ilostat_notes.tsv")}
        self.t30 = {k: m.group(1) for k, v in notes.items() if k.startswith("T30:")
                    for m in [re.search(r"\(([A-Z]{3})\)\s*$", v)] if m}
        self.fx = wb("PA.NUS.FCRF")
        self.usd = {}
        for (area, year, source), rows in surveys("EAR_EMTM_SEX_OCU_CUR_NB_A").items():
            v = {r["classif2"]: float(r["value"]) for r in rows if r["classif1"] == "OCU_ISCO08_TOTAL"} or \
                {r["classif2"]: float(r["value"]) for r in rows}
            if "CUR_TYPE_LCU" in v and "CUR_TYPE_USD" in v:
                self.usd[(area, year, source)] = v["CUR_TYPE_USD"] / v["CUR_TYPE_LCU"]
        self.cpi = collections.defaultdict(dict)
        for r in raw("CPI_NCYR_COI_RT_A"):
            self.cpi[r["ref_area"]][int(r["time"])] = float(r["value"])

    def currency_of(self, note):
        found = [self.t30[n] for n in note.split("_") if n in self.t30]
        return found[0] if found else None

    def factor(self, area, year, source, note, target, iso3_to_cca2):
        """Multiplier from the survey's currency to `target`, or (None, reason)."""
        cur = self.currency_of(note)
        if cur is None:
            return None, "no currency note"
        if cur == target:
            return 1.0, "same"
        if (cur, target) in REDENOMINATIONS:
            return REDENOMINATIONS[(cur, target)], f"{cur}->{target} redenomination"
        ratio = self.usd.get((area, year, source))
        rate = self.fx.get(area, {}).get(year) or self.fx.get(iso3_to_cca2.get(area, ""), {}).get(year)
        if ratio and rate:
            return ratio * rate, f"{cur}->{target} through USD"
        return None, f"{cur} is not {target} and cannot be converted"

    def uprate(self, area, year, fill=None):
        """Cumulative CPI factor from `year` to PRICE_YEAR, or None if the CPI has gaps.

        fill: {year: percent} used for missing years (a GDP anchor only, at most MAX_GAPS of them)."""
        c = self.cpi.get(area, {})
        f, gaps = 1.0, 0
        for y in range(year + 1, PRICE_YEAR + 1):
            if y not in c:
                if y == PRICE_YEAR and y - 1 in c:   # the latest year not published yet: repeat the last
                    f *= 1 + c[y - 1] / 100
                    continue
                if fill is None or y not in fill or gaps >= MAX_GAPS:
                    return None
                gaps += 1
                f *= 1 + fill[y] / 100
                continue
            f *= 1 + c[y] / 100
        return f

    def world_median(self):
        """{year: median CPI change over every country}, for gaps in a GDP anchor's uprating."""
        by = collections.defaultdict(list)
        for series in self.cpi.values():
            for y, v in series.items():
                by[y].append(v)
        return {y: st.median(v) for y, v in by.items()}


# -- Joint distribution of the employed (5) -----------------------------------------------------


class Joint:
    """P(sex, age10, education, major) among the employed, as get_occupation draws them."""

    def __init__(self, tier_dir):
        self.status = collections.defaultdict(dict)
        for r in read_tsv(tier_dir / "status.tsv"):
            self.status[r["country_code"]][(r["sex"], r["band"])] = float(r["employed"])
        self.edu = collections.defaultdict(dict)
        for r in read_tsv(tier_dir / "education.tsv"):
            self.edu[r["country_code"]][(r["sex"], r["band"])] = {n: float(r[n]) for n in EDU_NAMES}
        self.share = collections.defaultdict(lambda: collections.defaultdict(float))
        self.group = collections.defaultdict(lambda: collections.defaultdict(float))   # (sex, ISCO level 2)
        for r in read_tsv(tier_dir / "shares.tsv"):
            self.share[r["country_code"]][(r["sex"], r["code"][0])] += float(r["share"])
            self.group[r["country_code"]][(r["sex"], r["code"])] += float(r["share"])
        self.factor = collections.defaultdict(dict)
        for r in read_tsv(tier_dir / "factors.tsv"):
            self.factor[r["country_code"]][(r["major"], r["dimension"], r["value"])] = float(r["factor"])
        self.pop = collections.defaultdict(dict)   # working-age population by sex and 5-year band, latest survey
        for (area, year, source), rows in surveys("POP_XWAP_SEX_AGE_NB_A").items():
            cur = self.pop.get(area)
            if cur is None or year > cur.get("_year", 0):
                d = {"_year": year}
                for r in rows:
                    b = r["classif1"].removeprefix("AGE_5YRBANDS_")
                    b = "65+" if b == "YGE65" else b[1:] if b[1:2].isdigit() else None
                    if b:
                        d[(SEX_CODE[r["sex"]], b)] = float(r["value"])
                self.pop[area] = d

    def cells(self, c, area):
        """{(sex, age10, edu, major): weight}."""
        pop = self.pop.get(area, {})
        out = collections.defaultdict(float)
        for (sex, band5), rate in self.status[c].items():
            w_age = rate / 100 * pop.get((sex, band5), 1.0)
            a10 = age10_of(band5)
            ed = self.edu[c].get((sex, a10))
            if not ed or w_age <= 0:
                continue
            tot_e = sum(ed.values())
            for e, pe in ed.items():
                if pe <= 0:
                    continue
                ws = {}
                for m in MAJORS:
                    s = self.share[c].get((sex, m), 0.0)
                    if s > 0:
                        ws[m] = (s * self.factor[c].get((m, "age_band", age_class_of(a10)), 1.0)
                                 * self.factor[c].get((m, "education", e), 1.0))
                tot_m = sum(ws.values())
                for m, wm in ws.items():
                    out[(sex, a10, e, m)] += w_age * pe / tot_e * wm / tot_m
        return out


SEX_CODE = dict(SEXES)
BIN = 0.02   # log width of the grid the mixture median is solved on


def pay_structure(joint, c, jobs, pay):
    """{sex: {major: [(weight, log pay)]}}: the jobs a person of each sex draws within a major group.

    As the library draws them: the group by the sex's share, the job by its weight. Log pay is
    centred as the library normalises it, over both sexes' shares (ocg::normalise_pay_)."""
    groups = joint.group.get(c, {})
    both = collections.defaultdict(float)
    for (_, g), v in groups.items():
        both[g] += v
    if not any(v > 0 for v in both.values()):
        both = collections.defaultdict(lambda: 1.0)
    by_group = collections.defaultdict(list)
    for r in jobs:
        if float(r["weight"]) > 0:
            by_group[r["isco"]].append((float(r["weight"]), math.log(pay.get(r["unit"], 1.0))))
    centre = collections.defaultdict(lambda: [0.0, 0.0])
    for g, js in by_group.items():
        tot = sum(w for w, _ in js)
        for w, lp in js:
            a = centre[g[0]]
            a[0] += both[g] * w / tot
            a[1] += both[g] * w / tot * lp
    out = {}
    for sex in ("male", "female"):
        acc = collections.defaultdict(list)
        for g, js in by_group.items():
            share = groups.get((sex, g), 0.0) if groups else 1.0
            if share <= 0:
                continue
            tot = sum(w for w, _ in js)
            m = g[0]
            mean = centre[m][1] / centre[m][0] if centre[m][0] else 0.0
            acc[m] += [(share * w / tot, lp - mean) for w, lp in js]
        for m, js in acc.items():
            tot = sum(w for w, _ in js)
            acc[m] = [(w / tot, lp) for w, lp in js]
        out[sex] = dict(acc)
    return out


def mixture_median(cells, sex, eff, sigma, struct):
    """log median of income for a sex when mu is 0: cells x jobs x lognormal noise."""
    bins = collections.defaultdict(lambda: [0.0, 0.0])
    for k, w in cells.items():
        if k[0] != sex:
            continue
        x = sum(eff.get(d, {}).get(k[d], 0.0) for d in (1, 2, 3))
        for wj, lp in struct.get(k[3], [(1.0, 0.0)]):
            v = x + lp
            b = bins[(k[3], round(v / BIN))]
            b[0] += w * wj
            b[1] += w * wj * v
    W = sum(a for a, _ in bins.values())
    comps = [(a / W, b / a, sigma.get(k[0], SIGMA_FLOOR) * math.sqrt(2)) for k, (a, b) in bins.items() if a > 0]
    lo = min(x for _, x, _ in comps) - 4 * SIGMA_MAX
    hi = max(x for _, x, _ in comps) + 4 * SIGMA_MAX
    for _ in range(50):
        t = (lo + hi) / 2
        if sum(w * math.erfc((x - t) / sd) for w, x, sd in comps) / 2 < 0.5:
            lo = t
        else:
            hi = t
    return (lo + hi) / 2


def solve_mu(cells, eff, sigma, struct, target):
    """{sex: e^mu} so that each sex's median income is target[sex]."""
    return {sex: target[sex] / math.exp(mixture_median(cells, sex, eff, sigma, struct[sex]))
            for sex in ("male", "female")}


# -- Total Gini of the mixture, and the spread calibrated to it (5.1) ---------------------------

GH = 0.025              # log width of the grid the Gini is integrated on
Z_BOUND = 3.5           # the library clamps the normal draw to +-this (occupationgen.hpp, income_z_bound_)
K_RANGE = (0.5, 2.0)    # bounds of the per-country factor on sigma


def _cdf(hist, sig):
    """(lo, F): the unnormalised CDF of log income on the lattice GH*j, j >= lo.

    hist {major: {bin: weight}} are the cell-and-job medians; each spreads by a normal of
    the group's sigma, clamped at +-Z_BOUND as the library draws it."""
    reach = {m: math.ceil(Z_BOUND * sig[m] / GH) for m in hist}
    lo = min(min(b) - reach[m] for m, b in hist.items()) - 1
    hi = max(max(b) + reach[m] for m, b in hist.items()) + 1
    n = hi - lo + 1
    F, step = [0.0] * n, [0.0] * (n + 1)
    for m, bins in hist.items():
        L, s = reach[m], sig[m]
        ker = [0.5 * math.erfc(-d * GH / (s * math.sqrt(2))) if abs(d * GH) < Z_BOUND * s else float(d > 0)
               for d in range(-L, L + 1)]
        for b, w in bins.items():
            base = b - lo - L
            for i, kv in enumerate(ker):
                F[base + i] += w * kv
            step[b - lo + L + 1] += w
    acc = 0.0
    for j in range(n):
        acc += step[j]
        F[j] += acc
    return lo, F


def mixture_gini(cells, eff, sigma, struct, target):
    """Gini of income over everyone employed, both sexes, when each sex's median is target[sex].

    Gini = 1 - E[min(X, X')] / E[X], with E[min] = integral of (1 - F)^2 and E[X] of (1 - F)."""
    per_sex = []
    for sex in ("male", "female"):
        hist = collections.defaultdict(lambda: collections.defaultdict(float))
        for k, w in cells.items():
            if k[0] == sex:
                x = sum(eff.get(d, {}).get(k[d], 0.0) for d in (1, 2, 3))
                for wj, lp in struct[sex].get(k[3], [(1.0, 0.0)]):
                    hist[k[3]][round((x + lp) / GH)] += w * wj
        if not hist:
            continue
        lo, F = _cdf(hist, {m: sigma.get(m, SIGMA_FLOOR) for m in hist})
        W = F[-1]
        j = next(i for i, f in enumerate(F) if f >= W / 2)
        f0 = F[j - 1] / W if j else 0.0
        med = lo + j - 1 + (0.5 - f0) / (F[j] / W - f0)
        shift = round(math.log(target[sex]) / GH - med)
        per_sex.append((lo + shift, [f / W for f in F], W))
    lo = min(l for l, _, _ in per_sex)
    hi = max(l + len(F) for l, F, _ in per_sex)
    Wt = sum(W for _, _, W in per_sex)
    F = [sum(W * (Fs[j - l] if l <= j < l + len(Fs) else float(j >= l)) for l, Fs, W in per_sex) / Wt
         for j in range(lo, hi)]
    mean = sq = math.exp(lo * GH)   # from 0 to the grid, where F = 0
    for i in range(len(F) - 1):
        dy = math.exp((lo + i + 1) * GH) - math.exp((lo + i) * GH)
        mean += (2 - F[i] - F[i + 1]) / 2 * dy
        sq += ((1 - F[i]) ** 2 + (1 - F[i + 1]) ** 2) / 2 * dy
    return 1 - sq / mean


def calibrate_sigma(cells, eff, sigma, struct, target, gini):
    """(sigma x k, k): one factor on every group's sigma so the mixture's total Gini is `gini`.

    k is bounded by K_RANGE (and each sigma by SIGMA_FLOOR, SIGMA_MAX); at a bound the Gini
    is as close as it gets. Illinois false position on k."""
    def scaled(k):
        return {m: min(SIGMA_MAX, max(SIGMA_FLOOR, k * v)) for m, v in sigma.items()}

    def f(k):
        return mixture_gini(cells, eff, scaled(k), struct, target) - gini

    a, b = K_RANGE
    fa, fb = f(a), f(b)
    if fa >= 0:
        return scaled(a), a
    if fb <= 0:
        return scaled(b), b
    side = 0
    for _ in range(30):
        k = (a * fb - b * fa) / (fb - fa)
        fk = f(k)
        if abs(fk) < 2e-4:
            break
        if fk * fb > 0:
            b, fb = k, fk
            if side == 1:
                fa /= 2
            side = 1
        else:
            a, fa = k, fk
            if side == -1:
                fb /= 2
            side = -1
    return scaled(k), k


def backfit(cells, targets, sex_offset):
    """Log-additive effects whose weighted cell means reproduce each marginal target.

    targets: {dim_index: {level: log(median_level / median_total)}}; dims 1 = age10, 2 = edu, 3 = major.
    sex_offset: {sex: log level}, held fixed: the library adds each sex's level, so a group of mostly
    men must not take the sex gap into its own effect as well.
    Returns ({dim: {level: effect}}, worst residual)."""
    keys, ws = list(cells), list(cells.values())
    W = sum(ws)
    # ILOSTAT's medians describe employees, the cells everyone employed: only the differences
    # between levels are kept, so each dimension's targets are centred on the cells' weights.
    centred = {}
    for d, t in targets.items():
        acc = collections.defaultdict(float)
        for k, w in zip(keys, ws):
            if k[d] in t:
                acc[k[d]] += w
        tw = sum(acc.values())
        shift = sum(acc[l] * t[l] for l in acc) / tw if tw else 0.0
        centred[d] = {l: v - shift for l, v in t.items()}
    targets = centred
    eff = {d: {l: 0.0 for l in t} for d, t in targets.items()}

    def totals():
        return [sex_offset.get(k[0], 0.0) + sum(eff[d].get(k[d], 0.0) for d in eff) for k in keys]

    def by_level(d, tot):
        """{level: (weight, weighted mean of tot)} over the cells of dimension d."""
        acc = collections.defaultdict(lambda: [0.0, 0.0])
        for k, w, t in zip(keys, ws, tot):
            acc[k[d]][0] += w
            acc[k[d]][1] += w * t
        return {l: (a, b / a) for l, (a, b) in acc.items() if a > 0}

    for _ in range(100):
        change = 0.0
        for d, t in targets.items():
            tot = totals()
            overall = sum(w * x for w, x in zip(ws, tot)) / W
            for l, (_, mean) in by_level(d, tot).items():
                if l in t:
                    new = t[l] + overall - (mean - eff[d][l])
                    change = max(change, abs(new - eff[d][l]))
                    eff[d][l] = new
        if change < 1e-7:
            break
    tot = totals()
    overall = sum(w * x for w, x in zip(ws, tot)) / W
    worst = max((abs(mean - overall - targets[d][l]) for d in targets
                 for l, (_, mean) in by_level(d, tot).items() if l in targets[d]), default=0.0)
    for d in eff:   # centre each dimension on the employed
        m = sum(w * eff[d].get(k[d], 0.0) for k, w in zip(keys, ws)) / W
        eff[d] = {l: v - m for l, v in eff[d].items()}
    return eff, worst


# -- Pay (7.3) ----------------------------------------------------------------------------------


def build_pay(units):
    """{anchor: {unit: pay}} for the anchors and 'pooled', centred within each major group."""
    src = collections.defaultdict(dict)
    for r in read_comment_tsv(ROOT / "resources" / "raw" / "pay_anchors.tsv"):
        if r["unit"] in units:
            src[r["anchor"]][r["unit"]] = math.log(float(r["value"]))
    names = sorted(src)
    common = [u for u in units if all(u in src[a] for a in names)]
    ref = {a: {} for a in names}   # each anchor's mean over the common units of a major group
    for a in names:
        by = collections.defaultdict(list)
        for u in common:
            by[u[0]].append(src[a][u])
        ref[a] = {m: st.mean(v) for m, v in by.items()}
    cen = {a: {u: v - ref[a][u[0]] for u, v in src[a].items() if u[0] in ref[a]} for a in names}
    betas = []
    for t in names:   # leave one out: the slope of each anchor on the mean of the others
        xs, ys = [], []
        for u in common:
            xs.append(st.mean(cen[a][u] for a in names if a != t))
            ys.append(cen[t][u])
        betas.append(sum(x * y for x, y in zip(xs, ys)) / sum(x * x for x in xs))
        print(f"  pay: {t} on the others: r={st.correlation(xs, ys):.2f} beta={betas[-1]:.2f}")
    beta = st.mean(betas)
    pooled = {}
    for u in units:
        vs = [cen[a][u] for a in names if u in cen[a]]
        if len(vs) >= 2:
            pooled[u] = beta * st.mean(vs)
    out = {a: {u: math.exp(v) for u, v in cen[a].items()} for a in names}
    out["pooled"] = {u: math.exp(v) for u, v in pooled.items()}
    print(f"  pay: {len(common)} units in every anchor, mean beta {beta:.2f}, pooled covers {len(pooled)} of {len(units)}")
    return out


# -- Main ---------------------------------------------------------------------------------------


def main(cg_dir, check=False):
    cg = Path(cg_dir)
    countries = read_tsv(cg / "countries.tsv")
    iso3 = {c["cca3"]: c["cca2"] for c in countries}
    iso3["KOS"] = iso3.get("UNK", "XK")
    to3 = {v: k for k, v in iso3.items()}
    place_of = {c["cca2"]: tuple(x for x in (c["subregion"], c["region"], "world") if x) for c in countries}
    currency = {}
    for r in read_comment_tsv(cg / "locales.tsv"):
        currency.setdefault(r["country"], r["currency_code"])
    digits = {r["code"]: int(r["digits"]) for r in read_comment_tsv(cg / "currencies.tsv")}

    full = ROOT / "resources" / "full"
    universe = sorted({r["country_code"] for r in read_tsv(full / "status.tsv")})
    money = Money(cg)
    gdp = wb("NY.GDP.PCAP.CN")
    def read_curated(name):
        rows = read_comment_tsv(ROOT / "resources" / "raw" / name)
        for r in rows:
            if r["currency"] != currency.get(r["country_code"]):
                sys.exit(f"{name}: {r['country_code']} is in {r['currency']}, "
                         f"its locale currency is {currency.get(r['country_code'])}")
        return rows

    for r in read_curated("income_anchor_curated.tsv"):
        gdp[to3.get(r["country_code"], r["country_code"])][int(r["year"])] = float(r["gdp_per_capita"])
    # A measured median from a national office: the level only (no survey effects to fit)
    curated = {r["country_code"]: dict(year=int(r["year"]),
                                       **{s: float(r[f"median_{s}"]) * float(r["uprate"]) for s in ("male", "female")})
               for r in read_curated("income_level_curated.tsv")}

    world_cpi = money.world_median()

    def gdp_2025(area):
        if area in WB_NOT_LCU:
            return None
        g = gdp.get(area) or gdp.get(iso3.get(area, ""), {})
        if not g:
            return None
        y = max(g)
        if y < 2019:
            return None
        f = money.uprate(area, y, fill=world_cpi)
        return g[y] * f if f else None

    # Level per sex (4.1 to 4.3)
    med = latest_complete(surveys("EAR_EMTM_SEX_NB_A"), lambda c: all((s, "") in c for s in ("SEX_M", "SEX_F", "SEX_T")))
    levels, report = {}, []
    for area, (year, source, cells, note) in sorted(med.items()):
        c = iso3.get(area)
        if c not in universe:
            continue
        k, why = money.factor(area, year, source, note, currency.get(c), iso3)
        if k is None:
            report.append((c, "rejected", why))
            continue
        up = money.uprate(area, year)
        if up is None or up > MAX_UPRATE:
            report.append((c, "rejected", f"CPI from {year}: {'gaps' if up is None else f'x{up:.1f}'}"))
            continue
        annual = {sex: cells[(code, "")] * k * up * 12 for code, sex in SEXES}
        annual["total"] = cells[("SEX_T", "")] * k * up * 12
        g = gdp_2025(area)
        ratio = annual["total"] / g if g else None
        if ratio is not None and not (RATIO_RANGE[0] <= ratio <= RATIO_RANGE[1]):
            report.append((c, "rejected", f"earnings / GDP per capita = {ratio:.3g} ({why})"))
            continue
        levels[c] = dict(annual, year=year, basis="ilostat" if why == "same" else "converted", ratio=ratio)
        if why != "same":
            report.append((c, "converted", why))

    # Relative effects, Gini (currency-free)
    def rel(indicator, prefix, keyf):
        g = latest_complete(surveys(indicator, lambda r: r["sex"] == "SEX_T"),
                            lambda c: any(k[1].endswith(("TOTAL", "YGE15")) for k in c))
        out = {}
        for area, (year, _, cells, _) in g.items():
            tot = next((v for (s, k), v in cells.items() if k.endswith(("TOTAL", "YGE15")) and k.startswith(prefix)), None)
            if tot:
                d = {keyf(k): math.log(v / tot) for (s, k), v in cells.items() if k.startswith(prefix) and keyf(k)}
                if len(d) >= 3:
                    out[iso3.get(area)] = (year, d)
        return out

    occ = rel("EAR_EMTM_SEX_OCU_NB_A", "OCU_ISCO08_", lambda k: k[-1] if k[-1].isdigit() else None)
    age = rel("EAR_EMTM_SEX_AGE_NB_A", "AGE_10YRBANDS_",
              lambda k: {"Y15-24": "15-24", "Y25-34": "25-34", "Y35-44": "35-44", "Y45-54": "45-54",
                         "Y55-64": "55-64", "YGE65": "65+"}.get(k.removeprefix("AGE_10YRBANDS_")))
    edu = rel("EAR_EMTM_SEX_EDU_NB_A", "EDU_AGGREGATE_", lambda k: dict(EDU).get(k.removeprefix("EDU_AGGREGATE_")))

    def gini_table(indicator, keyf):
        g = latest_complete(surveys(indicator, lambda r: float(r["value"]) < 1), lambda c: True)
        return {iso3.get(a): {keyf(s, k): sigma_of_gini(v) for (s, k), v in cells.items() if keyf(s, k)}
                for a, (_, _, cells, _) in g.items()}

    sig_occ = gini_table("EAR_EMTG_SEX_OCU_NB_A", lambda s, k: k[-1] if s == "SEX_T" and k[-1].isdigit() else None)
    sig_all = gini_table("EAR_EMTG_SEX_NB_A", lambda s, k: "total" if s == "SEX_T" else None)
    gini_total = {iso3.get(a): cells[("SEX_T", "")]
                  for a, (_, _, cells, _) in latest_complete(surveys("EAR_EMTG_SEX_NB_A", lambda r: float(r["value"]) < 1),
                                                             lambda c: ("SEX_T", "") in c).items()}

    def calibrate(c, cells, eff, sigma, struct, target):
        """5.1: every country with a measured total Gini has its sigma scaled to meet it."""
        if c not in gini_total or not sigma or not cells:
            return sigma
        sigma, k = calibrate_sigma(cells, eff, sigma, struct, target, gini_total[c])
        if k in K_RANGE:
            g = mixture_gini(cells, eff, sigma, struct, target)
            report.append((c, "gini", f"sigma x {k} (bound): {g:.3f} against {gini_total[c]:.3f}"))
        return sigma

    # Pay first: sigma removes the variance pay explains within each group
    catalogue = {tier: read_tsv(ROOT / "resources" / tier / "occupations.tsv") for tier in ("lite", "full")}
    units = sorted({r["unit"] for r in catalogue["full"]})
    pay = build_pay(set(units))
    own = {"AU", "US", "BR", "CO"} & set(pay)

    joint = Joint(full)
    fitted = {}
    for c in sorted(levels):
        area = to3.get(c, c)
        cells = joint.cells(c, area)
        if not cells:
            report.append((c, "rejected", "no joint distribution"))
            continue
        targets = {}
        for d, t in ((1, age), (2, edu), (3, occ)):
            if c in t:
                targets[d] = {l: v for l, v in t[c][1].items()}
        offset = {sex: math.log(levels[c][sex]) for sex in ("male", "female")}
        eff, worst = backfit(cells, targets, offset) if targets else ({}, 0.0)
        if worst > 0.01:
            report.append((c, "fit", f"worst marginal off by {worst:.3f} in logs"))
        # sigma by major group: the measured spread less what age, education and pay explain
        anchor = c if c in own else "pooled"
        struct = pay_structure(joint, c, catalogue["full"], pay[anchor])
        sigma = {}
        base = sig_occ.get(c, {})
        for m in MAJORS:
            s0 = base.get(m) or sig_all.get(c, {}).get("total")
            if s0 is None:
                continue
            sub = [(w, eff.get(1, {}).get(k[1], 0) + eff.get(2, {}).get(k[2], 0)) for k, w in cells.items() if k[3] == m]
            v_exp = 0.0
            if sub:
                W = sum(w for w, _ in sub)
                mu = sum(w * x for w, x in sub) / W
                v_exp = sum(w * (x - mu) ** 2 for w, x in sub) / W
            jw = struct["male"].get(m, []) + struct["female"].get(m, [])
            if jw:
                W = sum(w for w, _ in jw)
                v_exp += sum(w * x * x for w, x in jw) / W
            sigma[m] = min(SIGMA_MAX, max(SIGMA_FLOOR, math.sqrt(max(0.0, s0 * s0 - v_exp))))
        sigma = calibrate(c, cells, eff, sigma, struct, levels[c])
        # mu per sex: each sex's median over everything the library draws is the measured one
        mu = solve_mu(cells, eff, sigma, struct, levels[c])
        fitted[c] = dict(levels[c], eff=eff, sigma=sigma, mu=mu)

    # Fill (4.4): effects, sigma and the sex gap from the place; level from GDP per capita
    by_place = collections.defaultdict(list)
    for c, f in fitted.items():
        for p in place_of.get(c, ()):
            by_place[p].append(f)

    def place_mean(c, get):
        for p in place_of.get(c, ()):
            vals = [v for f in by_place.get(p, ()) if (v := get(f)) is not None]
            if vals:
                return st.mean(vals)
        return None

    rows_income, rows_eff, rows_target = [], [], []
    for c in universe:
        cur = currency.get(c)
        if not cur or cur not in digits:
            report.append((c, "not served", f"no current currency ({cur})"))
            continue
        if c in fitted:
            f = fitted[c]
            mu, eff, sigma, basis, year = f["mu"], f["eff"], f["sigma"], f["basis"], f["year"]
            target = {sex: f[sex] for sex in ("male", "female")}
        else:
            g = gdp_2025(to3.get(c, c))
            if c in curated:
                target = {sex: curated[c][sex] for sex in ("male", "female")}
                basis, year = "curated", curated[c]["year"]
                if g:   # the same check a survey gets (4.3)
                    ratio = math.sqrt(target["male"] * target["female"]) / g
                    if not (RATIO_RANGE[0] <= ratio <= RATIO_RANGE[1]):
                        sys.exit(f"income_level_curated.tsv: {c} earnings / GDP per capita = {ratio:.3g}")
                    report.append((c, "curated", f"national median, earnings / GDP per capita = {ratio:.3g}"))
            else:
                ratio = place_mean(c, lambda f: math.log(f["ratio"]) if f.get("ratio") else None)
                if g is None or ratio is None:
                    report.append((c, "not served", "no survey and no GDP per capita"))
                    continue
                total = g * math.exp(ratio)
                gap = place_mean(c, lambda f: math.log(f["female"] / f["male"]))
                target = {"male": total * math.exp(-gap / 2), "female": total * math.exp(gap / 2)}
                basis, year = "gdp", ""
            eff = {d: {} for d in (1, 2, 3)}
            for d, levels_ in ((1, AGE10), (2, EDU_NAMES), (3, list(MAJORS))):
                for l in levels_:
                    v = place_mean(c, lambda f: f["eff"].get(d, {}).get(l))
                    if v is not None:
                        eff[d][l] = v
            sigma = {m: v for m in MAJORS if (v := place_mean(c, lambda f: f["sigma"].get(m))) is not None}
            cells = joint.cells(c, to3.get(c, c))
            struct = pay_structure(joint, c, catalogue["full"], pay[c if c in own else "pooled"])
            sigma = calibrate(c, cells, eff, sigma, struct, target)
            mu = solve_mu(cells, eff, sigma, struct, target) if cells else target
        anchor = c if c in own else "pooled"
        rows_target.append([c, f"{target['male']:.6g}", f"{target['female']:.6g}"])
        rows_income.append([c, cur, digits[cur], f"{mu['male']:.0f}", f"{mu['female']:.0f}", anchor, basis, year])
        for d, name in ((3, "major"), (1, "age_band"), (2, "education")):
            for l, v in sorted(eff.get(d, {}).items()):
                s = num(sigma[l], 3) if d == 3 and l in sigma else ""
                rows_eff.append([c, name, l, num(math.exp(v), 4), s, basis, year])
        for m in MAJORS:   # a group with a sigma but no effect row
            if m in sigma and m not in eff.get(3, {}):
                rows_eff.append([c, "major", m, "1", num(sigma[m], 3), basis, year])

    rows_pay = [[a, u, num(v, 4)] for a in sorted(pay) for u, v in sorted(pay[a].items())]
    counts = collections.Counter(r[6] for r in rows_income)
    print(f"  income: {len(rows_income)} of {len(universe)} countries served {dict(counts)}")
    for c, what, why in sorted(report, key=lambda x: (x[1], x[0])):
        print(f"  {what:10} {c}: {why}")
    if check:
        return
    # what each sex's median was calibrated to, for validation (not shipped)
    write(RAW_DIR / "income_targets.tsv", ["country_code", "male", "female"], rows_target)
    for tier in ("lite", "full"):
        d = ROOT / "resources" / tier
        write(d / "income.tsv", ["country_code", "currency", "digits", "median_male", "median_female", "pay_anchor", "basis", "year"], rows_income)
        write(d / "income_effects.tsv", ["country_code", "dimension", "value", "effect", "sigma", "basis", "year"], rows_eff)
        write(d / "pay.tsv", ["anchor", "unit", "pay"], rows_pay)


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if a != "--check"]
    if len(args) != 1:
        sys.exit(__doc__)
    main(args[0], check="--check" in sys.argv)
