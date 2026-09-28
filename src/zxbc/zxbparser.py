#!/usr/bin/env python3

# --------------------------------------------------------------------
# SPDX-License-Identifier: AGPL-3.0-or-later
# © Copyright 2008-2024 José Manuel Rodríguez de la Rosa and contributors.
# See the file CONTRIBUTORS.md for copyright details.
# See https://www.gnu.org/licenses/agpl-3.0.html for details.
# --------------------------------------------------------------------
import math
import sys
from collections.abc import Callable
from math import pi as PI

# typings
from typing import Any, NamedTuple, cast

import src.api.config
import src.api.dataref
import src.api.options
import src.api.utils

# Symbol Classes
from src import arch

# Global containers
from src.api import errmsg, opcodestemps
from src.api import global_ as gl
from src.api.check import (
    check_and_make_label,
    check_class,
    common_type,
    is_dynamic,
    is_ender,
    is_null,
    is_number,
    is_numeric,
    is_static,
    is_static_str,
    is_string,
    is_unsigned,
)
from src.api.config import OPTIONS
from src.api.constants import CLASS, CONVENTION, SCOPE, TYPE, LoopType
from src.api.debug import __DEBUG__
from src.api.errmsg import error, warning
from src.api.global_ import LoopInfo

# Compiler API
from src.api.symboltable.symboltable import SymbolTable

# Lexers and parsers, etc
from src.symbols import sym
from src.symbols.id_ import SymbolID
from src.symbols.symbol_ import Symbol
from src.symbols.type_ import Type
from src.zxbc import zxblex
from src.zxbc.zxblex import tokens  # noqa

from .zxbparser_standalone import Lark_StandAlone, Lexer, Token, Transformer, UnexpectedInput, v_args

# ----------------------------------------------------------------------
# Function level entry ID in which scope we are into. If the list
# is empty, we are at global scope
# ----------------------------------------------------------------------
FUNCTION_LEVEL: list[SymbolID] = gl.FUNCTION_LEVEL

# ----------------------------------------------------------------------
# Function calls pending to check
# Each scope pushes (prepends) an empty list
# ----------------------------------------------------------------------
FUNCTION_CALLS = gl.FUNCTION_CALLS

# ----------------------------------------------------------------------
# Initialization routines to be called automatically at program start
# ----------------------------------------------------------------------
INITS = gl.INITS

# ----------------------------------------------------------------------
# Global Symbol Table
# ----------------------------------------------------------------------
SYMBOL_TABLE = gl.SYMBOL_TABLE = SymbolTable()

# ----------------------------------------------------------------------
# Defined user labels. They all are prepended _label_. Line numbers 10,
# 20, 30... are in the form: __label_10, __label_20, __label_30...
# ----------------------------------------------------------------------
LABELS = {}

# ----------------------------------------------------------------------
# True if we're in the middle of a LET sentence. False otherwise.
# ----------------------------------------------------------------------
LET_ASSIGNMENT: bool = False

# ----------------------------------------------------------------------
# True if PRINT sentence has been used.
# ----------------------------------------------------------------------
PRINT_IS_USED: bool = False

# ----------------------------------------------------------------------
# Last line number output for checking program key board BREAK
# ----------------------------------------------------------------------
last_brk_linenum: int = 0


# ----------------------------------------------------------------------
# Start of parsing
# ----------------------------------------------------------------------


class Id(NamedTuple):
    """Encapsulates an ID name and its line number where it was read"""

    name: str
    lineno: int


def init():
    """Initializes parser state"""
    global LABELS
    global LET_ASSIGNMENT
    global PRINT_IS_USED
    global SYMBOL_TABLE

    global ast
    global data_ast
    global optemps
    global last_brk_linenum

    LABELS = {}
    LET_ASSIGNMENT = False
    PRINT_IS_USED = False
    last_brk_linenum = 0

    ast = None
    data_ast = None  # Global Variables AST
    opcodestemps.init()
    optemps = opcodestemps.OpcodesTemps()

    gl.INITS.clear()
    gl.syntax_error_occurred = False
    gl.tokens_yielded = 0
    gl.tokens_rejected = 0
    gl.shifted_at_last_error = 0
    gl.expr_linenos = {}
    del gl.FUNCTION_CALLS[:]
    del gl.FUNCTION_LEVEL[:]
    del gl.FUNCTIONS[:]
    SYMBOL_TABLE = gl.SYMBOL_TABLE = src.api.symboltable.symboltable.SymbolTable()

    # DATAs info
    gl.DATA_LABELS_REQUIRED.clear()
    gl.DATA_LABELS.clear()
    gl.DATA_IS_USED = False
    del gl.DATAS[:]
    gl.DATA_PTR_CURRENT = src.api.utils.current_data_label()
    gl.DATA_FUNCTIONS = []
    gl.error_msg_cache.clear()


# ----------------------------------------------------------------------
# "Macro" functions. Just return more complex expressions
# ----------------------------------------------------------------------
def _TYPE(type_: TYPE) -> sym.TYPE | None:
    """returns an internal type converted to a SYMBOL_TABLE type."""
    assert isinstance(type_, TYPE)
    return SYMBOL_TABLE.basic_types[type_]


def _TYPEREF(type_: TYPE, *, implicit: bool = True) -> sym.TYPEREF:
    """Returns a typing annotation"""
    return sym.TYPEREF(_TYPE(type_), 0, implicit=implicit)


# ----------------------------------------------------------------------
# Utils
# ----------------------------------------------------------------------
def mark_entry_as_accessed(entry: sym.ID):
    """Marks the entry as accessed (needed) only if in the global
    scope
    """
    assert isinstance(entry, sym.ID)
    if FUNCTION_LEVEL and entry.token == "FUNCTION":  # Not in global scope
        return
    entry.accessed = True


def convert_to_function(entry: sym.ID, class_: CLASS, lineno: int):
    """The given entry is converted to function if it is not already."""
    assert class_ in (CLASS.function, CLASS.sub)

    if check_class(entry, class_, lineno):
        if entry.class_ == class_:
            return

        entry.to_function(lineno, class_=class_)


# ----------------------------------------------------------------------
# Wrapper functions to make AST nodes
# ----------------------------------------------------------------------
def make_nop():
    """NOP does nothing."""
    return sym.NOP()


def make_number(value, lineno: int, type_=None):
    if hasattr(value, "original_value"):
        value = value.original_value
    elif isinstance(value, str):
        try:
            if "." in value or "e" in value.lower():
                value = float(value)
            else:
                value = int(value, 0)
        except ValueError:
            value = float(value)
    return sym.NUMBER(value, type_=type_, lineno=lineno)


def make_typecast(type_: sym.TYPING, node: sym.EXPR | None, lineno: int) -> sym.TYPECAST | sym.EXPR | None:
    """Wrapper: returns a Typecast node"""
    if node is None or node.type_ is None:
        return None  # syntax / semantic error

    if isinstance(type_, sym.TYPEREF):
        type_ = type_.type_

    assert isinstance(type_, sym.TYPE)

    result = sym.TYPECAST.make_node(type_, node, lineno)
    assert isinstance(result, None | sym.TYPECAST | sym.EXPR), f"{result.__class__.__name__} != TYPECAST | EXPR"

    return result


def make_binary(lineno: int, operator, left, right, func=None, type_=None):
    """Wrapper: returns a Binary node"""
    return sym.BINARY.make_node(operator, left, right, lineno, func, type_)


def make_unary(
    lineno: int,
    operator: str,
    operand: sym.EXPR | None,
    func=Callable,
    type_: sym.TYPE | None = None,
) -> sym.UNARY | sym.NUMBER | sym.STRING | None:
    """Wrapper: returns a Unary node"""
    if operand is None:  # syntax / semantic error
        return None

    return sym.UNARY.make_node(lineno, operator, operand, func, type_)


def make_builtin(
    lineno: int,
    fname: str,
    operands: Symbol | tuple | list | None,
    func: Callable | None = None,
    type_: sym.TYPE | None = None,
) -> sym.BUILTIN | sym.NUMBER:
    """Wrapper: returns a Builtin function node.
    Can be a Symbol, tuple, or list of Symbols
    If operand is an iterable, they will be expanded.
    """
    if operands is None:
        operands = []

    assert isinstance(operands, Symbol | tuple | list)
    assert isinstance(type_, sym.TYPE | None)

    # TODO: In the future, builtin functions will be implemented in an external stdlib, like POINT or ATTR
    __DEBUG__(f'Creating BUILTIN "{fname}"', 1)
    if not isinstance(operands, list | tuple):
        operands = [operands]

    return sym.BUILTIN.make_node(lineno, fname, func, type_, *operands)


def make_constexpr(lineno, expr):
    return sym.CONSTEXPR(expr, lineno=lineno)


def make_strslice(lineno: int, s, lower, upper):
    """Wrapper: returns String Slice node"""
    return sym.STRSLICE.make_node(lineno, s, lower, upper)


def make_sentence(lineno: int, sentence: str, *args, sentinel=False):
    """Wrapper: returns a Sentence node"""
    return sym.SENTENCE(lineno, gl.FILENAME, sentence, *args, is_sentinel=sentinel)


def make_asm_sentence(asm: str, lineno: int, sentinel: bool = False):
    """Creates a node for an ASM inline sentence"""
    return sym.ASM(asm, lineno, gl.FILENAME, is_sentinel=sentinel)


def make_block(*args):
    """Wrapper: Creates a chain of code blocks."""
    return sym.BLOCK.make_node(*args)


def make_var_declaration(entry):
    """This will return a node with a var declaration.
    The children node contains the symbol table entry.
    """
    return sym.VARDECL(entry)


def make_array_declaration(entry: sym.ID) -> sym.ARRAYDECL:
    """This will return a node with the symbol as an array."""
    return sym.ARRAYDECL(entry)


def make_func_declaration(
    func_name: str,
    lineno: int,
    class_: CLASS,
    type_: sym.TYPEREF | None = None,
) -> sym.FUNCDECL | None:
    """This will return a node with the symbol as a function or sub."""
    return sym.FUNCDECL.make_node(func_name, lineno, class_, type_=type_)


def make_arg_list(node, *args):
    """Wrapper: returns a node with an argument_list."""
    result = sym.ARGLIST.make_node(node, *args)
    return result


def make_argument(expr, lineno: int, byref=None, name: str = None):
    """Wrapper: Creates a node containing an ARGUMENT"""
    if expr is None:
        return None  # There were a syntax / semantic error

    if byref is None:
        byref = OPTIONS.default_byref
    return sym.ARGUMENT(expr, lineno=lineno, byref=byref, name=name)


def make_param_list(node, *args):
    """Wrapper: Returns a param declaration list (function header)"""
    return sym.PARAMLIST.make_node(node, *args)


def make_sub_call(id_, lineno, arg_list):
    """This will return an AST node for a sub/procedure call."""
    return sym.CALL.make_node(id_, arg_list, lineno, gl.FILENAME)


def make_func_call(id_, lineno, arg_list):
    """This will return an AST node for a function call."""
    return sym.FUNCCALL.make_node(id_, arg_list, lineno, gl.FILENAME)


def make_array_access(id_, lineno, arglist):
    """Creates an array access. A(x1, x2, ..., xn).
    This is an RVALUE (Read the element)
    """
    for i, arg in enumerate(arglist):
        value = make_typecast(
            Type.by_name(src.api.constants.TYPE.to_string(gl.BOUND_TYPE)),
            arg.value,
            arg.lineno,
        )
        if value is None:  # semantic error?
            return None  # return error
        arg.value = value

    return sym.ARRAYACCESS.make_node(id_, arglist, lineno, gl.FILENAME)


def make_array_substr_assign(lineno: int, id_: str, arg_list, substr, expr_) -> sym.SENTENCE | None:
    if arg_list is None or substr is None or expr_ is None:
        return None  # There were errors

    entry = SYMBOL_TABLE.access_call(id_, lineno)
    if entry is None:
        return None  # There were errors

    if entry.type_ != Type.string:
        error(lineno, "Array '%s' is not of type String" % id_)
        return None  # There were errors

    arr = make_array_access(id_, lineno, arg_list)
    if arr is None:
        return None  # There were errors

    expr_ = make_typecast(arr.type_, expr_, lineno)
    if expr_ is None:
        return None  # There were errors

    str_idx_type = _TYPE(gl.STR_INDEX_TYPE)
    s0 = make_typecast(str_idx_type, substr[0], lineno)
    if s0 is None:
        return None  # There were errors

    s1 = make_typecast(str_idx_type, substr[1], lineno)
    if s1 is None:
        return None  # There were errors

    if OPTIONS.string_base:
        base = make_number(OPTIONS.string_base, lineno, _TYPE(gl.STR_INDEX_TYPE))
        s0 = make_binary(lineno, "MINUS", s0, base, func=lambda x, y: x - y)
        s1 = make_binary(lineno, "MINUS", s1, base, func=lambda x, y: x - y)

    return make_sentence(lineno, "LETARRAYSUBSTR", arr, s0, s1, expr_)


