"""Fetch the ILOSTAT indicators behind the occupation tables and write compact raw TSVs.

ILOSTAT (ILO, https://ilostat.ilo.org) publishes its data under CC BY 4.0.
The indicators come from its bulk API (rplumber.ilo.org), one CSV per
indicator, from 2010 on. Only the rows the tables use are kept:

  EMP_TEMP_SEX_OC2_NB_A       employment by sex and ISCO-08 level 2    -> shares.tsv
  EMP_TEMP_SEX_AGE_OCU_NB_A   employment by age and ISCO-08 level 1    -> factors.tsv (age_band; 4 age classes)
  EMP_TEMP_SEX_OCU_EDU_NB_A   employment by ISCO-08 level 1, education -> factors.tsv (education)
  POP_XWAP_SEX_AGE_EDU_NB_A   working-age population by age, education -> education.tsv
  POP_XWAP_SEX_AGE_NB_A       working-age population by sex and age    -> status.tsv
  EMP_TEMP_SEX_AGE_NB_A       employment by sex and age                -> status.tsv
  UNE_TUNE_SEX_AGE_NB_A       unemployment by sex and age              -> status.tsv

Output, one file per indicator (raw_data/ilostat_<indicator>.tsv (ignored by git)):
ref_area (ISO alpha-3), source, time, sex, classif1, classif2, value, status
(`status` is ILOSTAT's observation flag: U = unreliable, B = break).

Usage: python fetch_ilostat.py [OUT_DIR]      (default: raw_data)
"""
import csv
import io
import sys
import urllib.request
from pathlib import Path

URL = ("https://rplumber.ilo.org/data/indicator/?id={id}&lang=en&type=code&format=.csv&timefrom=2010")
HEADERS = {"User-Agent": "Mozilla/5.0"}  # the server answers an empty body without one

AGE10 = "AGE_10YRBANDS_"
AGE5 = "AGE_5YRBANDS_"
AGE4 = "AGE_AGGREGATE_"  # 15-24, 25-54, 55-64, 65+: the only age classes ILOSTAT cross-tabulates with ISCO-08
ISCO1 = {f"OCU_ISCO08_{d}" for d in "0123456789"} | {"OCU_ISCO08_TOTAL"}
EDU = {f"EDU_AGGREGATE_{x}" for x in ("LTB", "BAS", "INT", "ADV", "X", "TOTAL")}

# indicator -> predicate on (sex, classif1, classif2)
FILTERS = {
    "EMP_TEMP_SEX_OC2_NB_A": lambda s, c1, c2: s in ("SEX_M", "SEX_F") and c1.startswith("OC2_ISCO08_"),
    "EMP_TEMP_SEX_AGE_OCU_NB_A": lambda s, c1, c2: s == "SEX_T" and c1.startswith(AGE4) and c2 in ISCO1,
    "EMP_TEMP_SEX_OCU_EDU_NB_A": lambda s, c1, c2: s in ("SEX_T",) and c1 in ISCO1 and c2 in EDU,
    "POP_XWAP_SEX_AGE_EDU_NB_A": lambda s, c1, c2: s in ("SEX_M", "SEX_F") and c1.startswith(AGE10) and c2 in EDU,
    "POP_XWAP_SEX_AGE_NB_A": lambda s, c1, c2: s in ("SEX_M", "SEX_F") and c1.startswith(AGE5),
    "EMP_TEMP_SEX_AGE_NB_A": lambda s, c1, c2: s in ("SEX_M", "SEX_F") and c1.startswith(AGE5),
    "UNE_TUNE_SEX_AGE_NB_A": lambda s, c1, c2: s in ("SEX_M", "SEX_F") and c1.startswith(AGE5),
}


def fetch(indicator, keep, out_path):
    req = urllib.request.Request(URL.format(id=indicator), headers=HEADERS)
    n = 0
    with urllib.request.urlopen(req, timeout=600) as r, open(out_path, "w", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(io.TextIOWrapper(r, encoding="utf-8-sig", newline=""))
        w = csv.writer(f, delimiter="\t", lineterminator="\n")
        w.writerow(["ref_area", "source", "time", "sex", "classif1", "classif2", "value", "status"])
        for row in reader:
            sex = row.get("sex", "")
            c1 = row.get("classif1", "")
            c2 = row.get("classif2", "")
            if not keep(sex, c1, c2) or row["obs_value"] == "":
                continue
            w.writerow([row["ref_area"], row["source"], row["time"], sex, c1, c2, row["obs_value"], row["obs_status"]])
            n += 1
    return n


def main(out_dir):
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    for indicator, keep in FILTERS.items():
        path = out / f"ilostat_{indicator}.tsv"
        print(f"{indicator}: {fetch(indicator, keep, path)} rows -> {path}")


if __name__ == "__main__":
    if len(sys.argv) > 2:
        sys.exit(__doc__)
    main(sys.argv[1] if len(sys.argv) == 2 else "raw_data")
