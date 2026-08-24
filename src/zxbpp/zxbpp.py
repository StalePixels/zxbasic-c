#!/usr/bin/env python3

# --------------------------------------------------------------------
# SPDX-License-Identifier: AGPL-3.0-or-later
# © Copyright 2008-2024 José Manuel Rodríguez de la Rosa and contributors.
# See the file CONTRIBUTORS.md for copyright details.
# See https://www.gnu.org/licenses/agpl-3.0.html for details.
# --------------------------------------------------------------------

import argparse
import os
import re
import sys
from dataclasses import dataclass
from enum import StrEnum, unique
from typing import Any, Final, NamedTuple

from src.api import config, global_, utils
from src.zxbpp import zxbasmpplex, zxbpplex
from src.zxbpp.base_pplex import STDIN
from src.zxbpp.prepro import ID, Arg, ArgList, DefinesTable, MacroCall, output
from src.zxbpp.prepro.builtinmacro import BuiltinMacro
from src.zxbpp.prepro.exceptions import PreprocError
from src.zxbpp.prepro.operators import Concatenation, Stringizing
from src.zxbpp.prepro.output import error, warning
from src.zxbpp.zxbpplex import tokens  # noqa

from .zxbpp_standalone import Lark_StandAlone, Lexer, Token, Transformer, UnexpectedInput


@unique
class PreprocMode(StrEnum):
    BASIC = "BASIC"
    ASM = "ASM"


# List of available architectures
AVAILABLE_ARCHITECTURES: Final[list[str]] = []

# Generated output
OUTPUT = ""

# Global macro (#defines) table
ID_TABLE = DefinesTable()

# Set to BASIC or ASM depending on the Lexer context
# e.g. for .ASM files should be set to zxbasmpplex.Lexer()
# Use setMode('ASM' or 'BASIC') to change this FLAG
LEXER: zxbasmpplex.Lexer | zxbpplex.Lexer = zxbpplex.Lexer(defines_table=ID_TABLE)

# CURRENT working directory for this cpp
CURRENT_DIR = None

# Default include path
INCLUDEPATH: list[str] = ["stdlib", "runtime"]

# Include paths for every arch
INCLUDE_MAP: Final[dict[str, list[str]]] = {}

# Enabled to FALSE if IFDEF failed
ENABLED: bool = True

# Defines Regexp to match filenames
RE_LOCAL_FIRST_FILENAME = re.compile('^"([^"]|"")*"$')
RE_GLOBAL_FIRST_FILENAME = re.compile("^<[^>]+>$")


class IfDef(NamedTuple):
    enabled: bool
    line: int


class ParentIncludingFile(NamedTuple):
    file_name: str
    lineno: int


@dataclass
class IncludedFileInfo:
    once: bool  # whether this file is to be included only once (e.g. #pragma once)
    parents: list[ParentIncludingFile]


# Files already included, with a list of (file, line) tuples where they were
# included, since a file can be included more than once.
INCLUDED: dict[str, IncludedFileInfo] = {}

# IFDEFS array
IFDEFS: list[IfDef] = []  # Push (Line, state here)


def remove_spaces(x: str) -> str:
    if not x:
        return x

    return x.strip(" \t") or " "


def reset_id_table():
    """Initializes ID_TABLE with default DEFINES
    (i.e. those that derives from OPTIONS)
    """
    ID_TABLE.clear()

    for name, val in config.OPTIONS.__DEFINES.items():
        ID_TABLE.define(name, value=val, lineno=0)

    for macro_name, macro_func in LEXER.builtin_macros.items():
        LEXER.set_macro(macro_name, BuiltinMacro(macro_name=macro_name, func=macro_func))