def make_call(id_: str, lineno: int, args: sym.ARGLIST):
    """This will return an AST node for a function call/array access.

    A "call" is just an ID followed by a list of arguments.
    E.g. a(4)
    - a(4) can be a function call if 'a' is a function
    - a(4) can be a string slice if 'a' is a string variable: a$(4)
    - a(4) can be an access to an array if 'a' is an array

    This function will inspect the id_. If it is undeclared, then
    id_ will be taken as a forwarded function.
    """
    if args is None:
        return None

    assert isinstance(args, sym.ARGLIST)

    entry = SYMBOL_TABLE.access_call(id_, lineno)
    if entry is None:
        return None

    if entry.class_ is CLASS.unknown and entry.type_ == Type.string and len(args) == 1 and is_numeric(args[0]):
        entry = entry.to_var()  # A scalar variable. e.g a$(expr)

    if entry.class_ == CLASS.array:  # An already declared array
        arr = sym.ARRAYLOAD.make_node(id_, args, lineno, gl.FILENAME)
        if arr is None:
            return None

        if arr.offset is not None:
            offset = make_typecast(Type.uinteger, make_number(arr.offset, lineno=lineno), lineno)
            arr.append_child(offset)
        return arr

    if entry.class_ in (CLASS.var, CLASS.const):  # An already declared/used string var
        if len(args) > 1:
            errmsg.syntax_error_not_array_nor_func(lineno, id_)
            return None

        if entry.class_ == CLASS.var:
            entry = SYMBOL_TABLE.access_var(id_, lineno)
            if entry is None:
                return None

        if len(args) == 1:
            if entry.class_ == CLASS.var:
                return make_strslice(lineno, entry, args[0].value, args[0].value)
            # it's a const
            return make_strslice(lineno, sym.STRING(entry.value, lineno), args[0].value, args[0].value)

        mark_entry_as_accessed(entry)
        return entry

    return make_func_call(id_, lineno, args)


def make_param_decl(
    id_: str,
    lineno: int,
    typedef: sym.TYPEREF,
    *,
    is_array: bool,
    default_value: sym.SYMBOL | None = None,
):
    """Wrapper that creates a param declaration"""
    return SYMBOL_TABLE.declare_param(id_, lineno, typedef, default_value, is_array=is_array)


def make_type(typename, lineno, implicit=False):
    """Converts a typename identifier (e.g. 'float') to
    its internal symbol table entry representation.

    Creates a type usage symbol stored in a AST
    E.g. DIM a As Integer
    will access Integer type
    """
    assert isinstance(typename, str)
    if not SYMBOL_TABLE.check_is_declared(typename, lineno, "type"):
        return None

    type_ = sym.TYPEREF(SYMBOL_TABLE.get_entry(typename), lineno, implicit=implicit)
    return type_


def make_bound(lower, upper, lineno):
    """Wrapper: Creates an array bound"""
    return sym.BOUND.make_node(lower, upper, lineno)


def make_bound_list(node, *args):
    """Wrapper: Creates an array BOUND LIST."""
    return sym.BOUNDLIST.make_node(node, *args)


def make_label(id_: str, lineno: int):
    """Creates a label entry. Returns None on error."""
    id_ = str(id_)  # Labels can be numbers and must be converted to strings
    entry = SYMBOL_TABLE.declare_label(id_, lineno)
    if entry:
        gl.DATA_LABELS[id_] = gl.DATA_PTR_CURRENT  # This label points to the current DATA block index
    return entry


def make_break(lineno: int, p):
    """Checks if --enable-break is set, and if so, calls
    BREAK keyboard interruption for this line if it has not been already
    checked"""
    global last_brk_linenum

    if not OPTIONS.enable_break or lineno == last_brk_linenum or is_null(p):
        return None

    last_brk_linenum = lineno
    return make_sentence(lineno, "CHKBREAK", make_number(lineno, lineno, Type.uinteger))


# ----------------------------------------------------------------------
# Operators precedence
# ----------------------------------------------------------------------
precedence = (
    ("nonassoc", "ID", "ARRAY_ID"),
    ("left", "OR"),
    ("left", "AND"),
    ("left", "XOR"),
    ("right", "NOT"),
    ("left", "LT", "GT", "EQ", "LE", "GE", "NE"),
    ("left", "BOR"),
    ("left", "BAND", "BXOR", "SHR", "SHL"),
    ("left", "BNOT", "PLUS", "MINUS"),
    ("left", "MOD"),
    ("left", "MUL", "DIV"),
    ("right", "UMINUS"),
    ("right", "POW"),
    ("left", "RP"),
    ("right", "LP"),
    ("right", "ELSE"),
    ("left", "CO"),
    ("left", "LABEL"),
    ("left", "NEWLINE"),
)


# ----------------------------------------------------------------------


def get_lineno(item):
    if hasattr(gl, "expr_linenos") and id(item) in gl.expr_linenos:
        return gl.expr_linenos[id(item)]
    if hasattr(item, "line"):
        return item.line
    if hasattr(item, "lineno"):
        return item.lineno
    if hasattr(item, "children") and item.children:
        for child in item.children:
            l = get_lineno(child)
            if l:
                return l
    return zxblex.lexer.lineno if hasattr(zxblex, "lexer") else 0


def meta_line(meta):
    return getattr(meta, "line", None) or (zxblex.lexer.lineno if hasattr(zxblex, "lexer") else 0) or 0


class ZXBasicToken(Token):
    def __eq__(self, other):
        return str.__eq__(self, other)

    def __hash__(self):
        return str.__hash__(self)


class ZXBasicLarkLexerAdapter(Lexer):
    def __init__(self, lexer_conf: Any) -> None:
        pass

    def lex(self, data: Any, parser_state: Any = None) -> Any:
        lexer = data
        lookahead = []

        def get_next():
            tok = lexer.token()
            if tok is None:
                return None
            tok.lexer = lexer
            col = zxblex.find_column(tok) if hasattr(zxblex, "find_column") else 1
            val = tok.value
            if isinstance(val, float) and val.is_integer():
                val = str(int(val))
            elif isinstance(val, (int, float)):
                val = str(val)
            t = ZXBasicToken(tok.type, val, line=tok.lineno, column=col)
            t.original_value = tok.value
            return t

        while True:
            if lookahead:
                t = lookahead.pop(0)
            else:
                t = get_next()
                if t is None:
                    break

            if hasattr(gl, "tokens_yielded"):
                gl.tokens_yielded += 1

            if t.type == "END":
                next_t = get_next()
                if next_t is not None:
                    if next_t.type in ("SUB", "FUNCTION", "IF", "WHILE"):
                        merged_type = "END_" + next_t.type
                        merged_value = t.value + " " + next_t.value
                        merged_t = ZXBasicToken(merged_type, merged_value, line=t.line, column=t.column)
                        merged_t.original_value = merged_value
                        yield merged_t
                        continue
                    else:
                        lookahead.append(next_t)

            yield t


