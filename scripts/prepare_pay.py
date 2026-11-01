"""Build the pay anchors: earnings by ISCO-08 unit group from national surveys.

Pay within an ISCO major group (designs/occupation-income.md, section 7.3) comes
from surveys that publish earnings by detailed occupation. Each anchor gives a
value per ISCO-08 unit group (4 digits); only relative values within a major
group are used, so the measures (mean or median, week or month) need not agree.

  AU  ABS Employee Earnings and Hours, May 2025, data cube 11 (CC BY 4.0):
      mean weekly total cash earnings by ANZSCO 4-digit, all employees; mapped
      with the ABS ANZSCO v1.3 -> ISCO-08 correspondence (exact matches preferred)
  US  BLS OEWS, May 2024, national (public domain): median annual wage by SOC
      2018; SOC 2018 -> 2010 through O*NET's 2010 -> 2019 taxonomy crosswalk, then
      the BLS ISCO-08 x SOC 2010 crosswalk; bls.gov refuses scripted downloads,
      so national_M2024_dl.xlsx and ISCO_SOC_Crosswalk.xls are fetched by hand
  BR  IBGE PNAD Continua, 2025 Q4 microdata: weighted median of usual monthly
      earnings in the main job (VD4016) of employees (VD4009 01-07) by COD (V4010)
  CO  DANE GEIH 2025, the twelve monthly "Ocupados" files: weighted median of the
      monthly wage (P6500) of employees (P6430 1-3) by CIUO-08 A.C. (OFICIO_C8)

Output: resources/raw/pay_anchors.tsv (anchor, unit, value, n), committed so the
library builds without these downloads. Cells with fewer than MIN_N respondents
are left out (microdata anchors only).

Inputs (raw_data/pay/, ignored by git):
  au/63060DO011_202505.xlsx, au/anzsco13_isco08.xlsx
  us/national_M2024_dl.xlsx, us/ISCO_SOC_Crosswalk.xls, us/onet_2010_to_2019.csv
  br/PNADC_042025.zip
  co/ocupados_2025_MM.csv (twelve)

Requires openpyxl and xlrd. Usage: python prepare_pay.py [RAW_PAY_DIR]
"""
import collections
import csv
import io
import math
import re
import sys
import zipfile
from pathlib import Path

import openpyxl
import xlrd

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "resources" / "raw" / "pay_anchors.tsv"
MIN_N = 150


def gmean(values):
    return math.exp(sum(math.log(v) for v in values) / len(values))


def wmedian(pairs):
    pairs.sort()
    half, acc = sum(w for _, w in pairs) / 2, 0.0
    for v, w in pairs:
        acc += w
        if acc >= half:
            return v


def anchor_au(d):
    earn = {}
    ws = openpyxl.load_workbook(d / "63060DO011_202505.xlsx", read_only=True, data_only=True)["Table_1"]
    for row in ws.iter_rows(values_only=True):
        if row and isinstance(row[0], str) and re.match(r"^\d{4} ", row[0]) and isinstance(row[3], (int, float)):
            earn[row[0][:4]] = float(row[3])  # persons
    exact, partial = collections.defaultdict(set), collections.defaultdict(set)
    ws = openpyxl.load_workbook(d / "anzsco13_isco08.xlsx", read_only=True, data_only=True).worksheets[1]
    for row in ws.iter_rows(values_only=True):
        if not row or row[0] is None or len(row) < 3 or row[2] is None:
            continue
        a, isco = str(row[0]).strip(), str(row[2]).strip()
        if re.fullmatch(r"\d{6}", a) and re.fullmatch(r"\d{4}", isco):
            p = len(row) > 3 and str(row[3]).strip() == "p"
            (partial if p else exact)[isco].add(a[:4])
    out = {}
    for isco in set(exact) | set(partial):
        grp = exact[isco] if any(g in earn for g in exact[isco]) else partial[isco]
        vs = [earn[g] for g in grp if g in earn]
        if vs:
            out[isco] = (gmean(vs), len(vs))
    return out


