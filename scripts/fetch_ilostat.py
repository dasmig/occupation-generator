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
  EAR_EMTM_SEX_NB_A           median monthly earnings by sex           -> income.tsv
  EAR_EMTG_SEX_NB_A           Gini of monthly earnings by sex          -> income_effects.tsv (sigma)
  EAR_EMTM_SEX_OCU_NB_A       median by ISCO-08 major group            -> income_effects.tsv (major)
  EAR_EMTG_SEX_OCU_NB_A       Gini by ISCO-08 major group              -> income_effects.tsv (sigma)
  EAR_EMTM_SEX_AGE_NB_A       median by 10-year age band               -> income_effects.tsv (age_band)
  EAR_EMTM_SEX_EDU_NB_A       median by education                      -> income_effects.tsv (education)
  EAR_EMTM_SEX_OCU_CUR_NB_A   the same median in USD                   -> converting rows in another currency
  CPI_NCYR_COI_RT_A           CPI, annual change, all items            -> uprating to the price year

Output, one file per indicator (raw_data/ilostat_<indicator>.tsv (ignored by git)):
ref_area (ISO alpha-3), source, time, sex, classif1, classif2, value, status, note
(`status` is ILOSTAT's observation flag: U = unreliable, B = break; `note` is the
indicator note, whose T30 code names the currency of an earnings row).

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
    # Earnings of employees (income, 3.1.0). The `note` column keeps the T30 currency note.
    "EAR_EMTM_SEX_NB_A": lambda s, c1, c2: s in ("SEX_M", "SEX_F", "SEX_T"),
    "EAR_EMTG_SEX_NB_A": lambda s, c1, c2: s in ("SEX_M", "SEX_F", "SEX_T"),
    "EAR_EMTM_SEX_OCU_NB_A": lambda s, c1, c2: s == "SEX_T" and c1 in ISCO1,
    "EAR_EMTG_SEX_OCU_NB_A": lambda s, c1, c2: s == "SEX_T" and c1 in ISCO1,
    "EAR_EMTM_SEX_AGE_NB_A": lambda s, c1, c2: s == "SEX_T" and c1.startswith(AGE10),
    "EAR_EMTM_SEX_EDU_NB_A": lambda s, c1, c2: s == "SEX_T" and c1 in EDU,
    "EAR_EMTM_SEX_OCU_CUR_NB_A": lambda s, c1, c2: s == "SEX_T" and c1 in ("OCU_ISCO08_TOTAL", "OCU_SKILL_TOTAL")
                                                   and c2 in ("CUR_TYPE_LCU", "CUR_TYPE_USD"),
    "CPI_NCYR_COI_RT_A": lambda s, c1, c2: c1 == "COI_COICOP_CP01T12",
}


def fetch(indicator, keep, out_path):
    req = urllib.request.Request(URL.format(id=indicator), headers=HEADERS)
    n = 0
    with urllib.request.urlopen(req, timeout=600) as r, open(out_path, "w", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(io.TextIOWrapper(r, encoding="utf-8-sig", newline=""))
        w = csv.writer(f, delimiter="\t", lineterminator="\n")
        w.writerow(["ref_area", "source", "time", "sex", "classif1", "classif2", "value", "status", "note"])
        for row in reader:
            sex = row.get("sex", "")
            c1 = row.get("classif1", "")
            c2 = row.get("classif2", "")
            if not keep(sex, c1, c2) or row["obs_value"] == "":
                continue
            w.writerow([row["ref_area"], row["source"], row["time"], sex, c1, c2, row["obs_value"],
                        row.get("obs_status", ""), row.get("note_indicator", "")])
            n += 1
    return n


NOTES_URL = "https://rplumber.ilo.org/metadata/dic/?var=note_indicator&lang=en&format=.csv"


def fetch_notes(out_path):
    """The indicator-note dictionary: code -> label (T30 notes name a currency)."""
    req = urllib.request.Request(NOTES_URL, headers=HEADERS)
    with urllib.request.urlopen(req, timeout=600) as r, open(out_path, "w", encoding="utf-8", newline="") as f:
        reader = csv.reader(io.TextIOWrapper(r, encoding="utf-8-sig", newline=""))
        w = csv.writer(f, delimiter="\t", lineterminator="\n")
        next(reader)
        w.writerow(["note", "label"])
        n = 0
        for row in reader:
            w.writerow(row[:2])
            n += 1
    return n


def main(out_dir, only=()):
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    for indicator, keep in FILTERS.items():
        if only and indicator not in only:
            continue
        path = out / f"ilostat_{indicator}.tsv"
        print(f"{indicator}: {fetch(indicator, keep, path)} rows -> {path}")
    print(f"notes: {fetch_notes(out / 'ilostat_notes.tsv')} rows")


if __name__ == "__main__":
    # fetch_ilostat.py [OUT_DIR [INDICATOR ...]]: with indicators, fetch only those
    main(sys.argv[1] if len(sys.argv) >= 2 else "raw_data", tuple(sys.argv[2:]))
