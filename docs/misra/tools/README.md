# MISRA tooling

Scripts that produce the baseline scan and regenerate the policy documents.

## One-time setup: the rule-text file

The cppcheck MISRA addon reports bare rule numbers unless you give it the
guideline text. That text is copyrighted, so it is **not** in this repository
and must not be committed. Build it from your own licensed PDF:

    pdftotext -layout MISRA_C_2012.pdf /tmp/misra_layout.txt
    python3 docs/misra/tools/make_rule_texts.py /tmp/misra_layout.txt /tmp/misra_rule_texts.txt

The script parses Appendix A and refuses to write a file if any entry comes out
truncated or duplicated. Expect `173 guidelines (156 rules, 17 directives)`.

Check the addon agrees:

    python3 /usr/share/Cppcheck/addons/misra.py \
      --rule-texts=/tmp/misra_rule_texts.txt --verify-rule-texts

It should print `Rule texts are correct.` Rules 1.4 and 21.21 are not in the
Third edition first revision; `make_rule_texts.py` emits a placeholder for each
so the addon does not report them as missing.

Then write the addon config:

    cat > /tmp/misra.json <<EOF
    {"script": "misra.py", "args": ["--rule-texts=/tmp/misra_rule_texts.txt"]}
    EOF

## Running the scan

    MISRA_JSON=/tmp/misra.json docs/misra/tools/scan.sh /tmp/scan.out \
      -j $(nproc) -- $(find src -name '*.c')

Roughly one minute on 147 translation units with `-j`.

## Reading the result

    python3 docs/misra/tools/aggregate.py /tmp/scan.out /tmp/misra_rule_texts.txt

Splits hand-written from generated code, holds back the rules known to be
checker defects, and reports what failed to parse. Read the coverage block
first: a translation unit that fails to parse produces no findings, which looks
exactly like a clean file.

To see what actually triggered one rule before believing its count:

    python3 docs/misra/tools/whatfired.py /tmp/scan.out 17.3 .

This is how the Rule 7.3 and Rule 17.3 checker defects were found. Do it for any
rule whose count you are about to quote.

## Regenerating the policy documents

The decisions live in `policy.py`. Edit them there, never in the markdown.

    python3 docs/misra/tools/policy.py /tmp/scan.out /tmp/misra_rule_texts.txt \
      /usr/share/Cppcheck/addons/misra.py docs/misra/guideline-decisions.tsv
    python3 docs/misra/tools/mkdocs.py docs/misra/guideline-decisions.tsv \
      /tmp/misra_rule_texts.txt docs/misra

`policy.py` checks its own table before writing: a Mandatory guideline marked
for deviation, an Advisory marked for deviation instead of decline, a Required
marked as declined, a deviation with no record id, or a violated rule with no
decision all stop it with an error.

## If you add this to the build

`cppcheck` exits 0 even when it reports violations. A gate that reads its exit
status will pass forever. Use `--error-exitcode=1`, and check the gate can fail
before trusting it.