def init():
    """Initializes the preprocessor"""
    global OUTPUT
    global INCLUDED
    global CURRENT_DIR
    global ENABLED
    global IFDEFS

    from src import arch

    AVAILABLE_ARCHITECTURES.clear()
    AVAILABLE_ARCHITECTURES.extend(arch.AVAILABLE_ARCHITECTURES)

    config.OPTIONS(config.Action.ADD_IF_NOT_DEFINED, name="debug_zxbpp", type=bool, default=False)
    global_.FILENAME = STDIN
    OUTPUT = ""
    INCLUDED = {}
    CURRENT_DIR = ""
    ENABLED = True
    IFDEFS = []
    global_.has_errors = 0
    global_.error_msg_cache.clear()
    parser.defaulted_states = {}
    del output.CURRENT_FILE[:]

    reset_id_table()


def get_include_path(arch: str = "") -> str:
    """Default include path using a tricky sys call."""
    return os.path.realpath(
        os.path.join(
            os.path.dirname(__file__),
            os.path.pardir,
            "lib",
            "arch",
            arch or config.OPTIONS.architecture or "",
        )
    )


def set_include_path():
    global INCLUDEPATH

    INCLUDE_MAP.clear()

    for arch_ in AVAILABLE_ARCHITECTURES:
        pwd = get_include_path(arch_)
        INCLUDE_MAP[arch_] = [os.path.join(pwd, "stdlib"), os.path.join(pwd, "runtime")]

    # zx81sd inherits runtime files from zx48k (only overrides specific ones).
    # zx81sd paths take priority: its own files shadow zx48k equivalents.
    if "zx81sd" in INCLUDE_MAP:
        zx48k_pwd = get_include_path("zx48k")
        INCLUDE_MAP["zx81sd"].extend([os.path.join(zx48k_pwd, "stdlib"), os.path.join(zx48k_pwd, "runtime")])

    INCLUDEPATH = INCLUDE_MAP.get(config.OPTIONS.architecture, [])


def setMode(mode: PreprocMode) -> None:
    global LEXER

    mode = mode.upper()
    if mode not in list(PreprocMode):
        raise PreprocError('Invalid mode "%s"' % mode, lineno=LEXER.lineno)

    lexers = {
        PreprocMode.ASM: zxbasmpplex.Lexer(defines_table=ID_TABLE),
        PreprocMode.BASIC: zxbpplex.Lexer(defines_table=ID_TABLE),
    }

    LEXER = lexers[PreprocMode(mode)]


def search_filename(fname: str, lineno: int, local_first: bool, arch: str = "") -> str:
    """Search a filename into the list of the include path.
    If local_first is true, it will try first in the current directory of
    the file being analyzed.
    """
    fname = utils.sanitize_filename(fname)

    assert CURRENT_DIR is not None
    include_path = INCLUDE_MAP.get(arch, INCLUDEPATH)

    i_path: list[str] = [CURRENT_DIR] + include_path if local_first else list(include_path)
    i_path.extend(config.OPTIONS.include_path.split(":") if config.OPTIONS.include_path else [])

    if os.path.isabs(fname):
        if os.path.isfile(fname):
            return fname
    else:
        for dir_ in i_path:
            path = utils.get_absolute_filename_path(utils.sanitize_filename(os.path.join(dir_, fname)))
            if os.path.exists(path):
                return path

    error(lineno, "file '%s' not found" % fname)
    return ""


def include_file(filename: str, lineno: int, local_first: bool, arch: str = "") -> str:
    """Performs a file inclusion (#include) in the preprocessor.
    Writes down that "filename" was included in the current file,
    at line <lineno>.

    If local_first is True, then it will first search the file in the
    local path before looking for it in the include path chain.
    This is used when doing a #include "filename".
    """
    global CURRENT_DIR

    abs_filename = search_filename(filename, lineno, local_first, arch=arch)
    filename = utils.get_relative_filename_path(abs_filename)

    if abs_filename not in INCLUDED:
        INCLUDED[abs_filename] = IncludedFileInfo(once=False, parents=[])
    elif INCLUDED[abs_filename].once:
        # Empty file (already included)
        LEXER.next_token = "_ENDFILE_"
        return ""

    if output.CURRENT_FILE:  # Added from which file, line
        INCLUDED[abs_filename].parents.append(ParentIncludingFile(output.CURRENT_FILE[-1], lineno))

    output.CURRENT_FILE.append(filename)
    CURRENT_DIR = os.path.dirname(filename)
    return LEXER.include(filename)


