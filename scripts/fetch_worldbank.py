"""Fetch the World Bank (WDI) series behind the income tables and write compact raw TSVs.

World Development Indicators are published under CC BY 4.0 (https://datacatalog.worldbank.org).

  NY.GDP.PCAP.CN   GDP per capita, current local currency   -> income.tsv (level of countries without a survey;
                                                               the sanity check of surveyed ones)
  PA.NUS.FCRF      official exchange rate, LCU per USD      -> converting earnings rows published in another currency

Output: raw_data/wb_<series>.tsv with ref_area (ISO alpha-3, as WDI writes it), year, value.

Usage: python fetch_worldbank.py [OUT_DIR]      (default: raw_data)
"""
import json
import sys
import urllib.request
from pathlib import Path

URL = "https://api.worldbank.org/v2/country/all/indicator/{id}?format=json&date=2010:2025&per_page=20000"
SERIES = ("NY.GDP.PCAP.CN", "PA.NUS.FCRF")


def fetch(series, out_path):
    req = urllib.request.Request(URL.format(id=series), headers={"User-Agent": "Mozilla/5.0"})
    with urllib.request.urlopen(req, timeout=600) as r:
        meta, rows = json.load(r)
    if meta.get("pages", 1) != 1:
        sys.exit(f"{series}: {meta['pages']} pages; raise per_page")
    n = 0
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("ref_area\tyear\tvalue\n")
        for row in rows:
            if row["value"] is None or not row["countryiso3code"]:
                continue
            f.write(f"{row['countryiso3code']}\t{row['date']}\t{row['value']!r}\n")
            n += 1
    return n


def main(out_dir):
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    for s in SERIES:
        path = out / f"wb_{s}.tsv"
        print(f"{s}: {fetch(s, path)} rows -> {path}")


if __name__ == "__main__":
    if len(sys.argv) > 2:
        sys.exit(__doc__)
    main(sys.argv[1] if len(sys.argv) == 2 else "raw_data")
