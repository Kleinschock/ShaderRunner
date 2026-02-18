# Code Audit Identifiers (C/C++)

**Near-Miss Finder & A/B-Vergleich von Identifier-Counts**

Dieses Tool hilft dir, in einer großen C/C++-Codebase zwei typische Refactor/Migrations-Probleme schnell aufzuspüren:

1. **`near`-Modus**: Finde Tippfehler / Varianten eines Identifiers (z.B. `Root` vs `Rot` vs `root`) gruppiert nach **Edit-Distanz 0..k**.
2. **`compare`-Modus**: Vergleiche zwei Identifier (z.B. `Elefant` als korrektes Baseline-Symbol vs `Giraffe` als neu eingeführtes Symbol) und prüfe, ob `Giraffe` **genauso oft** in Datei X / im Projekt vorkommt wie `Elefant` (oder andere Expectations).

> Wichtig: C/C++-Namen/Identifier sind **case-sensitive** – `Root` und `root` sind nicht dasselbe. ([Python documentation][1])

---

## Features

### Allgemein

* Rekursive Suche im Projektordner mit Default-Excludes (z.B. `.git`, `build`, `node_modules`, …)
* Scannt typische C/C++-Source Extensions (`.h/.hpp/.cpp/...`)
* Optional: **Maskiert Kommentare & String/Char-Literale** (default **an**), damit Zählungen nicht von Kommentaren/Strings verfälscht werden
* Optional: **Multiprocessing** via `--jobs` (Pool) für große Codebases. ([Python documentation][2])
* Export als **JSON** und **CSV**

### `near` (Near-Miss Search)

* Gruppiert Treffer nach Edit-Distanz (0 = exakt, 1 = ein Edit, …)
* Optionales “Speed-Filter”: nur Tokens mit gleichem ersten Buchstaben (`--same-first-char`)
* Token-Blacklist (`--ignore-token …`)
* Limit pro Token für Positionsausgabe (`--max-locs-per-token`)

**Technik:** bounded/banded Levenshtein mit Early Exit (Edit-Distance nur bis Schwelle *k*), basiert auf bekannten Threshold-/Banding-Ansätzen (Ukkonen-artig). ([University of Helsinki][3])

### `compare` (A/B-Counts)

* Zählt `A` und `B` **pro Datei** (und total)
* Checks/Expectations:

  * `equal`: A == B (mit optionaler Toleranz)
  * `a>=b` oder `b>=a`
  * `ratio`: A ≈ ratio * B (plus/minus Toleranz)
* Optional: nur Mismatches ausgeben (`--only-mismatches`)
* Pro Datei optional Positions-Snippets (Zeile:Spalte)

---

## Installation

Kein Paket nötig, nur Python 3.10+ empfohlen.

```bash
# Datei ins Repo legen, z.B. tools/code_audit_identifiers.py
python --version
```

---

## Quickstart

### Help / Usage

Das CLI ist mit `argparse` umgesetzt (Subcommands). ([Python documentation][1])

```bash
python code_audit_identifiers.py -h
python code_audit_identifiers.py near -h
python code_audit_identifiers.py compare -h
```

---

## Beispiele

### 1) Near-Misses für `Root` finden (Edit-Distanz bis 3)

```bash
python code_audit_identifiers.py --root . near --needle Root --max-dist 3
```

Typisches Ergebnis: Du siehst Buckets `EDIT DISTANCE = 0/1/2/3` und darin Tokens wie `Root`, `Rot`, `RooT`, `root` etc., jeweils mit Count & Fundstellen.

### 2) “Ich will nur Case-Probleme”

Wenn du weißt, dass es inhaltlich *dieselbe* Klasse ist und du nur Groß/Kleinschreibung jagst:

```bash
python code_audit_identifiers.py --root . near --needle Root --max-dist 1 --case-insensitive
```

> Achtung: `--case-insensitive` kann mehr “false positives” erzeugen, weil C++ case-sensitiv ist. ([Python documentation][1])

### 3) Near-Misses schneller machen (gleicher Anfangsbuchstabe)

```bash
python code_audit_identifiers.py --root . near --needle Root --max-dist 2 --same-first-char
```

### 4) “Elefant ist korrekt, Giraffe muss gleich oft vorkommen”

Gesamtes Projekt:

```bash
python code_audit_identifiers.py --root . compare --a Elefant --b Giraffe --expect equal
```

Nur Mismatches:

```bash
python code_audit_identifiers.py --root . compare --a Elefant --b Giraffe --expect equal --only-mismatches
```

### 5) Nur eine Datei prüfen (Datei X)

```bash
python code_audit_identifiers.py --root . --file src/foo/bar.cpp compare --a Elefant --b Giraffe --expect equal
```

### 6) Nur ein Teilbaum (Globs)

```bash
python code_audit_identifiers.py --root . --only-glob "src/**" compare --a Elefant --b Giraffe
```