def include_once(filename: str, lineno: int, local_first: bool, arch: str = "") -> str:
    """Performs a file inclusion (#include) in the preprocessor.
    Writes down that "filename" was included in the current file,
    at line <lineno>.

    The file is ignored if it was previously included (a warning will
    be emitted, though).

    If local_first is True, then it will first search the file in the
    local path before looking for it in the include path chain.
    This is used when doing a #include "filename".
    """
    abs_filename = search_filename(filename, lineno, local_first, arch=arch)

    if abs_filename not in INCLUDED:  # If not already included
        return include_file(filename, lineno, local_first, arch)  # include it and return

    # Now checks if the file has been included more than once
    if len(INCLUDED[abs_filename].parents) > 1:
        parent_file, lineno = INCLUDED[abs_filename].parents[0]
        warning(lineno, f"file '{filename}' already included more than once, in file '{parent_file}' at line {lineno}")

    # Empty file (already included)
    LEXER.next_token = "_ENDFILE_"
    return ""


def expand_macros(macros: list[Any], lineno: int) -> str | None:
    try:
        tmp = "".join(remove_spaces(str(x())) if isinstance(x, MacroCall) else x for x in macros)
    except PreprocError as v:
        error(v.lineno, v.message)
        return None

    if "\n" in tmp:
        tmp += f"\n#line {lineno + 1}"
    tmp += "\n"

    return tmp


def to_bool(expr: str | bool | int) -> int:
    if isinstance(expr, str) and expr.isdigit():
        expr = int(expr)

    return int(bool(expr))


def to_int(expr: str | int) -> int:
    if isinstance(expr, str) and expr.isdigit():
        expr = int(expr)
    else:
        expr = 0

    return expr


class PPToken(Token):
    pass


class LarkLexerAdapter(Lexer):
    def __init__(self, lexer_conf: Any) -> None:
        pass

    def lex(self, data: Any, parser_state: Any = None) -> Any:  # type: ignore[override]
        lexer = data
        while True:
            if lexer.next_token is not None:
                tok_type = "ENDFILE" if lexer.next_token == "_ENDFILE_" else lexer.next_token
                lexer.next_token = None
                t = PPToken(tok_type, "", line=lexer.lineno, column=1)
                t.fname = lexer.current_file
                yield t
                continue

            tok = lexer.token()
            if tok is None:
                break

            tok_type = "ENDFILE" if tok.type == "_ENDFILE_" else tok.type
            t = PPToken(
                tok_type, tok.value, line=tok.lineno, column=lexer.find_column(tok) if tok.type != "_ENDFILE_" else 1
            )
            t.fname = tok.fname
            yield t