@v_args(meta=True)
class ZXBasicTransformer(Transformer):
    def start(self, meta, items):
        p0 = None
        global ast, data_ast
        make_label(gl.ZXBASIC_USER_DATA, 0)
        make_label(gl.ZXBASIC_USER_DATA_LEN, 0)
        if PRINT_IS_USED:
            getattr(OPTIONS, "__DEFINES")["___PRINT_IS_USED___"] = 1
        if zxblex.IN_STATE:
            error(meta.line, "Unexpected end of line")
            sys.exit(1)
        ast = p0 = items[0]
        __end = make_sentence(meta_line(meta), "END", make_number(0, lineno=meta_line(meta)), sentinel=True)
        if not is_null(ast):
            if isinstance(ast, sym.BLOCK) and (not is_ender(ast[-1])):
                ast.append_child(__end)
        else:
            ast = __end
        SYMBOL_TABLE.check_labels()
        SYMBOL_TABLE.check_classes()
        if gl.has_errors:
            return p0
        __DEBUG__("Checking pending labels", 1)
        if not src.api.check.check_pending_labels(ast):
            return p0
        __DEBUG__("Checking pending calls", 1)
        if not src.api.check.check_pending_calls():
            return p0
        data_ast = make_sentence(meta_line(meta), "BLOCK")
        for var in SYMBOL_TABLE.vars_:
            data_ast.append_child(make_var_declaration(var))
        for var in SYMBOL_TABLE.arrays:
            data_ast.append_child(make_array_declaration(var))
        return p0

    def program_program_line(self, meta, items):
        p0 = None
        p0 = make_block(items[0], make_break(get_lineno(items[0]), items[0]))
        return p0

    def program(self, meta, items):
        p0 = None
        p0 = make_block(items[0], items[1], make_break(get_lineno(items[1]), items[1]))
        return p0

    def program_line(self, meta, items):
        p0 = None
        p0 = make_nop() if len(items) + 1 == 2 else items[0]
        return p0

    def co_statements_co(self, meta, items):
        p0 = None
        p0 = items[0] if len(items) + 1 == 3 else make_nop()
        return p0

    def co_statements(self, meta, items):
        p0 = None
        p0 = make_block(items[0], items[1])
        return p0

    def statements_co(self, meta, items):
        p0 = None
        p0 = items[0]
        return p0

    def statements_statement(self, meta, items):
        p0 = None
        if len(items) + 1 == 2:
            p0 = make_block(items[0])
        else:
            p0 = make_block(items[0], items[1])
        return p0

    def var_decls(self, meta, items):
        p0 = None
        p0 = items[0]
        return p0

    def label(self, meta, items):
        p0 = None
        p0 = make_label(items[0], get_lineno(items[0]))
        return p0

    def program_line_label(self, meta, items):
        p0 = None
        lbl = items[0]
        p0 = make_block(lbl, items[1]) if len(items) + 1 == 3 else lbl
        return p0

    def label_line_label_line_co(self, meta, items):
        p0 = None
        p0 = items[0]
        return p0

    def label_line_co(self, meta, items):
        p0 = None
        lbl = items[0]
        p0 = make_block(lbl, items[1]) if len(items) + 1 == 3 else lbl
        return p0

    def program_line_co(self, meta, items):
        p0 = None
        p0 = items[0] if len(items) + 1 == 2 else make_block(items[0], items[1])
        return p0

    def var_decl(self, meta, items):
        p0 = None
        for vardata in items[1]:
            SYMBOL_TABLE.declare_variable(vardata[0], vardata[1], items[2])
        p0 = None
        return p0

    def var_decl_at(self, meta, items):
        p0 = None
        p0 = None
        if items[1] is None or items[2] is None or items[4] is None:
            return p0
        if len(items[1]) != 1:
            error(get_lineno(items[0]), "Only one variable at a time can be declared this way")
            return p0
        idlist = items[1][0]
        entry = SYMBOL_TABLE.declare_variable(idlist[0], idlist[1], items[2])
        if entry is None:
            return p0
        if items[4].token == "CONSTEXPR":
            tmp = items[4].expr
            entry.addr = tmp
        elif not is_static(items[4]):
            errmsg.syntax_error_address_must_be_constant(get_lineno(items[3]))
            return p0
        else:
            entry.addr = make_typecast(_TYPE(gl.PTR_TYPE), items[4], get_lineno(items[3]))
            mark_entry_as_accessed(entry)
            if entry.scope == SCOPE.local:
                SYMBOL_TABLE.make_static(entry.name)
        return p0

    def var_decl_ini(self, meta, items):
        p0 = None
        keyword, idlist, typedef, expr = (items[0], items[1], items[2], items[4])
        if len(idlist) != 1:
            error(get_lineno(items[0]), "Initialized variables must be declared one by one.")
            return p0

        if expr is None:
            return p0

        if is_static(expr) and isinstance(expr, sym.UNARY):
            expr = make_constexpr(get_lineno(items[3]), expr)

        if typedef.implicit:
            typedef = sym.TYPEREF(expr.type_, meta_line(meta), implicit=True)
        value = make_typecast(typedef.type_, expr, get_lineno(items[3]))
        defval = value if is_static(expr) and value.type_ != Type.string else None

        if keyword == "DIM":
            SYMBOL_TABLE.declare_variable(idlist[0].name, idlist[0].lineno, typedef, default_value=defval)
        else:
            if defval is None:
                if not is_static_str(value):
                    errmsg.syntax_error_not_constant(get_lineno(items[3]))
                    return p0
                defval = value
            SYMBOL_TABLE.declare_const(idlist[0].name, idlist[0].lineno, typedef, default_value=defval)

        if defval is None:
            p0 = make_sentence(
                get_lineno(items[0]), "LET", SYMBOL_TABLE.access_var(idlist[0].name, get_lineno(items[0])), value
            )
        return p0

    def singleid(self, meta, items):
        p0 = Id(name=items[0], lineno=get_lineno(items[0]))
        return p0

    def idlist_id(self, meta, items):
        p0 = [items[0]]
        return p0

    def idlist_idlist_id(self, meta, items):
        items[0].append(items[2])
        p0 = items[0]
        return p0

    def arr_decl(self, meta, items):
        p0 = None
        return p0

    def arr_decl_attr(self, meta, items):
        p0 = None
        arr_decl, expr = (items[0], items[2])
        if arr_decl is None or expr is None:
            p0 = None
            return p0
        if expr.token == "CONSTEXPR":
            expr = expr.expr
            if expr.token == "UNARY" and expr.operator == "ADDRESS":
                if expr.operand.token == "ARRAYACCESS":
                    if expr.operand.offset is None:
                        error(get_lineno(items[3]), "Address is not constant. Only constant subscripts are allowed")
                        return p0
                else:
                    if expr.operand.token not in ("ID", "VAR", "LABEL"):
                        error(get_lineno(items[2]), "Only addresses of identifiers are allowed")
                        return p0
                    expr.operand.has_address = True
        elif not is_static(expr):
            errmsg.syntax_error_address_must_be_constant(get_lineno(items[2]))
            return p0
        arr_entry = SYMBOL_TABLE.access_array(arr_decl[0], arr_decl[1])
        arr_entry.addr = make_typecast(_TYPE(gl.PTR_TYPE), expr, get_lineno(items[1]))
        if arr_entry.scope == SCOPE.local:
            SYMBOL_TABLE.make_static(arr_entry.name)
        p0 = items[0]
        return p0

    def decl_arr(self, meta, items):
        p0 = None
        if len(items[1]) != 1:
            error(get_lineno(items[0]), "Array declaration only allows one variable name at a time")
        else:
            id_, lineno = items[1][0]
            SYMBOL_TABLE.declare_array(id_, lineno, items[5], items[3])
        p0 = items[1][0]
        return p0

    def arr_decl_initialized(self, meta, items):

        def check_bound(boundlist, remaining):
            lineno = get_lineno(items[6])
            if not boundlist:
                if not isinstance(remaining, list):
                    return True
                error(lineno, "Unexpected extra vector dimensions. It should be %i" % len(remaining))
                return False
            if not isinstance(remaining, list):
                error(lineno, "Mismatched vector size. Missing %i extra dimension(s)" % len(boundlist))
                return False
            if len(remaining) != boundlist[0].count:
                error(
                    lineno,
                    "Mismatched vector size. Expected %i elements, got %i." % (boundlist[0].count, len(remaining)),
                )
                return False
            for row in remaining:
                if not check_bound(boundlist[1:], row):
                    return False
            return True

        p0 = None

        if items[3] is None or items[5] is None or items[7] is None:
            return p0

        if not check_bound(items[3].children, items[7]):
            return p0

        id_, lineno = items[1][0]
        SYMBOL_TABLE.declare_array(id_, lineno, items[5], items[3], default_value=items[7])
        entry = SYMBOL_TABLE.get_entry(id_)
        if entry is None:
            return p0

        if items[5] == Type.string or entry.type_ == Type.string:
            errmsg.syntax_error_cannot_initialize_array_of_type(get_lineno(items[0]), Type.string)
            return p0

        return p0

    def bound_list(self, meta, items):
        p0 = make_bound_list(items[0])
        return p0

    def bound_list_bound(self, meta, items):
        p0 = make_bound_list(items[0], items[2])
        return p0

    def bound(self, meta, items):
        p0 = make_bound(make_number(OPTIONS.array_base, lineno=get_lineno(items[0])), items[0], meta_line(meta))
        return p0

    def bound_to_bound(self, meta, items):
        p0 = make_bound(items[0], items[2], get_lineno(items[1]))
        return p0

    def const_vector(self, meta, items):
        p0 = items[1]
        return p0

    def const_vector_elem_list(self, meta, items):
        p0 = None
        if items[0] is None:
            return p0

        if not is_static(items[0]):
            if isinstance(items[0], sym.UNARY):
                tmp = make_constexpr(get_lineno(items[0]), items[0])
            else:
                errmsg.syntax_error_not_constant(meta_line(meta))
                return p0
        else:
            tmp = items[0]

        p0 = [tmp]
        return p0

    def const_vector_elem_list_list(self, meta, items):
        p0 = None
        if items[0] is None or items[2] is None:
            return p0
        if not is_static(items[2]):
            if isinstance(items[2], sym.UNARY):
                tmp = make_constexpr(get_lineno(items[1]), items[2])
            else:
                errmsg.syntax_error_not_constant(get_lineno(items[1]))
                p0 = None
                return p0
        else:
            tmp = items[2]
        if items[0] is not None:
            items[0].append(tmp)
        p0 = items[0]
        return p0

    def const_vector_list(self, meta, items):
        p0 = [items[0]]
        return p0

    def const_vector_vector_list(self, meta, items):
        if len(items[2]) != len(items[0][0]):
            error(get_lineno(items[1]), "All rows must have the same number of elements")
            p0 = None
            return p0
        p0 = items[0] + [items[2]]
        return p0

    def staement_func_decl(self, meta, items):
        p0 = items[0]
        return p0

    def statement_border(self, meta, items):
        p0 = make_sentence(get_lineno(items[0]), "BORDER", make_typecast(Type.ubyte, items[1], get_lineno(items[0])))
        return p0

    def statement_plot(self, meta, items):
        p0 = make_sentence(
            get_lineno(items[0]),
            "PLOT",
            make_typecast(Type.ubyte, items[1], get_lineno(items[2])),
            make_typecast(Type.ubyte, items[3], get_lineno(items[2])),
        )
        return p0

    def statement_plot_attr(self, meta, items):
        p0 = make_sentence(
            get_lineno(items[0]),
            "PLOT",
            make_typecast(Type.ubyte, items[2], get_lineno(items[3])),
            make_typecast(Type.ubyte, items[4], get_lineno(items[3])),
            items[1],
        )
        return p0

    def statement_draw3(self, meta, items):
        p0 = make_sentence(
            get_lineno(items[0]),
            "DRAW3",
            make_typecast(Type.integer, items[1], get_lineno(items[2])),
            make_typecast(Type.integer, items[3], get_lineno(items[4])),
            make_typecast(Type.float_, items[5], get_lineno(items[4])),
        )
        return p0

    def statement_draw3_attr(self, meta, items):
        p0 = make_sentence(
            get_lineno(items[0]),
            "DRAW3",
            make_typecast(Type.integer, items[2], get_lineno(items[3])),
            make_typecast(Type.integer, items[4], get_lineno(items[5])),
            make_typecast(Type.float_, items[6], get_lineno(items[5])),
            items[1],
        )
        return p0

    def statement_draw(self, meta, items):
        p0 = make_sentence(
            get_lineno(items[0]),
            "DRAW",
            make_typecast(Type.integer, items[1], get_lineno(items[2])),
            make_typecast(Type.integer, items[3], get_lineno(items[2])),
        )
        return p0

    def statement_draw_attr(self, meta, items):
        p0 = make_sentence(
            get_lineno(items[0]),
            "DRAW",
            make_typecast(Type.integer, items[2], get_lineno(items[3])),
            make_typecast(Type.integer, items[4], get_lineno(items[3])),
            items[1],
        )
        return p0

    def statement_circle(self, meta, items):
        p0 = make_sentence(
            get_lineno(items[0]),
            "CIRCLE",
            make_typecast(Type.byte_, items[1], get_lineno(items[2])),
            make_typecast(Type.byte_, items[3], get_lineno(items[4])),
            make_typecast(Type.byte_, items[5], get_lineno(items[4])),
        )
        return p0

    def statement_circle_attr(self, meta, items):
        p0 = make_sentence(
            get_lineno(items[0]),
            "CIRCLE",
            make_typecast(Type.byte_, items[2], get_lineno(items[3])),
            make_typecast(Type.byte_, items[4], get_lineno(items[5])),
            make_typecast(Type.byte_, items[6], get_lineno(items[5])),
            items[1],
        )
        return p0

    def statement_cls(self, meta, items):
        p0 = make_sentence(get_lineno(items[0]), "CLS")
        return p0

    def statement_asm(self, meta, items):
        p0 = make_asm_sentence(items[0], get_lineno(items[0]))
        return p0

    def statement_randomize(self, meta, items):
        p0 = make_sentence(
            get_lineno(items[0]), "RANDOMIZE", make_number(0, lineno=get_lineno(items[0]), type_=Type.ulong)
        )
        return p0

    def statement_randomize_expr(self, meta, items):
        p0 = make_sentence(get_lineno(items[0]), "RANDOMIZE", make_typecast(Type.ulong, items[1], get_lineno(items[0])))
        return p0

    def statement_beep(self, meta, items):
        p0 = make_sentence(
            get_lineno(items[0]),
            "BEEP",
            make_typecast(Type.float_, items[1], get_lineno(items[0])),
            make_typecast(Type.float_, items[3], get_lineno(items[2])),
        )
        return p0

    def statement_call(self, meta, items):
        if len(items) + 1 > 2 and items[1] is None:
            p0 = None
        elif len(items) + 1 == 2:
            entry = SYMBOL_TABLE.get_entry(items[0])
            if entry is not None and entry.class_ in (CLASS.label, CLASS.unknown):
                p0 = make_label(items[0], get_lineno(items[0]))
            else:
                p0 = make_sub_call(items[0], get_lineno(items[0]), make_arg_list(None))
        else:
            p0 = make_sub_call(items[0], get_lineno(items[0]), items[1])

        return p0

    def assignment(self, meta, items):
        global LET_ASSIGNMENT
        LET_ASSIGNMENT = False
        p0 = None
        q = items[0:]
        i = 1
        if q[1] is None:
            return p0

        if q[1].token == "VAR" and q[1].class_ == CLASS.unknown:
            q[1] = SYMBOL_TABLE.access_var(q[1].name, get_lineno(items[i - 1]))

        q1class_ = q[1].class_ if q[1].token == "VAR" else CLASS.unknown
        variable = SYMBOL_TABLE.access_id(
            q[0], get_lineno(items[i - 1]), default_type=q[1].type_, default_class=q1class_
        )

        if variable is None:
            return p0

        if variable.class_ == CLASS.unknown:
            variable = variable.to_var()

        if variable.class_ not in (CLASS.var, CLASS.array):
            errmsg.syntax_error_cannot_assign_not_a_var(get_lineno(items[i - 1]), variable.name)
            return p0

        if variable.class_ == CLASS.var and q1class_ == CLASS.array:
            error(get_lineno(items[i - 1]), "Cannot assign an array to an scalar variable")
            return p0

        expr = make_typecast(variable.type_, q[1], get_lineno(items[i - 1]))
        p0 = make_sentence(get_lineno(items[0]), "LET", variable, expr)
        return p0

    def lexpr(self, meta, items):
        global LET_ASSIGNMENT
        LET_ASSIGNMENT = True
        if items[0] == "LET":
            p0 = items[1]
            i = 2
        else:
            p0 = items[0]
            i = 1

        SYMBOL_TABLE.access_id(items[i - 1], get_lineno(items[i - 1]))
        return p0

    def array_copy(self, meta, items):
        p0 = None
        if items[0] == "LET":
            array_id1, array_id2 = (items[1], items[3])
            l1, l2 = (get_lineno(items[1]), get_lineno(items[3]))
        else:
            array_id1, array_id2 = (items[0], items[2])
            l1, l2 = (get_lineno(items[0]), get_lineno(items[2]))

        larray = SYMBOL_TABLE.access_id(array_id1, l1)
        rarray = SYMBOL_TABLE.access_id(array_id2, l2)

        if larray is None or rarray is None:
            p0 = None
            return p0

        if larray.type_ != rarray.type_:
            error(l1, "Arrays must have the same element type")
            return p0

        if larray.ref.memsize != rarray.ref.memsize:
            error(l1, "Arrays '%s' and '%s' must have the same size" % (array_id1, array_id2))
            return p0

        if larray.ref.count != rarray.ref.count:
            warning(l1, "Arrays '%s' and '%s' don't have the same number of dimensions" % (larray.name, rarray.name))
        else:
            for b1, b2 in zip(larray.ref.bounds, rarray.ref.bounds):
                if b1.count != b2.count:
                    warning(l1, "Arrays '%s' and '%s' don't have the same dimensions" % (array_id1, array_id2))
                    break

        mark_entry_as_accessed(larray)
        mark_entry_as_accessed(rarray)
        p0 = make_sentence(get_lineno(items[0]), "ARRAYCOPY", larray, rarray)
        return p0

    def arr_assignment(self, meta, items):
        i = 2 if items[0].upper() == "LET" else 1
        id_ = items[i - 1]
        arg_list = items[i + 1 - 1]
        expr = items[i + 3 - 1]
        p0 = None
        if arg_list is None or expr is None:
            return p0

        entry = SYMBOL_TABLE.access_call(id_, get_lineno(items[i - 1]))
        if entry is None:
            return p0

        if entry.type_ == Type.string:
            variable = gl.SYMBOL_TABLE.access_array(id_, get_lineno(items[i - 1]))
            if len(variable.ref.bounds) and len(variable.ref.bounds) + 1 == len(arg_list):
                ss = arg_list.children.pop().value
                p0 = make_array_substr_assign(get_lineno(items[i - 1]), id_, arg_list, (ss, ss), expr)
                return p0

        arr = make_array_access(id_, get_lineno(items[i - 1]), arg_list)
        if arr is None:
            return p0

        expr = make_typecast(arr.type_, expr, get_lineno(items[i - 1]))
        if entry is None:
            return p0

        if entry.addr is not None:
            mark_entry_as_accessed(entry)
        p0 = make_sentence(get_lineno(items[0]), "LETARRAY", arr, expr)
        return p0

    def substr_assignment_no_let(self, meta, items):
        p0 = None
        entry = SYMBOL_TABLE.access_call(items[0], get_lineno(items[0]))
        if entry is None:
            return p0

        if entry.class_ == CLASS.unknown:
            errmsg.warning_uninitalized_string_var(get_lineno(items[0]), entry.name)
            entry.to_var()

        if items[5].type_ != Type.string:
            errmsg.syntax_error_expected_string(get_lineno(items[4]), items[5].type_)

        lineno = get_lineno(items[1])
        base = make_number(OPTIONS.string_base, lineno, _TYPE(gl.STR_INDEX_TYPE))
        substr = make_typecast(_TYPE(gl.STR_INDEX_TYPE), items[2], lineno)
        p0 = make_sentence(
            get_lineno(items[0]),
            "LETSUBSTR",
            entry,
            make_binary(lineno, "MINUS", substr, base, func=lambda x, y: x - y),
            make_binary(lineno, "MINUS", substr, base, func=lambda x, y: x - y),
            items[5],
        )
        return p0

    def substr_assignment(self, meta, items):
        p0 = None
        if items[2] is None or items[4] is None:
            return p0

        entry = SYMBOL_TABLE.access_call(items[1], get_lineno(items[1]))
        if entry is None:
            return p0

        if entry.class_ == CLASS.unknown:
            entry = entry.to_var()

        if entry.class_ != CLASS.var:
            errmsg.syntax_error_cannot_assign_not_a_var(get_lineno(items[1]), items[1])
            return p0

        if entry.type_ != Type.string:
            errmsg.syntax_error_expected_string(get_lineno(items[1]), entry.type_)
            return p0

        if items[4].type_ != Type.string:
            errmsg.syntax_error_expected_string(get_lineno(items[3]), items[4].type_)
            return p0

        if len(items[2]) > 1:
            error(get_lineno(items[1]), "Accessing string with too many indexes. Expected only one.")
            return p0

        if len(items[2]) == 1:
            substr = (
                make_typecast(_TYPE(gl.STR_INDEX_TYPE), items[2][0].value, get_lineno(items[1])),
                make_typecast(_TYPE(gl.STR_INDEX_TYPE), items[2][0].value, get_lineno(items[1])),
            )
        else:
            substr = (
                make_typecast(
                    _TYPE(gl.STR_INDEX_TYPE),
                    make_number(gl.MIN_STRSLICE_IDX, lineno=get_lineno(items[1])),
                    get_lineno(items[1]),
                ),
                make_typecast(
                    _TYPE(gl.STR_INDEX_TYPE),
                    make_number(gl.MAX_STRSLICE_IDX, lineno=get_lineno(items[1])),
                    get_lineno(items[1]),
                ),
            )

        lineno = get_lineno(items[1])
        base = make_number(OPTIONS.string_base, lineno, _TYPE(gl.STR_INDEX_TYPE))
        p0 = make_sentence(
            get_lineno(items[0]),
            "LETSUBSTR",
            entry,
            make_binary(lineno, "MINUS", substr[0], base, func=lambda x, y: x - y),
            make_binary(lineno, "MINUS", substr[1], base, func=lambda x, y: x - y),
            items[4],
        )
        return p0

    def str_assign(self, meta, items):
        p0 = None
        if items[0].upper() != "LET":
            q = items[0]
            r = items[3]
            s = items[1]
            lineno = get_lineno(items[2])
        else:
            q = items[1]
            r = items[4]
            s = items[2]
            lineno = get_lineno(items[3])
        if q is None or s is None:
            return p0

        if r.type_ != Type.string:
            errmsg.syntax_error_expected_string(lineno, r.type_)

        entry = SYMBOL_TABLE.access_var(q, lineno, default_type=Type.string)
        if entry is None:
            return p0

        p0 = make_sentence(get_lineno(items[0]), "LETSUBSTR", entry, s[0], s[1], r)
        return p0

    def goto(self, meta, items):
        p0 = None
        entry = check_and_make_label(items[1], get_lineno(items[1]))
        if entry is not None:
            p0 = make_sentence(get_lineno(items[0]), items[0].upper(), entry)

        return p0

    def go(self, meta, items):
        p0 = None
        p0 = items[0]
        if p0 == "GO":
            p0 += items[1]
        if p0 == "GOSUB" and FUNCTION_LEVEL:
            error(get_lineno(items[0]), "GOSUB not allowed within SUB or FUNCTION")
        return p0

    def if_sentence(self, meta, items):
        p0 = None
        cond_ = items[0]
        if len(items) + 1 == 6:
            lbl = items[2]
            stat_ = make_block(lbl, items[3])
            endif_ = items[4]
        elif len(items) + 1 == 5:
            stat_ = items[2]
            endif_ = items[3]
        else:
            stat_ = make_nop()
            endif_ = items[2]
        p0 = make_sentence(get_lineno(items[1]), "IF", cond_, make_block(stat_, endif_))
        return p0

    def endif(self, meta, items):
        p0 = None
        p0 = (
            make_nop()
            if getattr(items[0], "type", None) in ("END", "ENDIF", "END_IF") or items[0] in ("END", "ENDIF", "END_IF")
            else items[0]
        )
        return p0

    def statement_if(self, meta, items):
        p0 = None
        p0 = items[0]
        return p0

    def statement_if_then_endif(self, meta, items):
        p0 = None
        cond_ = items[0]
        stat_ = items[1]
        endif_ = items[2]
        p0 = make_sentence(get_lineno(items[0]), "IF", cond_, make_block(stat_, endif_))
        return p0

    def single_line_if(self, meta, items):
        p0 = None
        cond_ = items[0]
        stat_ = items[1]
        p0 = make_sentence(get_lineno(items[0]), "IF", cond_, stat_)
        return p0

    def if_elseif(self, meta, items):
        p0 = None
        cond_ = items[0]
        stats_ = items[2] if len(items) + 1 == 5 else make_nop()
        eliflist = items[3] if len(items) + 1 == 5 else items[2]
        p0 = make_sentence(get_lineno(items[1]), "IF", cond_, stats_, eliflist)
        return p0

    def elseif_part(self, meta, items):
        p0 = None
        if items[0] == "ELSEIF":
            label_ = make_nop()
            cond_ = items[1]
        else:
            label_ = items[0]
            cond_ = items[2]
        p0 = (label_, cond_)
        return p0

    def elseif_list(self, meta, items):
        p0 = None
        label_, cond_ = items[0]
        then_ = items[1]
        else_ = items[2]
        if isinstance(else_, list):
            else_ = make_block(*else_)
        else:
            then_ = make_block(then_, else_)
            else_ = None
        p0 = make_block(label_, make_sentence(get_lineno(items[0]), "IF", cond_, then_, else_))
        return p0

    def elseif_elseiflist(self, meta, items):
        p0 = None
        label_, cond_ = items[0]
        then_ = items[1]
        else_ = items[2]
        p0 = make_block(label_, make_sentence(get_lineno(items[0]), "IF", cond_, then_, else_))
        return p0

    def else_part_endif(self, meta, items):
        p0 = None
        if items[1] == "\n":
            if len(items) + 1 == 4:
                p0 = [make_nop(), items[2]]
            elif len(items) + 1 == 6:
                p0 = [items[2], items[3], items[4]]
            else:
                p0 = [items[2], items[3]]
        else:
            p0 = [items[1], items[2]]
        return p0

    def else_part(self, meta, items):
        p0 = None
        p0 = [items[1], make_nop()]
        return p0

    def else_part_is_inline(self, meta, items):
        p0 = None
        p0 = items[0]
        return p0

    def else_part_label(self, meta, items):
        p0 = None
        lbl = items[0]
        p0 = [make_block(lbl, items[2]), items[3]]
        return p0

    def if_then_part(self, meta, items):
        p0 = None
        expr = items[1]
        if expr is None:
            p0 = None
            return p0
        if is_number(expr):
            errmsg.warning_condition_is_always(get_lineno(items[0]), bool(expr.value))
        p0 = expr
        return p0

    def if_inline(self, meta, items):
        p0 = None
        items[0].append_child(make_block(items[1][0], items[1][1]))
        p0 = items[0]
        return p0

    def if_else(self, meta, items):
        p0 = None
        cond_ = items[0]
        then_ = items[2]
        else_ = items[3][0]
        endif = items[3][1]
        p0 = make_sentence(get_lineno(items[1]), "IF", cond_, then_, make_block(else_, endif))
        return p0

    def then(self, meta, items):
        p0 = None
        return p0

    def for_sentence(self, meta, items):
        p0 = None
        p0 = items[0]
        if is_null(p0):
            return p0
        items[0].append_child(make_block(items[1], items[2]))
        gl.LOOPS.pop()
        return p0

    def next(self, meta, items):
        p0 = None
        p0 = make_nop() if items[0] == "NEXT" else items[0]
        return p0

    def next1(self, meta, items):
        p0 = None
        if items[0] == "NEXT":
            p1 = make_nop()
            p3 = items[1]
        else:
            p1 = items[0]
            p3 = items[2]
        if p3 != gl.LOOPS[-1].var:
            errmsg.syntax_error_wrong_for_var(get_lineno(items[1]), gl.LOOPS[-1].var, p3)
            p0 = make_nop()
            return p0
        p0 = p1
        return p0

    def for_sentence_start(self, meta, items):
        p0 = None
        gl.LOOPS.append(LoopInfo(type=LoopType.FOR, lineno=get_lineno(items[0]), var=items[1]))
        p0 = None
        if items[3] is None or items[5] is None or items[6] is None:
            return p0
        if is_number(items[3], items[5], items[6]):
            if items[3].value != items[5].value and items[6].value == 0:
                warning(get_lineno(items[4]), "STEP value is 0 and FOR might loop forever")
            if items[3].value > items[5].value and items[6].value > 0:
                warning(get_lineno(items[4]), "FOR start value is greater than end. This FOR loop is useless")
            if items[3].value < items[5].value and items[6].value < 0:
                warning(get_lineno(items[1]), "FOR start value is lower than end. This FOR loop is useless")
        id_type = common_type(common_type(items[3].type_, items[5].type_), items[6].type_)
        variable = SYMBOL_TABLE.access_var(items[1], get_lineno(items[1]), default_type=id_type)
        if variable is None:
            return p0
        mark_entry_as_accessed(variable)
        expr1 = make_typecast(variable.type_, items[3], get_lineno(items[2]))
        expr2 = make_typecast(variable.type_, items[5], get_lineno(items[4]))
        expr3 = make_typecast(variable.type_, items[6], meta_line(meta))
        p0 = make_sentence(get_lineno(items[0]), "FOR", variable, expr1, expr2, expr3)
        return p0

    def step(self, meta, items):
        p0 = None
        p0 = make_number(1, lineno=meta_line(meta))
        return p0

    def step_expr(self, meta, items):
        p0 = None
        p0 = items[1]
        return p0

    def end(self, meta, items):
        p0 = None
        q = make_number(0, lineno=get_lineno(items[0])) if len(items) + 1 == 2 else items[1]
        p0 = make_sentence(get_lineno(items[0]), "END", q)
        return p0

    def error_raise(self, meta, items):
        p0 = None
        q = make_number(1, lineno=get_lineno(items[1]))
        r = make_binary(
            get_lineno(items[0]),
            "MINUS",
            make_typecast(Type.ubyte, items[1], get_lineno(items[0])),
            q,
            lambda x, y: x - y,
        )
        p0 = make_sentence(get_lineno(items[0]), "ERROR", r)
        return p0

    def stop_raise(self, meta, items):
        p0 = None
        q = make_number(9, lineno=get_lineno(items[0])) if len(items) + 1 == 2 else items[1]
        z = make_number(1, lineno=get_lineno(items[0]))
        r = make_binary(
            get_lineno(items[0]), "MINUS", make_typecast(Type.ubyte, q, get_lineno(items[0])), z, lambda x, y: x - y
        )
        p0 = make_sentence(get_lineno(items[0]), "STOP", r)
        return p0

    def loop(self, meta, items):
        p0 = None
        if items[0] == "LOOP":
            p0 = None
        else:
            p0 = items[0]
        return p0

    def do_loop(self, meta, items):
        p0 = None
        if len(items) + 1 == 4:
            q = make_block(items[1], items[2])
        else:
            q = items[1]
        if items[0] == "DO":
            gl.LOOPS.append(LoopInfo(LoopType.DO, get_lineno(items[0])))
        if q is None:
            warning(get_lineno(items[0]), "Infinite empty loop")
        p0 = make_sentence(get_lineno(items[0]), "DO_LOOP", q)
        gl.LOOPS.pop()
        return p0

    def do_loop_until(self, meta, items):
        p0 = None
        p0 = None
        if len(items) + 1 == 6:
            q = make_block(items[1], items[2])
            r = items[4]
        else:
            q = items[1]
            r = items[3]
        if items[0] == "DO":
            gl.LOOPS.append(LoopInfo(LoopType.DO, get_lineno(items[0])))
        p0 = make_sentence(get_lineno(items[0]), "DO_UNTIL", r, q)
        gl.LOOPS.pop()
        if is_number(r):
            errmsg.warning_condition_is_always(get_lineno(items[-2]), bool(r.value))
        if q is None:
            errmsg.warning_empty_loop(get_lineno(items[-2]))
        return p0

    def data(self, meta, items):
        p0 = None
        label_ = make_label(gl.DATA_PTR_CURRENT, lineno=get_lineno(items[0]))
        datas_ = []
        funcs = []
        if items[1] is None:
            p0 = None
            return p0
        if gl.FUNCTION_LEVEL:
            errmsg.error(get_lineno(items[0]), "DATA not allowed within Functions nor Subs")
            p0 = None
            return p0
        for d in items[1].children:
            value = d.value
            if is_static(value):
                datas_.append(d)
                continue
            new_lbl = f"__DATA__FUNCPTR__{len(gl.DATA_FUNCTIONS)}"
            type_ = value.type_
            assert isinstance(type_, sym.TYPING)
            if isinstance(type_, sym.TYPE):
                type_ = sym.TYPEREF(value.type_, 0)
            entry = make_func_declaration(new_lbl, get_lineno(items[0]), type_=type_, class_=CLASS.function)
            if not entry:
                continue
            func = entry.entry
            func.ref.convention = CONVENTION.fastcall
            SYMBOL_TABLE.enter_scope(new_lbl)
            func.ref.local_symbol_table = SYMBOL_TABLE.current_scope
            func.ref.locals_size = SYMBOL_TABLE.leave_scope()
            gl.DATA_FUNCTIONS.append(func)
            sent = make_sentence(get_lineno(items[0]), "RETURN", func, value)
            func.ref.body = make_block(sent)
            datas_.append(entry)
            funcs.append(entry)
        gl.DATAS.append(src.api.dataref.DataRef(label_, datas_))
        id_ = src.api.utils.current_data_label()
        gl.DATA_PTR_CURRENT = id_
        return p0

    def restore(self, meta, items):
        p0 = None
        if len(items) + 1 == 2:
            lbl = None
        else:
            lbl = check_and_make_label(items[1], get_lineno(items[0]))
        p0 = make_sentence(get_lineno(items[0]), "RESTORE", lbl)
        return p0

    def read(self, meta, items):
        p0 = None
        gl.DATA_IS_USED = True
        reads = []
        if items[1] is None:
            return p0
        for arg in items[1]:
            entry = arg.value
            if entry is None:
                p0 = None
                return p0
            if entry.token == "VARARRAY":
                errmsg.error(get_lineno(items[0]), "Cannot read '%s'. It's an array" % entry.name)
                p0 = None
                return p0
            if isinstance(entry, sym.ID):
                if entry.class_ != CLASS.var:
                    errmsg.syntax_error_cannot_assign_not_a_var(get_lineno(items[1]), entry.name)
                    p0 = None
                    return p0
                mark_entry_as_accessed(entry)
                if entry.type_ == Type.auto:
                    entry.type_ = _TYPE(gl.DEFAULT_TYPE)
                    errmsg.warning_implicit_type(get_lineno(items[1]), items[1], entry.type_.name)
                reads.append(make_sentence(get_lineno(items[0]), "READ", entry))
                continue
            if isinstance(entry, sym.ARRAYLOAD):
                reads.append(
                    make_sentence(
                        get_lineno(items[0]),
                        "READ",
                        sym.ARRAYACCESS(entry.entry, entry.args, entry.lineno, gl.FILENAME),
                    )
                )
                continue
            errmsg.error(get_lineno(items[0]), "Syntax error. Can only read a variable or an array element")
            p0 = None
            return p0
        p0 = make_block(*reads)
        return p0

    def do_loop_while(self, meta, items):
        p0 = None
        p0 = None
        if len(items) + 1 == 6:
            q = make_block(items[1], items[2])
            r = items[4]
        else:
            q = items[1]
            r = items[3]
        if items[0] == "DO":
            gl.LOOPS.append(LoopInfo(LoopType.DO, get_lineno(items[0])))
        p0 = make_sentence(get_lineno(items[0]), "DO_WHILE", r, q)
        gl.LOOPS.pop()
        if is_number(r):
            errmsg.warning_condition_is_always(get_lineno(items[-2]), bool(r.value))
        if q is None:
            errmsg.warning_empty_loop(get_lineno(items[-2]))
        return p0

    def do_while_loop(self, meta, items):
        p0 = None
        r = items[0]
        q = items[1]
        if q == "LOOP":
            q = None
        p0 = make_sentence(get_lineno(items[0]), "WHILE_DO", r, q)
        gl.LOOPS.pop()
        if is_number(r):
            errmsg.warning_condition_is_always(get_lineno(items[1]), bool(r.value))
        return p0

    def do_until_loop(self, meta, items):
        p0 = None
        r = items[0]
        q = items[1]
        if q == "LOOP":
            q = None
        p0 = make_sentence(get_lineno(items[1]), "UNTIL_DO", r, q)
        gl.LOOPS.pop()
        if is_number(r):
            errmsg.warning_condition_is_always(get_lineno(items[1]), bool(r.value))
        return p0

    def do_while_start(self, meta, items):
        p0 = None
        p0 = items[2]
        gl.LOOPS.append(LoopInfo(LoopType.DO, get_lineno(items[0])))
        return p0

    def do_until_start(self, meta, items):
        p0 = None
        p0 = items[2]
        gl.LOOPS.append(LoopInfo(LoopType.DO, get_lineno(items[0])))
        return p0

    def do_start(self, meta, items):
        p0 = None
        gl.LOOPS.append(LoopInfo(LoopType.DO, get_lineno(items[0])))
        return p0

    def label_end_while(self, meta, items):
        p0 = None
        if getattr(items[0], "type", None) in ("WEND", "END_WHILE") or items[0] in ("WEND", "END_WHILE"):
            p0 = None
        else:
            p0 = items[0]
        return p0

    def while_sentence(self, meta, items):
        p0 = None
        gl.LOOPS.pop()
        q = make_block(items[1], items[2])
        if is_number(items[0]):
            errmsg.warning_condition_is_always(get_lineno(items[0]), bool(items[0].value))
        p0 = make_sentence(get_lineno(items[0]), "WHILE", items[0], q)
        return p0

    def while_start(self, meta, items):
        p0 = None
        p0 = items[1]
        gl.LOOPS.append(LoopInfo(LoopType.WHILE, get_lineno(items[0])))
        if is_number(items[1]) and (not items[1].value):
            errmsg.warning_condition_is_always(get_lineno(items[0]))
        return p0

    def exit(self, meta, items):
        p0 = None
        q = items[1]
        p0 = make_sentence(get_lineno(items[0]), "EXIT_%s" % q)
        for loop in gl.LOOPS:
            if q == loop.type:
                return p0
        error(get_lineno(items[0]), "Syntax Error: EXIT %s out of loop" % q)
        return p0

    def continue_(self, meta, items):
        p0 = None
        q = items[1]
        p0 = make_sentence(get_lineno(items[0]), "CONTINUE_%s" % q)
        for i in gl.LOOPS:
            if q == i[0]:
                return p0
        error(get_lineno(items[0]), "Syntax Error: CONTINUE %s out of loop" % q)
        return p0

    def print_sentence(self, meta, items):
        p0 = None
        global PRINT_IS_USED
        p0 = items[1]
        PRINT_IS_USED = True
        return p0

    def print_elem_expr(self, meta, items):
        p0 = None
        p0 = items[0]
        if items[0] is not None and items[0].type_ == Type.boolean:
            p0 = make_typecast(Type.ubyte, items[0], get_lineno(items[0]))
        return p0

    def print_list_expr(self, meta, items):
        p0 = None
        if items[0] in ("BOLD", "ITALIC"):
            p0 = make_sentence(
                get_lineno(items[0]), items[0] + "_TMP", make_typecast(Type.ubyte, items[1], get_lineno(items[0]))
            )
        else:
            p0 = items[0]
        return p0

    def attr_list(self, meta, items):
        p0 = None
        p0 = items[0]
        return p0

    def attr_list_list(self, meta, items):
        p0 = None
        p0 = make_block(items[0], items[1])
        return p0

    def attr(self, meta, items):
        p0 = None
        p0 = make_sentence(
            get_lineno(items[0]), items[0] + "_TMP", make_typecast(Type.ubyte, items[1], get_lineno(items[0]))
        )
        return p0

    def print_list_epsilon(self, meta, items):
        p0 = None
        p0 = None
        return p0

    def print_list_elem(self, meta, items):
        p0 = None
        p0 = make_sentence(meta_line(meta), "PRINT", items[0])
        p0.eol = True
        return p0

    def print_list(self, meta, items):
        p0 = None
        p0 = items[0]
        p0.eol = items[2] is not None
        if items[2] is not None:
            p0.append_child(items[2])
        return p0

    def print_list_comma(self, meta, items):
        p0 = None
        p0 = items[0]
        p0.eol = items[2] is not None
        p0.append_child(make_sentence(get_lineno(items[1]), "PRINT_COMMA"))
        if items[2] is not None:
            p0.append_child(items[2])
        return p0

    def print_list_at(self, meta, items):
        p0 = None
        p0 = make_sentence(
            get_lineno(items[0]),
            "PRINT_AT",
            make_typecast(Type.ubyte, items[1], get_lineno(items[0])),
            make_typecast(Type.ubyte, items[3], get_lineno(items[2])),
        )
        return p0

    def print_list_tab(self, meta, items):
        p0 = None
        p0 = make_sentence(get_lineno(items[0]), "PRINT_TAB", make_typecast(Type.ubyte, items[1], get_lineno(items[0])))
        return p0

    def on_goto(self, meta, items):
        p0 = None
        expr = make_typecast(Type.ubyte, items[1], get_lineno(items[0]))
        p0 = make_sentence(get_lineno(items[0]), "ON_" + items[2], expr, *items[3])
        return p0

    def label_list(self, meta, items):
        p0 = None
        entry = check_and_make_label(items[0], get_lineno(items[0]))
        p0 = [entry]
        return p0

    def label_list_list(self, meta, items):
        p0 = None
        p0 = items[0]
        entry = check_and_make_label(items[2], get_lineno(items[2]))
        items[0].append(entry)
        return p0

    def return_(self, meta, items):
        p0 = None
        if not FUNCTION_LEVEL:
            p0 = make_sentence(get_lineno(items[0]), "RETURN")
            return p0
        if FUNCTION_LEVEL[-1].class_ != CLASS.sub:
            error(get_lineno(items[0]), "Syntax Error: Function must RETURN a value.")
            p0 = None
            return p0
        p0 = make_sentence(get_lineno(items[0]), "RETURN", FUNCTION_LEVEL[-1])
        return p0

    def return_expr(self, meta, items):
        p0 = None
        if not FUNCTION_LEVEL:
            error(get_lineno(items[0]), "Syntax Error: Returning value out of FUNCTION")
            p0 = None
            return p0
        if FUNCTION_LEVEL[-1].class_ is CLASS.unknown:
            p0 = None
            return p0
        if FUNCTION_LEVEL[-1].class_ != CLASS.function:
            error(get_lineno(items[0]), "Syntax Error: SUBs cannot return a value")
            p0 = None
            return p0
        if FUNCTION_LEVEL[-1].type_ is None:
            p0 = None
            return p0
        if is_numeric(items[1]) and FUNCTION_LEVEL[-1].type_.final == Type.string:
            error(get_lineno(items[1]), "Type Error: Function must return a string, not a numeric value")
            p0 = None
            return p0
        if not is_numeric(items[1]) and FUNCTION_LEVEL[-1].type_.final != Type.string:
            error(get_lineno(items[1]), "Type Error: Function must return a numeric value, not a string")
            p0 = None
            return p0
        p0 = make_sentence(
            get_lineno(items[0]),
            "RETURN",
            FUNCTION_LEVEL[-1],
            make_typecast(FUNCTION_LEVEL[-1].type_, items[1], get_lineno(items[0])),
        )
        return p0

    def pause(self, meta, items):
        p0 = None
        p0 = make_sentence(get_lineno(items[0]), "PAUSE", make_typecast(Type.uinteger, items[1], get_lineno(items[0])))
        return p0

    def poke(self, meta, items):
        p0 = None
        i = 2 if isinstance(items[1], Symbol) or items[1] is None else 3
        if items[i - 1] is None or items[i + 2 - 1] is None:
            p0 = None
            return p0
        p0 = make_sentence(
            get_lineno(items[0]),
            "POKE",
            make_typecast(Type.uinteger, items[i - 1], get_lineno(items[i + 1 - 1])),
            make_typecast(Type.ubyte, items[i + 2 - 1], get_lineno(items[i + 1 - 1])),
        )
        return p0

    def poke2(self, meta, items):
        p0 = None
        i = 2 if isinstance(items[1], Symbol) or items[1] is None else 3
        if items[i + 1 - 1] is None or items[i + 3 - 1] is None:
            p0 = None
            return p0
        p0 = make_sentence(
            get_lineno(items[0]),
            "POKE",
            make_typecast(Type.uinteger, items[i + 1 - 1], get_lineno(items[i + 2 - 1])),
            make_typecast(items[i - 1], items[i + 3 - 1], get_lineno(items[i + 3 - 1])),
        )
        return p0

    def poke3(self, meta, items):
        p0 = None
        i = 2 if isinstance(items[1], Symbol) or items[1] is None else 3
        if items[i + 2 - 1] is None or items[i + 4 - 1] is None:
            p0 = None
            return p0
        p0 = make_sentence(
            get_lineno(items[0]),
            "POKE",
            make_typecast(Type.uinteger, items[i + 2 - 1], get_lineno(items[i + 3 - 1])),
            make_typecast(items[i - 1], items[i + 4 - 1], get_lineno(items[i + 5 - 1])),
        )
        return p0

    def out(self, meta, items):
        p0 = None
        p0 = make_sentence(
            get_lineno(items[0]),
            "OUT",
            make_typecast(Type.uinteger, items[1], get_lineno(items[2])),
            make_typecast(Type.ubyte, items[3], get_lineno(items[3])),
        )
        return p0

    def simple_instruction(self, meta, items):
        p0 = None
        p0 = make_sentence(get_lineno(items[0]), items[0], make_typecast(Type.ubyte, items[1], get_lineno(items[0])))
        return p0

    def save_code(self, meta, items):
        p0 = None
        expr = items[1]
        if expr.type_ != Type.string:
            errmsg.syntax_error_expected_string(get_lineno(items[0]), expr.type_)
        if len(items) + 1 == 4:
            if items[2].upper() not in ("SCREEN", "SCREEN$"):
                error(get_lineno(items[2]), 'Unexpected "%s" ID. Expected "SCREEN$" instead' % items[2])
                return None
            start = make_number(16384, lineno=get_lineno(items[0]))
            length = make_number(6912, lineno=get_lineno(items[0]))
        else:
            start = make_typecast(Type.uinteger, items[3], get_lineno(items[3]))
            length = make_typecast(Type.uinteger, items[5], get_lineno(items[5]))
        p0 = make_sentence(get_lineno(items[0]), items[0], expr, start, length)
        return p0

    def save_data(self, meta, items):
        p0 = None
        if items[1].type_ != Type.string:
            errmsg.syntax_error_expected_string(get_lineno(items[0]), items[1].type_)
        if len(items) + 1 != 4:
            entry = SYMBOL_TABLE.access_id(items[3], get_lineno(items[3]))
            if entry is None:
                p0 = None
                return p0
            mark_entry_as_accessed(entry)
            access = entry
            start = make_unary(get_lineno(items[3]), "ADDRESS", access, type_=Type.uinteger)
            if entry.class_ == CLASS.array:
                length = make_number(entry.memsize, lineno=get_lineno(items[3]))
            else:
                length = make_number(entry.type_.size, lineno=get_lineno(items[3]))
        else:
            access = SYMBOL_TABLE.access_label(gl.ZXBASIC_USER_DATA, get_lineno(items[2]), SYMBOL_TABLE.global_scope)
            start = make_unary(get_lineno(items[2]), "ADDRESS", access, type_=Type.uinteger)
            access = SYMBOL_TABLE.access_label(
                gl.ZXBASIC_USER_DATA_LEN, get_lineno(items[2]), SYMBOL_TABLE.global_scope
            )
            length = make_unary(get_lineno(items[2]), "ADDRESS", access, type_=Type.uinteger)
        p0 = make_sentence(get_lineno(items[0]), items[0], items[1], start, length)
        return p0

    def load_or_verify(self, meta, items):
        p0 = None
        p0 = items[0]
        return p0

    def load_code(self, meta, items):
        p0 = None
        if items[1].type_ != Type.string:
            errmsg.syntax_error_expected_string(get_lineno(items[2]), items[1].type_)
        if len(items) + 1 == 4:
            if items[2].upper() not in ("SCREEN", "SCREEN$", "CODE"):
                error(get_lineno(items[2]), 'Unexpected "%s" ID. Expected "SCREEN$" instead' % items[2])
                return None
            if items[2].upper() == "CODE":
                start = make_number(0, lineno=get_lineno(items[2]))
                length = make_number(0, lineno=get_lineno(items[2]))
            else:
                start = make_number(16384, lineno=get_lineno(items[2]))
                length = make_number(6912, lineno=get_lineno(items[2]))
        else:
            start = make_typecast(Type.uinteger, items[3], get_lineno(items[2]))
            if len(items) + 1 == 5:
                length = make_number(0, lineno=get_lineno(items[2]))
            else:
                length = make_typecast(Type.uinteger, items[5], get_lineno(items[4]))
        p0 = make_sentence(get_lineno(items[2]), items[0], items[1], start, length)
        return p0

    def load_data(self, meta, items):
        p0 = None
        if items[1].type_ != Type.string:
            errmsg.syntax_error_expected_string(get_lineno(items[0]), items[1].type_)
        if len(items) + 1 != 4:
            entry = SYMBOL_TABLE.access_id(items[3], get_lineno(items[3]))
            if entry is None:
                p0 = None
                return p0
            mark_entry_as_accessed(entry)
            start = make_unary(get_lineno(items[3]), "ADDRESS", entry, type_=Type.uinteger)
            if entry.class_ == CLASS.array:
                length = make_number(entry.memsize, lineno=get_lineno(items[3]))
            else:
                length = make_number(entry.type_.size, lineno=get_lineno(items[3]))
        else:
            entry = SYMBOL_TABLE.access_label(gl.ZXBASIC_USER_DATA, get_lineno(items[2]), SYMBOL_TABLE.global_scope)
            start = make_unary(get_lineno(items[2]), "ADDRESS", entry, type_=Type.uinteger)
            entry = SYMBOL_TABLE.access_label(gl.ZXBASIC_USER_DATA_LEN, get_lineno(items[2]), SYMBOL_TABLE.global_scope)
            length = make_unary(get_lineno(items[2]), "ADDRESS", entry, type_=Type.uinteger)
        p0 = make_sentence(get_lineno(items[2]), items[0], items[1], start, length)
        return p0

    def numbertype(self, meta, items):
        p0 = None
        p0 = make_type(items[0].lower(), get_lineno(items[0]))
        return p0

    def expr_plus_expr(self, meta, items):
        p0 = None
        p0 = make_binary(get_lineno(items[1]), "PLUS", items[0], items[2], lambda x, y: x + y)
        return p0

    def expr_minus_expr(self, meta, items):
        p0 = None
        p0 = make_binary(get_lineno(items[1]), "MINUS", items[0], items[2], lambda x, y: x - y)
        return p0

    def expr_mul_expr(self, meta, items):
        p0 = None
        p0 = make_binary(get_lineno(items[1]), "MUL", items[0], items[2], lambda x, y: x * y)
        return p0

    def expr_div_expr(self, meta, items):
        p0 = None
        p0 = make_binary(get_lineno(items[1]), "DIV", items[0], items[2], lambda x, y: x / y)
        return p0

    def expr_mod_expr(self, meta, items):
        p0 = None
        p0 = make_binary(get_lineno(items[1]), "MOD", items[0], items[2], lambda x, y: x % y)
        return p0

    def expr_pow_expr(self, meta, items):
        p0 = None
        p0 = make_binary(
            get_lineno(items[1]),
            "POW",
            make_typecast(Type.float_, items[0], get_lineno(items[1])),
            make_typecast(Type.float_, items[2], meta_line(meta)),
            lambda x, y: x**y,
        )
        return p0

    def expr_shl_expr(self, meta, items):
        p0 = None
        if items[0] is None or items[2] is None:
            p0 = None
            return p0
        if items[0].type_ in (Type.float_, Type.fixed):
            items[0] = make_typecast(Type.ulong, items[0], get_lineno(items[1]))
        p0 = make_binary(
            get_lineno(items[1]),
            "SHL",
            items[0],
            make_typecast(Type.ubyte, items[2], get_lineno(items[1])),
            lambda x, y: x << y,
        )
        return p0

    def expr_shr_expr(self, meta, items):
        p0 = None
        if items[0] is None or items[2] is None:
            p0 = None
            return p0
        if items[0].type_ in (Type.float_, Type.fixed):
            items[0] = make_typecast(Type.ulong, items[0], get_lineno(items[1]))
        p0 = make_binary(
            get_lineno(items[1]),
            "SHR",
            items[0],
            make_typecast(Type.ubyte, items[2], get_lineno(items[1])),
            lambda x, y: x >> y,
        )
        return p0

    def minus_expr(self, meta, items):
        p0 = None
        p0 = make_unary(get_lineno(items[0]), "MINUS", items[1], lambda x: -x)
        return p0

    def expr_eq_expr(self, meta, items):
        p0 = None
        p0 = make_binary(get_lineno(items[1]), "EQ", items[0], items[2], lambda x, y: x == y)
        return p0

    def expr_lt_expr(self, meta, items):
        p0 = None
        p0 = make_binary(get_lineno(items[1]), "LT", items[0], items[2], lambda x, y: x < y)
        return p0

    def expr_le_expr(self, meta, items):
        p0 = None
        p0 = make_binary(get_lineno(items[1]), "LE", items[0], items[2], lambda x, y: x <= y)
        return p0

    def expr_gt_expr(self, meta, items):
        p0 = None
        p0 = make_binary(get_lineno(items[1]), "GT", items[0], items[2], lambda x, y: x > y)
        return p0

    def expr_ge_expr(self, meta, items):
        p0 = None
        p0 = make_binary(get_lineno(items[1]), "GE", items[0], items[2], lambda x, y: x >= y)
        return p0

    def expr_ne_expr(self, meta, items):
        p0 = None
        p0 = make_binary(get_lineno(items[1]), "NE", items[0], items[2], lambda x, y: x != y)
        return p0

    def expr_or_expr(self, meta, items):
        p0 = None
        p0 = make_binary(get_lineno(items[1]), "OR", items[0], items[2], lambda x, y: x or y)
        return p0

    def expr_bor_expr(self, meta, items):
        p0 = None
        p0 = make_binary(get_lineno(items[1]), "BOR", items[0], items[2], lambda x, y: x | y)
        return p0

    def expr_xor_expr(self, meta, items):
        p0 = None
        p0 = make_binary(get_lineno(items[1]), "XOR", items[0], items[2], lambda x, y: x and (not y) or (not x and y))
        return p0

    def expr_bxor_expr(self, meta, items):
        p0 = None
        p0 = make_binary(get_lineno(items[1]), "BXOR", items[0], items[2], lambda x, y: x ^ y)
        return p0

    def expr_and_expr(self, meta, items):
        p0 = None
        p0 = make_binary(get_lineno(items[1]), "AND", items[0], items[2], lambda x, y: x and y)
        return p0

    def expr_band_expr(self, meta, items):
        p0 = None
        p0 = make_binary(get_lineno(items[1]), "BAND", items[0], items[2], lambda x, y: x & y)
        return p0

    def not_expr(self, meta, items):
        p0 = None
        p0 = make_unary(get_lineno(items[0]), "NOT", items[1], lambda x: not x)
        return p0

    def bnot_expr(self, meta, items):
        p0 = None
        p0 = make_unary(get_lineno(items[0]), "BNOT", items[1], lambda x: ~x)
        return p0

    def lp_expr_rp(self, meta, items):
        p0 = None
        p0 = items[1]
        return p0

    def cast(self, meta, items):
        p0 = None
        p0 = make_typecast(items[2], items[4], get_lineno(items[5]))
        return p0

    def number_expr(self, meta, items):
        p0 = None
        p0 = make_number(items[0], lineno=get_lineno(items[0]))
        return p0

    def expr_pi(self, meta, items):
        p0 = None
        p0 = make_number(PI, lineno=get_lineno(items[0]), type_=Type.float_)
        return p0

    def expr_string(self, meta, items):
        p0 = None
        p0 = items[0]
        return p0

    def string_func_call(self, meta, items):
        p0 = None
        p0 = make_strslice(get_lineno(items[0]), items[0], items[1][0], items[1][1])
        return p0

    def string_func_call_single(self, meta, items):
        p0 = None
        p0 = make_strslice(get_lineno(items[0]), items[0], items[2], items[2])
        return p0

    def string_str(self, meta, items):
        p0 = None
        p0 = sym.STRING(items[0], get_lineno(items[0]))
        return p0

    def string_lprp(self, meta, items):
        p0 = None
        p0 = items[0]
        return p0

    def string_lp_expr_rp(self, meta, items):
        p0 = None
        p0 = make_strslice(get_lineno(items[1]), items[0], items[2], items[2])
        return p0

    def expr_id_substr(self, meta, items):
        p0 = None
        entry = SYMBOL_TABLE.get_entry(items[0])
        if entry is not None and entry.type_ == Type.string and (entry.token == "CONST"):
            p0 = make_strslice(get_lineno(items[0]), entry, items[1][0], items[1][1])
            return p0
        entry = SYMBOL_TABLE.access_var(items[0], get_lineno(items[0]), default_type=Type.string)
        p0 = None
        if entry is None:
            return p0
        mark_entry_as_accessed(entry)
        p0 = make_strslice(get_lineno(items[0]), entry, items[1][0], items[1][1])
        return p0

    def string_substr(self, meta, items):
        p0 = None
        p0 = make_strslice(get_lineno(items[0]), items[0], items[1][0], items[1][1])
        return p0

    def string_expr_lp(self, meta, items):
        p0 = None
        if items[1].type_ != Type.string:
            error(
                meta_line(meta),
                "Expected a string type expression. Got %s type instead" % Type.to_string(items[1].type_),
            )
            p0 = None
        else:
            p0 = make_strslice(meta_line(meta), items[1], items[3][0], items[3][1])
        return p0

    def subind_str(self, meta, items):
        p0 = None
        p0 = (
            make_typecast(Type.uinteger, items[1], get_lineno(items[0])),
            make_typecast(Type.uinteger, items[3], get_lineno(items[2])),
        )
        return p0

    def subind_strto(self, meta, items):
        p0 = None
        p0 = (
            make_typecast(Type.uinteger, make_number(0, lineno=get_lineno(items[1])), get_lineno(items[0])),
            make_typecast(Type.uinteger, items[2], get_lineno(items[1])),
        )
        return p0

    def subind_tostr(self, meta, items):
        p0 = None
        p0 = (
            make_typecast(Type.uinteger, items[1], get_lineno(items[0])),
            make_typecast(
                Type.uinteger,
                make_number(gl.MAX_STRSLICE_IDX, lineno=get_lineno(items[3])),
                lineno=get_lineno(items[3]),
            ),
            get_lineno(items[2]),
        )
        return p0

    def subind_to(self, meta, items):
        p0 = None
        p0 = (
            make_typecast(Type.uinteger, make_number(0, lineno=get_lineno(items[1])), get_lineno(items[0])),
            make_typecast(
                Type.uinteger, make_number(gl.MAX_STRSLICE_IDX, lineno=get_lineno(items[2])), get_lineno(items[1])
            ),
        )
        return p0

    def id_expr(self, meta, items):
        p0 = None
        entry = SYMBOL_TABLE.access_id(items[0], get_lineno(items[0]), default_class=CLASS.var)
        if entry is None:
            p0 = None
            return p0
        mark_entry_as_accessed(entry)
        if entry.type_ == Type.auto:
            entry.type_ = _TYPEREF(gl.DEFAULT_TYPE)
            errmsg.warning_implicit_type(get_lineno(items[0]), items[0], entry.type_.name)
        p0 = entry
        if p0 is not None:
            if not hasattr(gl, "expr_linenos"):
                gl.expr_linenos = {}
            gl.expr_linenos[id(p0)] = get_lineno(items[0])
        if entry.class_ == CLASS.array:
            if not LET_ASSIGNMENT:
                error(get_lineno(items[0]), "Variable '%s' is an array and cannot be used in this context" % items[0])
                p0 = None
        elif entry.class_ == CLASS.function:
            p0 = make_call(items[0], get_lineno(items[0]), make_arg_list(None))
        elif entry.class_ == CLASS.sub:
            errmsg.syntax_error_is_a_sub_not_a_func(get_lineno(items[0]), items[0])
            p0 = None
        return p0

    def addr_of_id(self, meta, items):
        p0 = None
        id_: Id = items[1]
        entry = SYMBOL_TABLE.access_id(id_.name, id_.lineno, ignore_explicit_flag=True)
        if entry is None:
            p0 = None
            return p0
        entry.has_address = True
        mark_entry_as_accessed(entry)
        result = make_unary(get_lineno(items[0]), "ADDRESS", entry, type_=_TYPE(gl.PTR_TYPE))
        if is_dynamic(entry):
            p0 = result
        else:
            p0 = make_constexpr(get_lineno(items[0]), result)
        return p0

    def expr_bexpr(self, meta, items):
        p0 = None
        p0 = items[0]
        return p0

    def expr_funccall(self, meta, items):
        p0 = None
        p0 = items[0]
        return p0

    def idcall_expr(self, meta, items):
        p0 = None
        if items[1] is None:
            p0 = None
            return p0
        p0 = make_call(items[0], get_lineno(items[0]), items[1])
        if p0 is None:
            return p0
        if p0.token in ("STRSLICE", "ID", "STRING") or (p0.token == "CONST" and p0.type_ == Type.string):
            entry = SYMBOL_TABLE.access_call(items[0], get_lineno(items[0]))
            mark_entry_as_accessed(entry)
            return p0
        convert_to_function(p0.entry, CLASS.function, get_lineno(items[0]))
        mark_entry_as_accessed(p0.entry)
        return p0

    def array_eq_error(self, meta, items):
        p0 = None
        error(get_lineno(items[3]), f"Invalid assignment. Variable {items[1]}() is an array")
        p0 = None
        return p0

    def arr_access_expr(self, meta, items):
        p0 = None
        p0 = make_call(items[0], get_lineno(items[0]), items[1])
        if p0 is None:
            return p0
        entry = SYMBOL_TABLE.access_call(items[0], get_lineno(items[0]))
        mark_entry_as_accessed(entry)
        return p0

    def let_arr_substr(self, meta, items):
        p0 = None
        i = 2 if items[0].upper() == "LET" else 1
        id_ = items[i - 1]
        arg_list = items[i + 1 - 1]
        substr = items[i + 2 - 1]
        expr_ = items[i + 4 - 1]
        p0 = make_array_substr_assign(get_lineno(items[i - 1]), id_, arg_list, substr, expr_)
        return p0

    def let_arr_substr_single(self, meta, items):
        p0 = None
        i = 2 if items[0].upper() == "LET" else 1
        id_ = items[i - 1]
        arg_list = items[i + 1 - 1]
        substr = (items[i + 3 - 1], items[i + 3 - 1])
        expr_ = items[i + 6 - 1]
        p0 = make_array_substr_assign(get_lineno(items[i - 1]), id_, arg_list, substr, expr_)
        return p0

    def let_arr_substr_in_args(self, meta, items):
        p0 = None
        i = 2 if items[0].upper() == "LET" else 1
        id_ = items[i - 1]
        arg_list = items[i + 2 - 1]
        substr = (arg_list.children.pop().value, make_number(gl.MAX_STRSLICE_IDX, lineno=get_lineno(items[i + 3 - 1])))
        expr_ = items[i + 6 - 1]
        p0 = make_array_substr_assign(get_lineno(items[i - 1]), id_, arg_list, substr, expr_)
        return p0

    def let_arr_substr_in_args2(self, meta, items):
        p0 = None
        i = 2 if items[0].upper() == "LET" else 1
        id_ = items[i - 1]
        arg_list = items[i + 2 - 1]
        top_ = items[i + 5 - 1]
        substr = (make_number(0, lineno=get_lineno(items[i + 4 - 1])), top_)
        expr_ = items[i + 8 - 1]
        p0 = make_array_substr_assign(get_lineno(items[i - 1]), id_, arg_list, substr, expr_)
        return p0

    def let_arr_substr_in_args3(self, meta, items):
        p0 = None
        i = 2 if items[0].upper() == "LET" else 1
        id_ = items[i - 1]
        arg_list = items[i + 2 - 1]
        substr = (
            make_number(0, lineno=get_lineno(items[i + 4 - 1])),
            make_number(gl.MAX_STRSLICE_IDX, lineno=get_lineno(items[i + 3 - 1])),
        )
        expr_ = items[i + 7 - 1]
        p0 = make_array_substr_assign(get_lineno(items[i - 1]), id_, arg_list, substr, expr_)
        return p0

    def let_arr_substr_in_args4(self, meta, items):
        p0 = None
        i = 2 if items[0].upper() == "LET" else 1
        id_ = items[i - 1]
        arg_list = items[i + 2 - 1]
        substr = (arg_list.children.pop().value, items[i + 4 - 1])
        expr_ = items[i + 7 - 1]
        p0 = make_array_substr_assign(get_lineno(items[i - 1]), id_, arg_list, substr, expr_)
        return p0

    def addr_of_array_element(self, meta, items):
        p0 = None
        p0 = None
        if items[2] is None:
            return p0
        result = make_array_access(items[1], get_lineno(items[1]), items[2])
        if result is None:
            return p0
        mark_entry_as_accessed(result.entry)
        p0 = make_unary(get_lineno(items[0]), "ADDRESS", result, type_=_TYPE(gl.PTR_TYPE))
        return p0

    def err_undefined_arr_access(self, meta, items):
        p0 = None
        error(get_lineno(items[1]), 'Undeclared array "%s"' % items[1])
        p0 = None
        return p0

    def bexpr_func(self, meta, items):
        p0 = None
        args = make_arg_list(make_argument(items[1], get_lineno(items[1])))
        p0 = make_call(items[0], get_lineno(items[0]), args)
        if p0 is None:
            return p0
        if p0.token in ("STRSLICE", "VAR", "STRING"):
            entry = SYMBOL_TABLE.access_call(items[0], get_lineno(items[0]))
            mark_entry_as_accessed(entry)
            return p0
        convert_to_function(p0.entry, CLASS.function, get_lineno(items[0]))
        mark_entry_as_accessed(p0.entry)
        return p0

    def arg_list(self, meta, items):
        p0 = None
        p0 = make_arg_list(None)
        return p0

    def arg_list_arg(self, meta, items):
        p0 = None
        p0 = items[1]
        return p0

    def arguments(self, meta, items):
        p0 = None
        if items[0] is None:
            p0 = None
            return p0
        p0 = make_arg_list(items[0])
        return p0

    def arguments_argument(self, meta, items):
        p0 = None
        if items[0] is None or items[2] is None:
            p0 = None
        else:
            p0 = make_arg_list(items[0], items[2])
        return p0

    def argument(self, meta, items):
        p0 = None
        p0 = make_argument(items[0], meta_line(meta))
        return p0

    def named_argument(self, meta, items):
        p0 = None
        p0 = make_argument(items[2], meta_line(meta), name=items[0])
        return p0

    def argument_array(self, meta, items):
        p0 = None
        entry = SYMBOL_TABLE.access_array(items[0], get_lineno(items[0]))
        if entry is None:
            p0 = None
            return p0
        mark_entry_as_accessed(entry)
        p0 = make_argument(entry, meta_line(meta))
        return p0

    def funcdecl(self, meta, items):
        p0 = None
        if items[0] is None:
            p0 = None
            return p0
        p0 = items[0]
        p0.local_symbol_table = SYMBOL_TABLE.current_scope
        p0.locals_size = SYMBOL_TABLE.leave_scope()
        FUNCTION_LEVEL.pop()
        p0.entry.ref.body = items[1]
        p0.local_symbol_table.owner = p0
        p0.entry.ref.forwarded = False
        return p0

    def funcdeclforward(self, meta, items):
        p0 = None
        if items[1] is None:
            if FUNCTION_LEVEL:
                FUNCTION_LEVEL.pop()
            return p0
        if items[1].entry.forwarded:
            error(get_lineno(items[0]), "duplicated declaration for function '%s'" % items[1].name)
        items[1].entry.ref.forwarded = True
        SYMBOL_TABLE.leave_scope(show_warnings=False)
        FUNCTION_LEVEL.pop()
        return p0

    def function_header(self, meta, items):
        p0 = None
        p0 = items[0]
        return p0

    def function_header_pre(self, meta, items):
        p0 = None
        if items[0] is None or items[1] is None:
            p0 = None
            return p0
        forwarded = items[0].entry.forwarded
        p0 = items[0]
        p0.append_child(items[1])
        p0.params_size = items[1].size
        lineno = get_lineno(items[2])
        previoustype_ = p0.type_
        if not items[2].implicit or p0.entry.type_ is None or p0.entry.type_ == Type.unknown:
            p0.type_ = items[2]
            if items[2].implicit and p0.entry.class_ == CLASS.function:
                errmsg.warning_implicit_type(items[2].lineno, p0.entry.name, p0.type_.name)
        if forwarded and previoustype_ != p0.type_:
            errmsg.syntax_error_func_type_mismatch(lineno, p0.entry)
            p0 = None
            return p0
        if forwarded:
            p1 = p0.entry.ref.params
            p2 = items[1].children
            if len(p1) != len(p2):
                errmsg.syntax_error_parameter_mismatch(lineno, p0.entry)
                p0 = None
                return p0
            for a, b in zip(p1, p2):
                if a.name != b.name:
                    warning(
                        lineno, "Parameter '%s' in function '%s' has been renamed to '%s'" % (a.name, p0.name, b.name)
                    )
                if a.type_ != b.type_ or a.byref != b.byref:
                    errmsg.syntax_error_parameter_mismatch(lineno, p0.entry)
                    p0 = None
                    return p0
        p0.entry.ref.params = items[1]
        if FUNCTION_LEVEL[-1].class_ == CLASS.sub and (not items[2].implicit):
            error(lineno, "SUBs cannot have a return type definition")
            p0 = None
            return p0
        if FUNCTION_LEVEL[-1].class_ == CLASS.function:
            src.api.check.check_type_is_explicit(p0.lineno, p0.entry.name, items[2])
        if p0.entry.convention == CONVENTION.fastcall and len(items[1]) > 1:
            class_ = "SUB" if FUNCTION_LEVEL[-1].class_ == CLASS.sub else "FUNCTION"
            errmsg.warning_fastcall_with_N_parameters(lineno, class_, p0.entry.name, len(items[1]))
        return p0

    def function_def(self, meta, items):
        p0 = None
        convention = items[1]
        name = items[2]
        class_ = CLASS.sub if items[0] == "SUB" else CLASS.function
        p0 = make_func_declaration(name, get_lineno(items[2]), class_)
        SYMBOL_TABLE.enter_scope(name)
        entry = SYMBOL_TABLE.get_entry(name)
        FUNCTION_LEVEL.append(entry)
        if entry.class_ in (CLASS.function, CLASS.sub):
            FUNCTION_LEVEL[-1].ref.convention = convention
        return p0

    def convention(self, meta, items):
        p0 = None
        p0 = CONVENTION.stdcall
        return p0

    def convention2(self, meta, items):
        p0 = None
        p0 = CONVENTION.fastcall
        return p0

    def param_decl_none(self, meta, items):
        p0 = None
        p0 = make_param_list(None)
        return p0

    def param_decl(self, meta, items):
        p0 = None
        p0 = items[1]
        return p0

    def param_decl_list(self, meta, items):
        p0 = None
        p0 = make_param_list(items[0])
        return p0

    def param_decl_list2(self, meta, items):
        p0 = None
        if items[0] is not None and items[2] is not None:
            if items[2].default_value is None and items[0][-1].default_value is not None:
                errmsg.syntax_error_mandatory_param_after_optional(items[2].lineno, items[0][-1].name, items[2].name)
        p0 = make_param_list(items[0], items[2])
        return p0

    def param_byref_definition(self, meta, items):
        p0 = None
        p0 = items[1]
        if p0 is not None:
            p0.ref.byref = True
        return p0

    def param_byval_definition(self, meta, items):
        p0 = None
        param_def = items[1]
        p0 = param_def
        if p0 is not None:
            if param_def.class_ == CLASS.array:
                errmsg.syntax_error_cannot_pass_array_by_value(get_lineno(items[0]), param_def.name)
                p0 = None
                return p0
            param_def.ref.byref = False
        return p0

    def param_definition(self, meta, items):
        p0 = None
        param_def = items[0]
        p0 = param_def
        if p0 is not None:
            if param_def.class_ == CLASS.array:
                param_def.ref.byref = True
            else:
                param_def.ref.byref = OPTIONS.default_byref
        return p0

    def param_def_array(self, meta, items):
        p0 = None
        typeref = items[3]
        if typeref is None:
            p0 = None
            return p0
        lineno = items[0].lineno
        id_ = items[0].name
        src.api.check.check_type_is_explicit(lineno, id_, typeref)
        p0 = make_param_decl(id_, lineno, typeref, is_array=True)
        return p0

    def param_def_type(self, meta, items):
        p0 = None
        id_: Id = items[0]
        typedef = items[1]
        if typedef is not None:
            src.api.check.check_type_is_explicit(id_.lineno, id_.name, typedef)
        default_value = make_typecast(typedef, items[2], id_.lineno)
        p0 = make_param_decl(id_.name, id_.lineno, typedef, is_array=False, default_value=default_value)
        return p0

    def param_def_default_arg_value(self, meta, items):
        p0 = None
        if len(items) + 1 == 1:
            p0 = None
            return p0
        p0 = items[1]
        return p0
        return p0

    def function_body(self, meta, items):
        p0 = None
        if not FUNCTION_LEVEL:
            end_tok = items[-1]
            error(get_lineno(end_tok), "Unexpected token '%s'. No Function or Sub has been defined." % end_tok)
            p0 = None
            return None
        a = FUNCTION_LEVEL[-1].class_
        if a not in (CLASS.sub, CLASS.function):
            p0 = None
            return None
        end_tok = items[-1]
        b = end_tok.split()[-1].lower()
        if a != b:
            error(get_lineno(end_tok), "Unexpected token '%s'. Should be 'END %s'" % (end_tok.upper(), a.upper()))
            p0 = None
        else:
            p0 = make_block() if len(items) == 1 else items[0]
        return p0

    def type_def_empty(self, meta, items):
        p0 = None
        p0 = make_type(_TYPE(gl.DEFAULT_TYPE).name, meta_line(meta), implicit=True)
        return p0

    def type_def(self, meta, items):
        p0 = None
        p0 = make_type(items[1], get_lineno(items[1]), implicit=False)
        return p0

    def type_def_id(self, meta, items):
        p0 = None
        errmsg.error(get_lineno(items[1]), "Syntax Error. Unexpected token '%s' <ID>" % items[1])
        p0 = make_type("float", get_lineno(items[1]), implicit=False)
        return p0

    def type(self, meta, items):
        p0 = None
        p0 = items[0].lower()
        return p0

    def preproc_line_init(self, meta, items):
        p0 = None
        INITS.add(items[1])
        return p0

    def preproc_line_require(self, meta, items):
        p0 = None
        arch.target.backend.REQUIRES.add(items[1])
        return p0

    def preproc_line_pragma_option(self, meta, items):
        p0 = None
        try:
            setattr(OPTIONS, items[1], items[3])
        except src.api.options.UndefinedOptionError:
            errmsg.warning_ignoring_unknown_pragma(get_lineno(items[1]), items[1])
        return p0

    def preproc_pragma_push(self, meta, items):
        p0 = None
        try:
            OPTIONS[items[3]].push()
        except src.api.options.UndefinedOptionError:
            errmsg.warning_ignoring_unknown_pragma(get_lineno(items[3]), items[3])
        return p0

    def preproc_pragma_pop(self, meta, items):
        p0 = None
        try:
            OPTIONS[items[3]].pop()
        except src.api.options.UndefinedOptionError:
            errmsg.warning_ignoring_unknown_pragma(get_lineno(items[3]), items[3])
        return p0

    def expr_usr(self, meta, items):
        p0 = None
        if items[1].type_ == Type.string:
            p0 = make_builtin(get_lineno(items[0]), "USR_STR", items[1], type_=Type.uinteger)
        else:
            p0 = make_builtin(
                get_lineno(items[0]),
                "USR",
                make_typecast(Type.uinteger, items[1], get_lineno(items[0])),
                type_=Type.uinteger,
            )
        return p0

    def expr_rnd(self, meta, items):
        p0 = None
        p0 = make_builtin(get_lineno(items[0]), "RND", None, type_=Type.float_)
        return p0

    def expr_peek(self, meta, items):
        p0 = None
        p0 = make_builtin(
            get_lineno(items[0]), "PEEK", make_typecast(Type.uinteger, items[1], get_lineno(items[0])), type_=Type.ubyte
        )
        return p0

    def expr_peektype_(self, meta, items):
        p0 = None
        if items[2] is None or items[4] is None:
            p0 = None
            return p0
        p0 = make_builtin(
            get_lineno(items[0]),
            "PEEK",
            make_typecast(Type.uinteger, items[4], get_lineno(items[3])),
            type_=cast(sym.TYPEREF, items[2]).type_,
        )
        return p0

    def expr_in(self, meta, items):
        p0 = None
        p0 = make_builtin(
            get_lineno(items[0]), "IN", make_typecast(Type.uinteger, items[1], get_lineno(items[0])), type_=Type.ubyte
        )
        return p0

    def expr_lbound(self, meta, items):
        p0 = None
        entry = SYMBOL_TABLE.access_array(items[2], get_lineno(items[2]))
        if entry is None:
            p0 = None
            return p0
        mark_entry_as_accessed(entry)
        if entry.scope == SCOPE.parameter:
            num = make_number(0, get_lineno(items[2]), Type.uinteger)
            p0 = make_builtin(get_lineno(items[0]), items[0], [entry, num], type_=Type.uinteger)
        else:
            p0 = make_number(len(entry.bounds), get_lineno(items[2]), Type.uinteger)
        return p0

    def expr_lbound_expr(self, meta, items):
        p0 = None
        expr = items[4]
        if expr is None:
            p0 = None
            return p0
        entry = SYMBOL_TABLE.access_array(items[2], get_lineno(items[2]))
        if entry is None:
            p0 = None
            return p0
        mark_entry_as_accessed(entry)
        num = make_typecast(Type.uinteger, expr, get_lineno(items[5]))
        if num is None:
            p0 = None
            return p0
        if is_number(num) and entry.scope in (SCOPE.local, SCOPE.global_):
            val = num.value
            if val < 0 or val > len(entry.bounds):
                error(get_lineno(items[5]), "Dimension out of range")
                p0 = None
                return p0
            if not val:
                p0 = make_number(len(entry.bounds), get_lineno(items[2]), Type.uinteger)
            elif items[0] == "LBOUND":
                p0 = make_number(entry.bounds[val - 1].lower, get_lineno(items[2]), Type.uinteger)
            else:
                p0 = make_number(entry.bounds[val - 1].upper, get_lineno(items[2]), Type.uinteger)
            return p0
        if items[0] == "LBOUND":
            entry.ref.lbound_used = True
        else:
            entry.ref.ubound_used = True
        p0 = make_builtin(get_lineno(items[0]), items[0], [entry, num], type_=Type.uinteger)
        return p0

    def len(self, meta, items):
        p0 = None
        arg = items[1]
        if arg is None:
            p0 = None
        elif arg.token == "VAR" and arg.class_ == CLASS.array:
            p0 = make_number(len(arg.bounds), lineno=get_lineno(items[0]))
        elif arg.type_ != Type.string:
            errmsg.syntax_error_expected_string(get_lineno(items[0]), Type.to_string(arg.type_))
            p0 = None
        elif is_string(arg):
            p0 = make_number(len(arg.value), lineno=get_lineno(items[0]))
        else:
            p0 = make_builtin(get_lineno(items[0]), "LEN", arg, type_=Type.uinteger)
        return p0

    def sizeof(self, meta, items):
        p0 = None
        if Type.to_type(items[2].lower()) is not None:
            p0 = make_number(Type.size(Type.to_type(items[2].lower())), lineno=get_lineno(items[2]))
        else:
            entry = SYMBOL_TABLE.get_id_or_make_var(items[2], get_lineno(items[0]))
            p0 = make_number(Type.size(entry.type_), lineno=get_lineno(items[2]))
        return p0

    def str(self, meta, items):
        p0 = None
        if is_number(items[1]):
            p0 = sym.STRING(str(items[1].value), get_lineno(items[0]))
        else:
            p0 = make_builtin(
                get_lineno(items[0]),
                "STR",
                make_typecast(Type.float_, items[1], get_lineno(items[0])),
                type_=Type.string,
            )
        return p0

    def inkey(self, meta, items):
        p0 = None
        p0 = make_builtin(get_lineno(items[0]), "INKEY", None, type_=Type.string)
        return p0

    def chr_one(self, meta, items):
        p0 = None
        arg_list = make_arg_list(make_argument(items[1], get_lineno(items[0])))
        arg_list[0].value = make_typecast(Type.ubyte, arg_list[0].value, get_lineno(items[0]))
        p0 = make_builtin(get_lineno(items[0]), "CHR", arg_list, type_=Type.string)
        return p0

    def chr(self, meta, items):
        p0 = None
        if len(items[1]) < 1:
            error(get_lineno(items[0]), "CHR$ function need at less 1 parameter")
            p0 = None
            return p0
        for i in range(len(items[1])):
            items[1][i].value = make_typecast(Type.ubyte, items[1][i].value, get_lineno(items[0]))
        p0 = make_builtin(get_lineno(items[0]), "CHR", items[1], type_=Type.string)
        return p0

    def val(self, meta, items):
        p0 = None

        def val(s):
            try:
                x = float(eval(s, {}, {}))
            except:
                x = 0
                warning(get_lineno(items[0]), f"Invalid string numeric constant '{s}' evaluated as 0")
            return x

        if items[1].type_ != Type.string:
            errmsg.syntax_error_expected_string(get_lineno(items[0]), Type.to_string(items[1].type_))
            p0 = None
        else:
            p0 = make_builtin(get_lineno(items[0]), "VAL", items[1], val, type_=Type.float_)
        return p0

    def code(self, meta, items):
        p0 = None

        def asc(x):
            if len(x):
                return ord(x[0])
            return 0

        if items[1] is None:
            p0 = None
            return p0
        if items[1].type_ != Type.string:
            errmsg.syntax_error_expected_string(get_lineno(items[0]), Type.to_string(items[1].type_))
            p0 = None
        else:
            p0 = make_builtin(get_lineno(items[0]), "CODE", items[1], asc, type_=Type.ubyte)
        return p0

    def sgn(self, meta, items):
        p0 = None
        sgn = lambda x: x < 0 and -1 or (x > 0 and 1) or 0
        if items[1].type_ == Type.string:
            error(get_lineno(items[0]), "Expected a numeric expression, got TYPE.string instead")
            p0 = None
        else:
            if is_unsigned(items[1]) and (not is_number(items[1])):
                warning(get_lineno(items[0]), "Sign of unsigned value is always 0 or 1")
            p0 = make_builtin(get_lineno(items[0]), "SGN", items[1], sgn, type_=Type.byte_)
        return p0

    def expr_trig(self, meta, items):
        p0 = None
        p0 = make_builtin(
            get_lineno(items[0]),
            items[0],
            make_typecast(Type.float_, items[1], get_lineno(items[0])),
            {
                "SIN": math.sin,
                "COS": math.cos,
                "TAN": math.tan,
                "ASN": math.asin,
                "ACS": math.acos,
                "ATN": math.atan,
                "LN": lambda y: math.log(y, math.exp(1)),
                "EXP": math.exp,
                "SQR": math.sqrt,
            }[items[0]],
            type_=Type.float_,
        )
        return p0

    def math_fn(self, meta, items):
        p0 = None
        p0 = items[0]
        return p0

    def expr_int(self, meta, items):
        p0 = None
        p0 = make_typecast(Type.long_, items[1], get_lineno(items[0]))
        return p0

    def abs(self, meta, items):
        p0 = None
        if is_unsigned(items[1]):
            p0 = items[1]
            warning(get_lineno(items[0]), "Redundant operation ABS for unsigned value")
            return p0
        p0 = make_builtin(get_lineno(items[0]), "ABS", items[1], lambda x: x if x >= 0 else -x)
        return p0