def anchor_us(d):
    sh = xlrd.open_workbook(d / "ISCO_SOC_Crosswalk.xls").sheet_by_name("2010 SOC to ISCO-08")
    soc2isco = collections.defaultdict(set)
    for i in range(sh.nrows):
        soc, _, _, isco = [str(c).strip() for c in sh.row_values(i)[:4]]
        if re.fullmatch(r"\d\d-\d{4}", soc) and re.fullmatch(r"\d{4}", isco):
            soc2isco[soc].add(isco)
    s18to10 = collections.defaultdict(set)
    with open(d / "onet_2010_to_2019.csv", encoding="utf-8-sig") as f:
        for r in csv.DictReader(f):
            s18to10[r["O*NET-SOC 2019 Code"][:7]].add(r["O*NET-SOC 2010 Code"][:7])

    def isco_of(code):
        socs = s18to10.get(code)
        if socs is None and code.endswith("0"):  # an OEWS aggregate such as 51-2090
            socs = set().union(*[v for k, v in s18to10.items() if k[:6] == code[:6]] or [set()])
        found = set().union(*[soc2isco[s] for s in socs or () if s in soc2isco] or [set()])
        return found or soc2isco.get(code, set())

    rows = list(openpyxl.load_workbook(d / "national_M2024_dl.xlsx", read_only=True).worksheets[0].iter_rows(values_only=True))
    ix = {k: rows[0].index(k) for k in ("OCC_CODE", "O_GROUP", "TOT_EMP", "A_MEDIAN", "H_MEDIAN")}
    acc, total, lost = collections.defaultdict(lambda: [0.0, 0.0]), 0.0, 0.0
    for r in rows[1:]:
        if r[ix["O_GROUP"]] != "detailed" or not isinstance(r[ix["TOT_EMP"]], (int, float)):
            continue
        med = r[ix["A_MEDIAN"]]
        if med == "#":
            med = 239200.0  # top-coded at the survey's ceiling
        elif not isinstance(med, (int, float)):
            h = r[ix["H_MEDIAN"]]
            med = h * 2080 if isinstance(h, (int, float)) else None
        if med is None:
            continue
        emp = float(r[ix["TOT_EMP"]])
        total += emp
        units = isco_of(r[ix["OCC_CODE"]])
        if not units:
            lost += emp
        for u in units:
            w = emp / len(units)
            acc[u][0] += w * math.log(med)
            acc[u][1] += w
    print(f"  US: {100 * lost / total:.1f}% of employment not mapped to ISCO-08")
    return {u: (math.exp(s / w), round(w)) for u, (s, w) in acc.items()}


def anchor_br(d):
    by = collections.defaultdict(list)
    with zipfile.ZipFile(d / "PNADC_042025.zip") as z, z.open(z.namelist()[0]) as fh:
        for line in io.TextIOWrapper(fh, encoding="latin-1"):
            if line[409] != "1" or line[416:418] not in ("01", "02", "03", "04", "05", "06", "07"):
                continue
            occ, inc = line[151:155].strip(), line[426:434].strip()
            if occ and occ != "0000" and inc and float(inc) > 0:
                by[occ].append((float(inc), float(line[49:64])))
    return {k: (wmedian(v), len(v)) for k, v in by.items() if len(v) >= MIN_N}


def anchor_co(d):
    by = collections.defaultdict(list)
    files = sorted(d.glob("ocupados_2025_*.csv"))
    if len(files) != 12:
        sys.exit(f"CO: expected 12 monthly files, found {len(files)}")
    for path in files:
        with open(path, encoding="latin-1", newline="") as f:
            for r in csv.DictReader(f, delimiter=";"):
                occ, wage = (r.get("OFICIO_C8") or "").strip(), (r.get("P6500") or "").strip()
                if r.get("P6430", "").strip() not in ("1", "2", "3") or not occ or not wage:
                    continue
                v = float(wage.replace(",", "."))
                if 0 < v < 98_000_000:  # 98/99 million are "does not know" codes
                    by[occ.zfill(4)].append((v, float(r["FEX_C18"].replace(",", "."))))
    return {k: (wmedian(v), len(v)) for k, v in by.items() if len(v) >= MIN_N}


def main(raw):
    raw = Path(raw)
    rows = []
    for name, fn in (("AU", anchor_au), ("US", anchor_us), ("BR", anchor_br), ("CO", anchor_co)):
        a = fn(raw / name.lower())
        print(f"  {name}: {len(a)} unit groups")
        # ISCO's armed forces units are 0110, 0210 and 0310; COD's other 0xxx codes are police and fire services
        rows += [(name, u, v, n) for u, (v, n) in sorted(a.items()) if not u.startswith("0") or u in ("0110", "0210", "0310")]
    OUT.parent.mkdir(parents=True, exist_ok=True)
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("# Earnings by ISCO-08 unit group from national surveys; written by scripts/prepare_pay.py\n")
        f.write("anchor\tunit\tvalue\tn\n")
        for name, u, v, n in rows:
            f.write(f"{name}\t{u}\t{v:.6g}\t{n}\n")
    print(f"  {OUT.name}: {len(rows)} rows")


if __name__ == "__main__":
    if len(sys.argv) > 2:
        sys.exit(__doc__)
    main(sys.argv[1] if len(sys.argv) == 2 else ROOT / "raw_data" / "pay")