class ZxbppTransformer(Transformer):
    def start(self, items):
        global OUTPUT
        OUTPUT += "".join(items[0])
        return items[0]

    def program(self, items):
        return items[0]

    def program_tokenstring(self, items):
        tmp = expand_macros(items[0], items[1].line)
        if tmp is None:
            return []
        return [tmp]

    def program_tokenstring_2(self, items):
        return items[0] + [items[1]]

    def program_char(self, items):
        return items[0] + items[1]

    def program_newline(self, items):
        tmp = expand_macros(items[1], items[2].line)
        if tmp is None:
            return []
        res = list(items[0])
        res.append(tmp)
        return res

    def program_newline_2(self, items):
        return items[0] + [f'#line {items[2].line + 1} "{items[2].fname}"\n']

    def token(self, items):
        return items[0]

    def include_file(self, items):
        p0 = [items[0] + items[1]] + items[2] + [items[3]]
        output.CURRENT_FILE.pop()
        global CURRENT_DIR
        CURRENT_DIR = os.path.dirname(output.CURRENT_FILE[-1])
        return p0

    def include_file_empty(self, items):
        return [items[1]]

    def include_once_empty(self, items):
        return [items[1]]

    def include_once_ok(self, items):
        p0 = [items[0] + items[1]] + items[2] + [items[3]]
        output.CURRENT_FILE.pop()
        global CURRENT_DIR
        CURRENT_DIR = os.path.dirname(output.CURRENT_FILE[-1])
        return p0

    def include_fname(self, items):
        modifier = items[1]
        if modifier is None:
            return []
        filename = items[2]
        if ENABLED:
            arch = modifier.get("arch", "")
            return include_file(filename, items[2].line, local_first=False, arch=arch)
        LEXER.next_token = "_ENDFILE_"
        return []

    def include_macro(self, items):
        modifier = items[1]
        if modifier is None:
            return []
        expr = items[2]
        global_first = RE_GLOBAL_FIRST_FILENAME.match(expr)
        local_first = RE_LOCAL_FIRST_FILENAME.match(expr)
        if global_first is None and local_first is None:
            error(items[0].line, f"invalid filename {expr}")
            return []
        if ENABLED:
            arch = modifier.get("arch", "")
            return include_file(expr[1:-1], items[0].line, local_first=local_first is not None, arch=arch)
        LEXER.next_token = "_ENDFILE_"
        return []

    def include_once_str(self, items):
        modifier = items[2]
        if modifier is None:
            return []
        string = items[3]
        p0 = []
        if ENABLED:
            arch = modifier.get("arch", "")
            p0 = include_once(string[1:-1], items[3].line, local_first=True, arch=arch)
        if not p0:
            LEXER.next_token = "_ENDFILE_"
        return p0

    def include_once_fname(self, items):
        modifier = items[2]
        if modifier is None:
            return []
        filename = items[3]
        p0 = []
        if ENABLED:
            arch = modifier.get("arch", "")
            p0 = include_once(filename, items[3].line, local_first=False, arch=arch)
        if not p0:
            LEXER.next_token = "_ENDFILE_"
        return p0

    def include_modifier_empty(self, items):
        return {}

    def include_modifier_arch(self, items):
        modifier = items[1]
        value = items[3]
        if modifier == "arch":
            return {"arch": value}
        error(items[0].line, f"unknown modifier {modifier}")
        return None

    def line(self, items):
        if ENABLED:
            return ["#%s %s%s" % (items[0], items[1], items[2])]
        return []

    def line_file(self, items):
        if ENABLED:
            return ['#%s %s "%s"%s' % (items[0], items[1], items[2], items[3])]
        return []

    def require(self, items):
        return ["#%s %s\n" % (items[0], utils.sanitize_filename(items[1]))]

    def init_id(self, items):
        return ['#%s "%s"\n' % (items[0], items[1])]

    def init_str(self, items):
        return ["#%s %s\n" % (items[0], items[1])]

    def undef(self, items):
        if ENABLED:
            ID_TABLE.undef(items[1])
        return []

    def errormsg(self, items):
        if ENABLED:
            error(items[0].line, items[1])
        return []

    def warningmsg(self, items):
        if ENABLED:
            warning(items[0].line, items[1])
        return []

    def define(self, items):
        id_ = items[1]
        params = items[2]
        defs = items[3]
        if ENABLED:
            if defs:
                if isinstance(defs[0], str) and defs[0] in " \t":
                    defs[0] = defs[0].lstrip(" \t")
                else:
                    output.warning_missing_whitespace_after_macro(items[0].line, LEXER.current_file)
            ID_TABLE.define(
                id_,
                args=params,
                value=defs,
                lineno=items[1].line,
                fname=items[1].fname,
            )
        return []

    def params_epsilon(self, items):
        return None

    def params_empty(self, items):
        return [ID("", value="", args=None, lineno=items[0].line, fname=items[0].fname)]

    def params_paramlist(self, items):
        params = items[1]
        if params is None:
            return None
        for i in params:
            if not isinstance(i, ID):
                error(items[2].line, '"%s" might not appear in a macro parameter list' % str(i))
                return None
        names = [x.name for x in params]
        for i in range(len(names)):
            if names[i] in names[i + 1 :]:
                error(items[2].line, 'Duplicated name parameter "%s"' % (names[i]))
                return None
        return params

    def paramlist_single(self, items):
        return [ID(items[0], value="", args=None, lineno=items[0].line, fname=items[0].fname)]

    def paramlist_paramlist(self, items):
        return items[0] + [ID(items[2], value="", args=None, lineno=items[2].line, fname=items[2].fname)]

    def pragma_id(self, items):
        return ["#%s %s" % (items[0], items[1])]

    def pragma_id_expr(self, items):
        return ["#%s %s %s %s" % (items[0], items[1], items[2], items[3])]

    def pragma_id_string(self, items):
        return ["#%s %s %s %s" % (items[0], items[1], items[2], items[3][1:-1])]

    def pragma_push(self, items):
        return ["#%s %s%s%s%s" % (items[0], items[1], items[2], items[3], items[4])]

    def pragma_once(self, items):
        abs_filename = utils.get_absolute_filename_path(output.CURRENT_FILE[-1])
        if abs_filename not in INCLUDED:
            INCLUDED[abs_filename] = IncludedFileInfo(once=False, parents=[])
        INCLUDED[abs_filename].once = True
        return []

    def ifdef(self, items):
        global ENABLED
        p0 = []
        if ENABLED:
            p0 = [items[1]] + items[2]
        p0 += ['#line %i "%s"' % (items[3].line + 1, items[3].fname)]
        ENABLED = IFDEFS.pop().enabled
        return p0

    def ifdef_else(self, items):
        global ENABLED
        ENABLED = IFDEFS.pop().enabled
        p0 = []
        if ENABLED:
            p0 = items[0] + items[1]
        p0 += ['#line %i "%s"' % (items[2].line + 1, items[2].fname)]
        return p0

    def ifdefelsea(self, items):
        global ENABLED
        p0 = []
        if IFDEFS[-1].enabled:
            if items[0]:
                p0 = [items[1]] + items[2]
            ENABLED = not items[0]
        return p0

    def ifdefelseb(self, items):
        if ENABLED:
            p0 = ['#line %i "%s"%s' % (items[0].line + 1, items[0].fname, items[1])]
            p0 += items[2]
        else:
            p0 = []
        return p0

    def ifdef_header(self, items):
        global ENABLED
        IFDEFS.append(IfDef(ENABLED, items[1].line))
        if ENABLED:
            ENABLED = ID_TABLE.defined(items[1])
        return ENABLED

    def ifndef_header(self, items):
        global ENABLED
        IFDEFS.append(IfDef(ENABLED, items[1].line))
        if ENABLED:
            ENABLED = not ID_TABLE.defined(items[1])
        return ENABLED

    def if_expr_header(self, items):
        global ENABLED
        IFDEFS.append(IfDef(ENABLED, items[1].line if hasattr(items[1], "line") else items[0].line))
        if ENABLED:
            val = items[1]
            ENABLED = bool(int(val)) if (isinstance(val, str) and val.isdigit()) else ID_TABLE.defined(val)
        return ENABLED

    def expr_macrocall(self, items):
        return str(items[0]()).strip()

    def expr_val(self, items):
        return items[0]

    def expr_str(self, items):
        return items[0]

    def expr_par(self, items):
        return items[1]

    def expreq(self, items):
        return "1" if items[0] == items[2] else "0"

    def exprne(self, items):
        return "1" if items[0] != items[2] else "0"

    def exprlt(self, items):
        return "1" if to_int(items[0]) < to_int(items[2]) else "0"

    def exprle(self, items):
        return "1" if to_int(items[0]) <= to_int(items[2]) else "0"

    def exprgt(self, items):
        return "1" if to_int(items[0]) > to_int(items[2]) else "0"

    def exprge(self, items):
        return "1" if to_int(items[0]) >= to_int(items[2]) else "0"

    def exprand(self, items):
        return "1" if to_bool(items[0]) and to_bool(items[2]) else "0"

    def expror(self, items):
        return "1" if to_bool(items[0]) or to_bool(items[2]) else "0"

    def defs_list_eps(self, items):
        return []

    def defs_list(self, items):
        return items[0] + [items[1]]

    def def_item_val(self, items):
        return items[0]

    def def_macrocall(self, items):
        return items[0]

    def macrocall_id(self, items):
        return MacroCall(items[0].fname, items[0].line, ID_TABLE, items[0], None)

    def macrocall_args(self, items):
        return MacroCall(items[0].fname, items[1].end_lineno, ID_TABLE, items[0], items[1])

    def macrocall_paste(self, items):
        return Concatenation(items[0].fname, items[0].lineno, ID_TABLE, items[0], items[2])

    def macrocall_stringizing(self, items):
        return Stringizing(items[1].fname, items[1].lineno, ID_TABLE, items[1])

    def args(self, items):
        arglist = items[1]
        arglist.start_lineno = items[0].line
        arglist.end_lineno = items[2].line
        return arglist

    def arglist_single(self, items):
        al = ArgList(ID_TABLE)
        al.addNewArg(items[0])
        return al

    def arglist(self, items):
        items[0].addNewArg(items[2])
        return items[0]

    def arg_eps(self, items):
        return Arg()

    def arg_val(self, items):
        return items[0]

    def argstring_token_single(self, items):
        return Arg(items[0])

    def argstring_macrocall_single(self, items):
        return Arg(items[0])

    def argstring_argslist(self, items):
        return Arg(items[1])

    def argstring_token(self, items):
        items[0].addToken(items[1])
        return items[0]

    def argstring_macrocall(self, items):
        items[0].addToken(items[1])
        return items[0]

    def argstring_argstring(self, items):
        items[0].addToken(items[2])
        return items[0]


