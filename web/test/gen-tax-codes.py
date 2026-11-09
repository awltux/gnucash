#!/usr/bin/env python3
"""Generate libgnucash/reports/gnc-tax-codes.cpp from the Scheme TXF tables.

The Scheme report's category mapping is data: four tables (income, expense,
asset, liability/equity), each keyed by tax entity type, then by TXF code, with
a vector of (payer-name source, form, description, format, multiple, category
key, line-per-year list, last valid year).  This script reads those tables out
of libgnucash/tax/us/txf.scm and writes the same data as C++ so the native
report needs no Scheme at run time.

Usage: python3 web/test/gen-tax-codes.py <txf.scm> <out.cpp>
"""
import re
import sys

TABLES = {
    "txf-income-categories": ("Income", "income"),
    "txf-expense-categories": ("Expense", "expense"),
    "txf-asset-categories": ("Asset", "asset"),
    "txf-liab-eq-categories": ("LiabEq", "liab_eq"),
}


def split_top(text):
    """Split a Scheme vector body on top-level spaces."""
    parts = []
    current = []
    depth = 0
    in_string = False
    escaped = False
    for char in text:
        if in_string:
            current.append(char)
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                in_string = False
            continue
        if char == '"':
            in_string = True
            current.append(char)
        elif char in "([":
            depth += 1
            current.append(char)
        elif char in ")]":
            depth -= 1
            current.append(char)
        elif char.isspace() and depth == 0:
            if current:
                parts.append("".join(current))
                current = []
        else:
            current.append(char)
    if current:
        parts.append("".join(current))
    return parts


def scheme_string(token):
    if not (token.startswith('"') and token.endswith('"')):
        raise ValueError(f"not a string: {token}")
    body = token[1:-1]
    out = []
    i = 0
    while i < len(body):
        if body[i] == "\\" and i + 1 < len(body):
            nxt = body[i + 1]
            out.append({"n": "\n", "t": "\t", "\\": "\\", '"': '"'}.get(nxt, nxt))
            i += 2
        else:
            out.append(body[i])
            i += 1
    return "".join(out)


def parse_line_data(token):
    """((1990 "1") (1982 "2")) -> [(1990, "1"), ...]"""
    token = token.strip()
    if not token.startswith("("):
        return []
    inner = token[1:-1]
    pairs = []
    for match in re.finditer(r'\(\s*(\d+)\s*"((?:[^"\\]|\\.)*)"\s*\)', inner):
        pairs.append((int(match.group(1)), scheme_string('"' + match.group(2) + '"')))
    return pairs


def entries_in(text, start_index):
    """Yield (code, body) for each (cons 'CODE #(...)) in text[start:end]."""
    pattern = re.compile(r"\(cons\s+'([A-Za-z0-9]+)\s+#\(")
    pos = start_index
    while True:
        match = pattern.search(text, pos)
        if match is None:
            return
        depth = 0
        in_string = False
        escaped = False
        index = match.end() - 1  # at the '#(' opening paren
        while index < len(text):
            char = text[index]
            if in_string:
                if escaped:
                    escaped = False
                elif char == "\\":
                    escaped = True
                elif char == '"':
                    in_string = False
            else:
                if char == '"':
                    in_string = True
                elif char == "(":
                    depth += 1
                elif char == ")":
                    depth -= 1
                    if depth == 0:
                        break
            index += 1
        body = text[match.end():index]
        yield match.group(1), body, match.start(), index + 1
        pos = index + 1


def sort_key(item):
    return (item[1], item[0])  # category, code