def handle_parse_error(e):
    if not isinstance(e, UnexpectedInput):
        raise e
    gl.reporting_syntax_error = True
    try:
        return _handle_parse_error_inner(e)
    finally:
        gl.reporting_syntax_error = False


def _handle_parse_error_inner(e):
    from src.api import errmsg
    from src.api.errmsg import error

    from .zxbparser_standalone import UnexpectedEOF, UnexpectedToken

    is_eof = isinstance(e, UnexpectedEOF) or (isinstance(e, UnexpectedToken) and e.token.type == "$END")

    if not is_eof:
        # UnexpectedToken, not EOF
        tok = e.token
        t_type = tok.type
        t_val = tok.value
        if t_type.startswith("END_") and "_" in t_type:
            t_type = "END"
            t_val = t_val.split()[0]
        if t_type == "NEWLINE":
            msg = "Unexpected end of line"
        else:
            msg = "Syntax Error. Unexpected token '%s' <%s>" % (t_val, t_type)
        if hasattr(gl, "tokens_rejected"):
            gl.tokens_rejected += 1
        yielded = getattr(gl, "tokens_yielded", 0)
        rejected = getattr(gl, "tokens_rejected", 0)
        shifted = yielded - rejected
        shifted_at_last = getattr(gl, "shifted_at_last_error", 0)
        if shifted - shifted_at_last < 3:
            return True
        gl.shifted_at_last_error = shifted
        key = (getattr(gl, "FILENAME", ""), tok.line)
        last_key = getattr(handle_parse_error, "last_key", None)
        if key == last_key:
            return True
        handle_parse_error.last_key = key
        gl.syntax_error_occurred = True
        error(tok.line, msg)
        return True

    # EOF cases
    if gl.LOOPS:  # some loop(s) are not closed
        loop_info = gl.LOOPS[-1]
        if loop_info.type == LoopType.FOR:
            errmsg.syntax_error_for_without_next(loop_info.lineno)
        else:
            errmsg.syntax_error_loop_not_closed(loop_info.lineno, loop_info.type.value)

    if gl.has_errors:
        return False

    msg = "Unexpected end of file"
    error(zxblex.lexer.lineno, msg)
    return False