lark_parser = Lark_StandAlone(lexer=LarkLexerAdapter, transformer=ZxbppTransformer())


def parse_with_lark():
    try:
        lark_parser.parse(LEXER)
    except UnexpectedInput as e:
        from .zxbpp_standalone import UnexpectedToken

        if isinstance(e, UnexpectedToken):
            tok = e.token
            if tok.type == "$END":
                if global_.has_errors == 0:
                    error(
                        tok.line,
                        "Syntax error. Unexpected end of file",
                        output.CURRENT_FILE[-1],
                    )
                    global_.has_errors += 1
                return

            if tok.type == "ENDFILE":
                error(
                    tok.line,
                    "Syntax error. Unexpected end of file",
                    output.CURRENT_FILE[-1],
                )
            elif tok.type == "NEWLINE":
                error(
                    tok.line,
                    "Syntax error. Unexpected end of line",
                    output.CURRENT_FILE[-1],
                )
            else:
                value = tok.value
                value = "".join(["|%s|" % hex(ord(x)) if x < " " else x for x in value])
                error(
                    tok.line,
                    "Syntax error. Unexpected token '%s' [%s]" % (value, tok.type),
                    output.CURRENT_FILE[-1],
                )

            # Skip remaining tokens on the same line if error wasn't on newline/endfile
            if tok.type not in ("NEWLINE", "ENDFILE"):
                while True:
                    t = LEXER.token()
                    if t is None or t.type in ("NEWLINE", "_ENDFILE_"):
                        break
        else:
            error(
                e.line,
                "Syntax error. Unexpected input",
                output.CURRENT_FILE[-1],
            )
        global_.has_errors += 1
        parse_with_lark()