def main(argv):
    if len(argv) not in (3, 4):
        raise SystemExit(__doc__)
    revision = argv[3] if len(argv) == 4 else "unknown"
    source = open(argv[1], encoding="utf-8").read()
    records = []  # (category, entity, code, fields)
    for table, (category, _slug) in TABLES.items():
        match = re.search(r"\(define\s+" + table + r"\b", source)
        if match is None:
            raise SystemExit(f"table {table} not found")
        # The table ends at the next top-level "(define " or end of file.
        end = re.search(r"\n\(define\s+", source[match.end():])
        end_index = match.end() + end.start() if end else len(source)
        text = source[match.end():end_index]
        # Entity blocks: (cons 'F1040\n (list ...))
        entity_pattern = re.compile(r"\(cons\s+'([A-Za-z0-9]+)\s*\n?\s*\(list")
        entity_matches = list(entity_pattern.finditer(text))
        for number, entity_match in enumerate(entity_matches):
            entity = entity_match.group(1)
            block_end = (entity_matches[number + 1].start()
                         if number + 1 < len(entity_matches) else len(text))
            for code, body, entry_start, entry_end in entries_in(
                    text, entity_match.end()):
                if entry_start >= block_end:
                    continue
                fields = split_top(body)
                if len(fields) < 6:
                    raise SystemExit(f"short entry {entity}/{code}: {body!r}")
                pns = fields[0]
                form = scheme_string(fields[1])
                description = scheme_string(fields[2])
                fmt = int(fields[3])
                multiple = fields[4] == "#t"
                category_key = scheme_string(fields[5])
                lines = parse_line_data(fields[6]) if len(fields) > 6 else []
                last_year = int(fields[7]) if len(fields) > 7 else 0
                records.append((category, entity, code, pns, form, description,
                                fmt, multiple, category_key, lines, last_year))

    # Emit one array per (category, entity), sorted by code for a stable file.
    out = []
    w = out.append
    w("/* Generated by web/test/gen-tax-codes.py from libgnucash/tax/us/txf.scm")
    w(f" * at revision {revision}.")
    w(" * Do not edit by hand; rerun the generator when the Scheme table changes. */")
    w("")
    w("#include <glib.h>")
    w("")
    w('#include "gnc-tax-codes.hpp"')
    w("")
    w("namespace gnc")
    w("{")
    w("namespace report")
    w("{")
    w("namespace tax")
    w("{")
    w("namespace")
    w("{")
    w("")

    arrays = {}
    groups = {}
    for record in records:
        groups.setdefault((record[0], record[1]), []).append(record)

    line_index = 0
    for (category, entity), group in sorted(groups.items()):
        for record in sorted(group, key=lambda item: item[2]):
            code = record[2]
            lines = record[9]
            if lines:
                name = f"lines_{line_index}"
                line_index += 1
                w(f"const TaxLine {name}[] = {{")
                for year, line in lines:
                    w(f'    {{{year}, "{line}"}},')
                w("};")
                w("")
                arrays[(category, entity, code)] = (name, len(lines))
            else:
                arrays[(category, entity, code)] = ("nullptr", 0)

    for (category, entity), group in sorted(groups.items()):
        array = f"{category.lower()}_{entity.lower()}"
        w(f"const TaxCode {array}[] = {{")
        for record in sorted(group, key=lambda item: item[2]):
            (_, _, code, pns, form, description, fmt, multiple, category_key,
             lines, last_year) = record
            line_name, line_count = arrays[(category, entity, code)]
            pns_map = {"none": "None", "current": "Current", "parent": "Parent",
                       "not-impl": "NotImplemented"}
            w('    {{"{}", "{}", "{}", {}, TaxPayer::{}, {}, "{}", {}, {}, {}}},'
              .format(code, form, description, fmt, pns_map.get(pns, "None"),
                      "true" if multiple else "false", category_key,
                      line_name, line_count, last_year))
        w("};")
        w("")

    w("} // namespace")
    w("")
    w("const TaxCode*")
    w("lookup (Class cls, const char* entity, const char* code)")
    w("{")
    w("    if (entity == nullptr || code == nullptr)")
    w("        return nullptr;")
    w("    struct Table { Class cls; const char* entity; const TaxCode* codes;")
    w("                   size_t count; };")
    w("    static const Table tables[] = {")
    for (category, entity), group in sorted(groups.items()):
        array = f"{category.lower()}_{entity.lower()}"
        count = len(group)
        w(f'        {{Class::{category}, "{entity}", {array}, {count}}},')
    w("    };")
    w("    for (const Table& table : tables)")
    w("    {")
    w("        if (table.cls != cls || g_strcmp0 (table.entity, entity) != 0)")
    w("            continue;")
    w("        for (size_t index = 0; index < table.count; ++index)")
    w("        {")
    w("            if (g_strcmp0 (table.codes[index].code, code) == 0)")
    w("                return &table.codes[index];")
    w("        }")
    w("    }")
    w("    return nullptr;")
    w("}")
    w("")
    w("} // namespace tax")
    w("} // namespace report")
    w("} // namespace gnc")
    w("")

    with open(argv[2], "w", encoding="utf-8") as handle:
        handle.write("\n".join(out))
    print(f"wrote {argv[2]}: {len(records)} codes, {line_index} line tables")
    entities = sorted({(category, entity) for category, entity, *_ in records})
    print(", ".join(f"{category}/{entity}" for category, entity in entities))


if __name__ == "__main__":
    main(sys.argv)
