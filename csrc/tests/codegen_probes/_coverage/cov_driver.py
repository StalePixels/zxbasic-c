#!/usr/bin/env python3
"""cov_driver.py — codegen-scoped coverage driver for the probe series.

Compiles a list of .bas files IN-PROCESS through the Python reference zxbc
(--output-format=asm, i.e. the full frontend+backend codegen path) under
coverage.py, scoped to the codegen-relevant reference modules ONLY:

    src/symbols/*.py
    src/api/check.py
    src/api/errmsg.py
    src/zxbc/*.py          (incl. zxbparser.py — the parser/translator)
    src/arch/z80/backend/*.py

main() re-inits all global state at entry (config.init / zxbparser.init /
Backend().init / Translator.reset / asmparse.init / zxbpp.init), so repeated
in-process calls are sound for coverage.

Usage:
    cov_driver.py run   <suffix> <file-with-list-of-bas-paths>
    cov_driver.py report
    cov_driver.py combine-report

Run from project ROOT (the dir containing src/). Writes .coverage* there.
Always exits 0 (diagnostic).
"""
import sys, os, io, contextlib, glob

ROOT = os.environ.get("ZXBC_ROOT") or os.getcwd()
sys.path.insert(0, ROOT)

INCLUDE = [
    "src/symbols/*.py",
    "src/api/check.py",
    "src/api/errmsg.py",
    "src/zxbc/*.py",
    "src/arch/z80/backend/*.py",
]


def _cov(suffix=None):
    import coverage
    return coverage.Coverage(
        data_file=os.path.join(ROOT, ".coverage"),
        data_suffix=suffix,
        include=INCLUDE,
        branch=True,
    )


def cmd_run(suffix, listfile):
    with open(listfile) as fh:
        files = [ln.strip() for ln in fh if ln.strip()]
    extra = os.environ.get("ZXBC_ARGS", "").split()
    cov = _cov(suffix=suffix)
    cov.start()
    # import AFTER coverage.start so import-time lines in target modules count
    from src.zxbc.zxbc import main
    tmpasm = os.path.join(ROOT, ".cov_scratch.asm")
    ok = err = 0
    for f in files:
        argv = ["--output-format=asm", "-o", tmpasm] + extra
        if os.path.basename(f).startswith("zxnext_"):
            argv.append("--zxnext")
        argv.append(f)
        try:
            with contextlib.redirect_stdout(io.StringIO()), \
                 contextlib.redirect_stderr(io.StringIO()):
                main(argv)
            ok += 1
        except SystemExit:
            ok += 1
        except BaseException:
            err += 1
    cov.stop()
    cov.save()
    try:
        os.remove(tmpasm)
    except OSError:
        pass
    sys.stderr.write(f"[cov_driver] suffix={suffix} compiled={len(files)} ok={ok} pyexc={err}\n")


def cmd_report():
    cov = _cov()
    cov.load()
    cov.report(show_missing=True, file=sys.stdout)


def cmd_combine_report():
    cov = _cov()
    cov.combine()  # merges .coverage.* into .coverage
    cov.save()
    cov.report(show_missing=True, file=sys.stdout)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit("usage: cov_driver.py run|report|combine-report ...")
    c = sys.argv[1]
    if c == "run":
        cmd_run(sys.argv[2], sys.argv[3])
    elif c == "report":
        cmd_report()
    elif c == "combine-report":
        cmd_combine_report()
    else:
        sys.exit(f"unknown subcommand {c}")
    sys.exit(0)