def filter_(input_, filename="<internal>", state="INITIAL"):
    """Filter the input string thought the preprocessor.
    result is appended to OUTPUT global str
    """
    global CURRENT_DIR

    prev_dir = CURRENT_DIR
    output.CURRENT_FILE.append(filename)
    CURRENT_DIR = os.path.dirname(output.CURRENT_FILE[-1])
    LEXER.input(input_, filename)
    LEXER.lex.begin(state)
    parse_with_lark()
    output.CURRENT_FILE.pop()
    CURRENT_DIR = prev_dir


def main(argv):
    global OUTPUT, ID_TABLE, ENABLED, CURRENT_DIR

    ENABLED = True
    OUTPUT = ""
    set_include_path()

    if argv:
        output.CURRENT_FILE.append(argv[0])
    else:
        output.CURRENT_FILE.append(global_.FILENAME)

    CURRENT_DIR = os.path.dirname(output.CURRENT_FILE[-1])

    if config.OPTIONS.sinclair:
        included_file = search_filename("sinclair.bas", 0, local_first=False)
        if not included_file:
            return None

        OUTPUT += include_once(included_file, 0, local_first=False)
        if OUTPUT and OUTPUT[-1] != "\n":
            OUTPUT += "\n"

        parse_with_lark()
        output.CURRENT_FILE.pop()
        CURRENT_DIR = os.path.dirname(output.CURRENT_FILE[-1])

    prev_file = global_.FILENAME
    global_.FILENAME = output.CURRENT_FILE[-1]
    OUTPUT += LEXER.include(output.CURRENT_FILE[-1])
    if OUTPUT and OUTPUT[-1] != "\n":
        OUTPUT += "\n"

    parse_with_lark()
    output.CURRENT_FILE.pop()
    global_.FILENAME = prev_file
    return global_.has_errors