class LarkParserWrapper:
    def __init__(self, lark_instance):
        self.lark = lark_instance

        # Wrap LALR parser callbacks for inline execution during parsing
        lalr_parser = lark_instance.parser.parser.parser
        transformer = ZXBasicTransformer()
        wrapped_callbacks = {}
        for rule, callback in list(lalr_parser.callbacks.items()):
            method_name = rule.alias if rule.alias is not None else rule.origin.name
            if hasattr(transformer, method_name):
                method = getattr(transformer, method_name)

                def make_wrapper(m=method):
                    def wrapper(children):
                        class MockMeta:
                            @property
                            def line(self):
                                for child in children:
                                    lineno = get_lineno(child)
                                    if lineno:
                                        return lineno
                                return zxblex.lexer.lineno if hasattr(zxblex, "lexer") else 0

                        meta = MockMeta()
                        return m(meta, children)

                    return wrapper

                wrapped_callbacks[rule] = make_wrapper()
            else:
                wrapped_callbacks[rule] = callback
        lalr_parser.callbacks = wrapped_callbacks

    def parse(self, text, lexer=None, tracking=True, debug=False):
        if hasattr(handle_parse_error, "last_key"):
            handle_parse_error.last_key = None
        if lexer is not None:
            lexer.input(text)
            try:
                self.lark.parse(lexer, on_error=handle_parse_error)
            except Exception as e:
                if not isinstance(e, UnexpectedInput):
                    raise e
        else:
            try:
                self.lark.parse(text, on_error=handle_parse_error)
            except Exception as e:
                if not isinstance(e, UnexpectedInput):
                    raise e


# DO NOT pass transformer=ZXBasicTransformer() to Lark constructor when using meta args
lark_parser = Lark_StandAlone(lexer=ZXBasicLarkLexerAdapter, propagate_positions=True)
parser = LarkParserWrapper(lark_parser)