### 7) “Giraffe darf nie seltener sein als Elefant”

```bash
python code_audit_identifiers.py --root . compare --a Giraffe --b Elefant --expect a>=b
```

### 8) Ratio-Check (Migration in Stufen)

Beispiel: Du erwartest grob `Giraffe ≈ 0.5 * Elefant` (halbe Migration). Toleranz = 3 (Counts sind integer):

```bash
python code_audit_identifiers.py --root . compare --a Giraffe --b Elefant --expect ratio --ratio 0.5 --tolerance 3
```

### 9) Exporte (für Excel/QA/CI)

```bash
python code_audit_identifiers.py --root . compare --a Elefant --b Giraffe --csv compare.csv --json compare.json
python code_audit_identifiers.py --root . near --needle Root --max-dist 3 --csv near.csv --json near.json
```

### 10) Kommentare/Strings mitzählen (default ist maskiert)

```bash
python code_audit_identifiers.py --root . --no-mask compare --a Elefant --b Giraffe
```

---

## Beispiel-Output (compare)

```text
Root:              /path/to/repo
Mode:              compare
A:                 Elefant
B:                 Giraffe
expect:            equal
tolerance:         0
files:             412

TOTAL A:           193
TOTAL B:           187
TOTAL diff:        6
TOTAL ok?:         NO

Mismatching files: 4

FILE                                                         A        B     DIFF  LOCS(A) / LOCS(B)
--------------------------------------------------------------------------------------------------------------
src/foo/bar.cpp                                              12       10        2  11:5, 32:9 / 12:7, 40:3
...
```

---

## Typische Workflows / Ideen

### A) “Ich habe ein Modul eingebunden und will sicher sein, dass ich überall konsistent war”

1. `near` auf den neuen Klassennamen (`Root`) mit `--max-dist 2..3` laufen lassen
2. Alle Tokens mit Distanz 1–3 reviewen → oft sind das echte Tippfehler
3. Danach `compare` gegen “Baseline-Symbol” (z.B. alte Klasse oder Interface) um Vollständigkeit zu checken

### B) “Refactor-Progress messen” (CI/Automation)

* `compare` in CI laufen lassen und failen, wenn `--only-mismatches` nicht leer ist
* JSON ausgeben und im CI Artefakt speichern (Trend über Zeit)

### C) “Nur Hotspots”

* `--only-glob "src/**"` oder nur bestimmte Module scannen
* `--file` wenn du gerade eine Datei reparierst

### D) “Noise reduzieren”

* Default masking aktiv lassen (Kommentare/Strings ignorieren), damit Counts “real code usage” reflektieren
* Für `near`: `--ignore-token` mit häufigen Tokens füttern (z.B. `int`, `size`, `std`, projektinterne Prefixe)

### E) “Ich will nur ‘fast sichere’ Tippfehler”

* `near --max-dist 1` + `--same-first-char` ist oft ein guter Sweet Spot

---

## Performance-Tipps

* Setze `--max-dist` klein (1–3). Banded/threshold Edit-Distance skaliert dann gut. ([University of Helsinki][3])
* Nutze `--jobs N` bei großen Repos (Process Pool). ([Python documentation][2])
* Begrenze Locations (`--max-locs-per-token` / `--locs-per-file`), damit Output nicht explodiert.

---

## Limitations (ehrlich, damit du weißt was du bekommst)

* Kein echter C++-Parser: Tokens werden per Regex als Identifier gelesen (funktioniert in der Praxis sehr gut für solche Audits, aber es kann Edge-Cases geben).
* Makros/Preprocessor-Tricks können “komisch” aussehen; dafür ist `--no-mask` manchmal hilfreich.
* Unicode-Identifier werden nicht speziell unterstützt (ASCII-Regex).

---

## README-Struktur / Erweiterungen

Wenn du das auf GitHub packst: eine README mit **Overview → Install → Usage → Examples → Limitations** ist ein bewährtes Muster. ([GitHub][4])

---

Wenn du willst, kann ich dir als nächsten Schritt:

* eine **`.gitignore` + minimalen Repo-Layout Vorschlag** geben,
* oder ein **CI-Snippet** (GitHub Actions) schreiben, das `compare --only-mismatches` als Quality Gate nutzt.

[1]: https://docs.python.org/3/library/argparse.html?utm_source=chatgpt.com "argparse — Parser for command-line options, arguments ..."
[2]: https://docs.python.org/3/library/multiprocessing.html?utm_source=chatgpt.com "multiprocessing — Process-based parallelism"
[3]: https://www.cs.helsinki.fi/u/ukkonen/InfCont85.PDF?utm_source=chatgpt.com "Algorithms for Approximate String Matching - Computer Science"
[4]: https://github.com/banesullivan/README?utm_source=chatgpt.com "How to write a good README"