parser = lark_parser
parser.defaulted_states = {}


# ------- ERROR And Warning messages ----------------


def entry_point(args=None):
    if args is None:
        args = sys.argv[1:]

    config.init()
    init()
    setMode(PreprocMode.BASIC)

    parser = argparse.ArgumentParser()
    parser.add_argument(
        "-o",
        "--output",
        type=str,
        dest="output_file",
        default=None,
        help="Sets output file. Default is to output to console (STDOUT)",
    )
    parser.add_argument(
        "-d",
        "--debug",
        dest="debug",
        default=config.OPTIONS.debug_level,
        action="count",
        help="Enable verbosity/debugging output. Additional -d increases verbosity/debug level",
    )
    parser.add_argument(
        "-e",
        "--errmsg",
        type=str,
        dest="stderr",
        default=None,
        help="Error messages file. Standard error console by default (STDERR)",
    )
    parser.add_argument(
        "input_file",
        type=str,
        default=None,
        nargs="?",
        help="File to parse. If not specified, console input will be used (STDIN)",
    )
    parser.add_argument(
        "--arch",
        type=str,
        default=AVAILABLE_ARCHITECTURES[0],
        help=f"Target architecture (defaults is'{AVAILABLE_ARCHITECTURES[0]}'). "
        f"Available architectures: {','.join(AVAILABLE_ARCHITECTURES)}",
    )
    parser.add_argument(
        "--expect-warnings",
        default=config.OPTIONS.expected_warnings,
        type=int,
        help="Expects N warnings: first N warnings will be silenced",
    )

    options = parser.parse_args(args=args)
    config.OPTIONS.debug_level = options.debug
    config.OPTIONS.debug_zxbpp = config.OPTIONS.debug_level > 0
    config.OPTIONS.expected_warnings = options.expect_warnings

    if options.arch not in AVAILABLE_ARCHITECTURES:
        parser.error(f"Invalid architecture '{options.arch}'")  # Exits with error

    config.OPTIONS.architecture = options.arch

    if options.stderr:
        config.OPTIONS.stderr_filename = options.stderr
        config.OPTIONS.stderr = utils.open_file(config.OPTIONS.stderr_filename, "wt", "utf-8")

    if options.input_file:
        _, ext = os.path.splitext(options.input_file)
        if ext.lower() == "asm":
            setMode(PreprocMode.ASM)

    result = main([options.input_file] if options.input_file else [])
    if not global_.has_errors:  # ok?
        if options.output_file:
            with utils.open_file(options.output_file, "wt", "utf-8") as output_file:
                output_file.write(OUTPUT)
        else:
            config.OPTIONS.stdout.write(OUTPUT)

    return result


if __name__ == "__main__":
    sys.exit(entry_point())
