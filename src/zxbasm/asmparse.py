# --------------------------------------------------------------------
# SPDX-License-Identifier: AGPL-3.0-or-later
# © Copyright 2008-2026 José Manuel Rodríguez de la Rosa and contributors.
# See the file CONTRIBUTORS.md for copyright details.
# See https://www.gnu.org/licenses/agpl-3.0.html for details.
# --------------------------------------------------------------------

import os
from typing import Any

import src.api.utils
from src import outfmt
from src.api import global_ as gl
from src.api.config import OPTIONS
from src.api.debug import __DEBUG__
from src.api.errmsg import error, warning
from src.zxbasm import asmlex, basic
from src.zxbasm import global_ as asm_gl
from src.zxbasm.asm import Asm, Container
from src.zxbasm.asmlex import tokens  # noqa
from src.zxbasm.expr import Expr
from src.zxbasm.global_ import DOT
from src.zxbasm.memory import Memory
from src.zxbpp import zxbpp

from .asmparse_standalone import Lark_StandAlone as BaseLarkStandAlone
from .asmparse_standalone import Lexer, Token, Transformer, UnexpectedInput
from .asmparse_zxnext_standalone import Lark_StandAlone as ZXNextLarkStandAlone

LEXER = asmlex.Lexer()

ORG = 0  # Origin of CODE
INITS = []
MEMORY: Memory | None = None  # Memory for instructions (Will be initialized with a Memory() instance)
AUTORUN_ADDR = None  # Where to start the execution automatically

REGS16 = {"BC", "DE", "HL", "SP", "IX", "IY"}  # 16 Bits registers


def init():
    """Initializes this module"""
    global ORG
    global LEXER
    global MEMORY
    global INITS
    global AUTORUN_ADDR

    ORG = 0  # Origin of CODE
    INITS = []
    MEMORY = None  # Memory for instructions (Will be initialized with a Memory() instance)
    AUTORUN_ADDR = None  # Where to start the execution automatically
    gl.has_errors = 0
    gl.error_msg_cache.clear()

    # Current namespace (defaults to ''). It's a prefix added to each global label
    asm_gl.NAMESPACE = asm_gl.GLOBAL_NAMESPACE


class AsmToken(Token):
    pass


class AsmLarkLexerAdapter(Lexer):
    def __init__(self, lexer_conf: Any) -> None:
        pass

    def lex(self, data: Any, parser_state: Any = None) -> Any:  # type: ignore[override]
        lexer = data
        while True:
            if lexer.next_token is not None:
                tok_type = lexer.next_token
                lexer.next_token = None
                t = AsmToken(tok_type, "", line=lexer.lineno, column=1)
                yield t
                continue

            tok = lexer.token()
            if tok is None:
                break

            t = AsmToken(tok.type, tok.value, line=tok.lineno, column=lexer.find_column(tok))
            yield t


class AsmTransformer(Transformer):
    def start(self, items):
        return items[0]

    def program(self, items):
        return items[0]

    def empty_program(self, items):
        return None

    def program_endline2(self, items):
        global AUTORUN_ADDR
        AUTORUN_ADDR = items[1].eval()
        return items[0]

    def def_label(self, items):
        MEMORY.declare_label(items[0], items[0].line, items[2])

    def line_asm(self, items):
        return None

    def preprocessor_line(self, items):
        return None

    def preproc_line_init(self, items):
        global INITS
        INITS.append(Container(items[0].strip('"'), items[0].line))

    def asms_empty(self, items):
        return MEMORY.org

    def asms_asm(self, items):
        asm = items[0]
        if isinstance(asm, Asm):
            MEMORY.add_instruction(asm)
        return MEMORY.org

    def asms_asms_asm(self, items):
        asm = items[2]
        if isinstance(asm, Asm):
            MEMORY.add_instruction(asm)
        return items[0]

    def asm_label(self, items):
        MEMORY.declare_label(str(items[0]), items[0].line)

    def asm_ld8(self, items):
        if items[1] in ("H", "L") and items[3] in ("IXH", "IXL", "IYH", "IYL"):
            error(items[0].line, "Unexpected token '%s'" % items[3])
            return None
        return Asm(items[0].line, "LD %s,%s" % (items[1], items[3]))

    def ld_a_instr(self, items):
        return Asm(items[0].line, "LD " + "".join(str(x).replace("[", "(").replace("]", ")") for x in items[1:]))

    def proc_scope(self, items):
        MEMORY.enter_proc(items[0].line)

    def endp_scope(self, items):
        MEMORY.exit_proc(items[0].line)

    def local_labels(self, items):
        for label, line in items[1]:
            MEMORY.set_label(label, line, local=True)

    def idlist(self, items):
        return (Container(items[0], items[0].line),)

    def idlist_id(self, items):
        return items[0] + (Container(items[2], items[2].line),)

    def defb_op(self, items):
        return Asm(items[0].line, "DEFB", items[1])

    def defs_op(self, items):
        num_list = items[1]
        if len(num_list) > 2:
            error(items[0].line, "too many arguments for DEFS")
        if len(num_list) < 2:
            num = Expr.makenode(Container(0, items[0].line))
            num_list = num_list + (num,)
        return Asm(items[0].line, "DEFS", num_list)

    def defw_op(self, items):
        return Asm(items[0].line, "DEFW", items[1])

    def expr_list_from_string(self, items):
        return tuple(Expr.makenode(Container(ord(x), items[0].line)) for x in items[0])

    def expr_list_from_num(self, items):
        return (items[0],)

    def expr_list_plus_expr(self, items):
        return items[0] + (items[2],)

    def expr_list_plus_string(self, items):
        return items[0] + tuple(Expr.makenode(Container(ord(x), items[2].line)) for x in items[2])

    def number_list(self, items):
        return (items[0],)

    def number_list_number(self, items):
        return items[0] + (items[2],)

    def asm_ldind_r8(self, items):
        return Asm(items[0].line, "LD %s,%s" % (items[1][0], items[3]), items[1][1])

    def asm_ldr8_ind(self, items):
        return Asm(items[0].line, "LD %s,%s" % (items[1], items[3][0]), items[3][1])

    def reg8_hl(self, items):
        return "(HL)"

    def ind8_i(self, items):
        if len(items) == 5:
            expr = items[3]
            sign = items[2]
        else:
            expr = items[2]
            gen_ = expr.inorder()
            first_expr = next(gen_, "")
            if first_expr and first_expr.parent:
                if len(first_expr.parent.children) == 2:
                    first_token = first_expr.symbol.item
                else:
                    first_token = first_expr.parent.symbol.item
            else:
                first_token = "<nothing>"
            if first_token not in ("-", "+"):
                error(items[1].line, f"Unexpected token '{first_token}'. Expected '+' or '-'")
            sign = "+"

        if sign == "-":
            expr = Expr.makenode(Container(sign, items[1].line), expr)

        return ("(%s+N)" % items[1], expr)

    def ex_af_af(self, items):
        return Asm(items[0].line, "EX AF,AF'")

    def ex_de_hl(self, items):
        return Asm(items[0].line, "EX DE,HL")

    def org(self, items):
        MEMORY.set_org(items[1].eval(), items[0].line)

    def namespace(self, items):
        asm_gl.NAMESPACE = asm_gl.normalize_namespace(items[1])
        __DEBUG__("Setting namespace to " + (asm_gl.NAMESPACE or DOT), level=1)

    def push_namespace(self, items):
        asm_gl.NAMESPACE_STACK.append(asm_gl.NAMESPACE)
        asm_gl.NAMESPACE = asm_gl.normalize_namespace(items[2] if len(items) == 3 else asm_gl.NAMESPACE)
        if asm_gl.NAMESPACE != asm_gl.NAMESPACE_STACK[-1]:
            __DEBUG__("Setting namespace to " + (asm_gl.NAMESPACE or DOT), level=1)

    def pop_namespace(self, items):
        if not asm_gl.NAMESPACE_STACK:
            error(items[1].line, f"Stack underflow. No more Namespaces to pop. Current namespace is {asm_gl.NAMESPACE}")
        else:
            asm_gl.NAMESPACE = asm_gl.NAMESPACE_STACK.pop()

    def align(self, items):
        align = items[1].eval()
        if align < 2:
            error(items[0].line, "ALIGN value must be greater than 1")
            return None
        MEMORY.set_org(MEMORY.org + (align - MEMORY.org % align) % align, items[0].line)
        return None

    def incbin(self, items):
        try:
            fname = zxbpp.search_filename(items[1], items[1].line, local_first=True)
            if not fname:
                return None
            with src.api.utils.open_file(fname, "rb") as f:
                filecontent = f.read()
        except IOError:
            error(items[1].line, "cannot read file '%s'" % items[1])
            return None

        offset = 0
        length = None

        if len(items) > 3:
            offset = items[3].eval()

        if len(items) > 5:
            length = items[5].eval()
            if length < 1:
                error(items[0].line, "INCBIN length must be greater than 0")

        if offset < 0:
            offset = len(filecontent) + offset
            if offset < 0 or offset >= len(filecontent):
                error(items[0].line, "INCBIN offset is out of range")

        if length is None:
            length = len(filecontent) - offset

        if offset + length > len(filecontent):
            excess = len(filecontent) - (offset + length)
            warning(items[0].line, f"INCBIN length if beyond file length by {excess} bytes")

        filecontent = filecontent[offset : offset + length]
        return Asm(items[0].line, "DEFB", filecontent)

    def ex_sp_reg8(self, items):
        return Asm(items[0].line, "EX (SP)," + items[5])

    def incdec(self, items):
        return Asm(items[0].line, "%s %s" % (items[0], items[1]))

    def incdeci(self, items):
        return Asm(items[0].line, "%s %s" % (items[0], items[1][0]), items[1][1])

    def ld_reg_val(self, items):
        s = "LD %s,N" % items[1]
        if items[1] in REGS16:
            s += "N"
        return Asm(items[0].line, s, items[3])

    def ld_reg_val_i(self, items):
        return Asm(items[0].line, "LD %s,N" % items[1][0], (items[1][1], items[3]))

    def jp_hl(self, items):
        s = "JP "
        if items[1] == "(HL)":
            s += items[1]
        else:
            s += "(%s)" % items[2]
        return Asm(items[0].line, s)

    def sbcadd(self, items):
        return Asm(items[0].line, "%s %s,%s" % (items[0], items[1], items[3]))

    def arith_a_expr(self, items):
        return Asm(items[0].line, "%s A,N" % items[0], items[3])

    def arith_a_reg_i(self, items):
        return Asm(items[0].line, "%s A,%s" % (items[0], items[3][0]), items[3][1])

    def bitwiseop_reg(self, items):
        return Asm(items[0][1], "%s %s" % (items[0][0], items[1]))

    def bitwiseop_reg_i(self, items):
        return Asm(items[0][1], "%s %s" % (items[0][0], items[1][0]), items[1][1])

    def bitwise_expr(self, items):
        return Asm(items[0][1], "%s N" % items[0][0], items[1])

    def bitwise(self, items):
        return (items[0], items[0].line)

    def push_pop(self, items):
        return Asm(items[0].line, "%s %s" % (items[0], items[1]))

    def ld_addr_reg(self, items):
        return Asm(items[0].line, "LD (NN),%s" % items[3], items[1])

    def ld_reg_addr(self, items):
        return Asm(items[0].line, "LD %s,(NN)" % items[1], items[3])

    def rotate(self, items):
        return Asm(items[0][1], "%s %s" % (items[0][0], items[1]))

    def rotate_ix(self, items):
        return Asm(items[0][1], "%s %s" % (items[0][0], items[1][0]), items[1][1])

    def bit(self, items):
        bit = items[1].eval()
        if bit < 0 or bit > 7:
            error(items[2].line, "Invalid bit position %i. Must be in [0..7]" % bit)
            return None
        return Asm(items[2].line, "%s %i,%s" % (items[0], bit, items[3]))

    def bit_ix(self, items):
        bit = items[1].eval()
        if bit < 0 or bit > 7:
            error(items[2].line, "Invalid bit position %i. Must be in [0..7]" % bit)
            return None
        return Asm(items[2].line, "%s %i,%s" % (items[0], bit, items[3][0]), items[3][1])

    def bitop(self, items):
        return items[0]

    def rotation(self, items):
        return (items[0], items[0].line)

    def reg_inc(self, items):
        return items[0]

    def reg8(self, items):
        return items[0]

    def reg_bcde(self, items):
        return items[0]

    def reg8i(self, items):
        return items[0]

    def reg16(self, items):
        return items[0]

    def reg16i(self, items):
        return items[0]

    def jp(self, items):
        return Asm(items[0].line, "%s %s,NN" % (items[0], items[1]), items[3])

    def ret(self, items):
        return Asm(items[0].line, "RET %s" % items[1])

    def jpflags_other(self, items):
        return items[0]

    def jr(self, items):
        expr = Expr.makenode(
            Container("-", items[2].line), items[3], Expr.makenode(Container(MEMORY.org + 2, items[0].line))
        )
        return Asm(items[0].line, "JR %s,N" % items[1], expr)

    def jr_flags(self, items):
        return items[0]

    def jrjp(self, items):
        if items[0] in ("JR", "DJNZ"):
            op = "N"
            expr = Expr.makenode(
                Container("-", items[0].line), items[1], Expr.makenode(Container(MEMORY.org + 2, items[0].line))
            )
        else:
            op = "NN"
            expr = items[1]
        return Asm(items[0].line, items[0] + " " + op, expr)

    def rst(self, items):
        val = items[1].eval()
        if val not in (0, 8, 16, 24, 32, 40, 48, 56):
            error(items[0].line, "Invalid RST number %i" % val)
            return None
        return Asm(items[0].line, "RST %XH" % val)

    def im(self, items):
        val = items[1].eval()
        if val not in (0, 1, 2):
            error(items[0].line, "Invalid IM number %i" % val)
            return None
        return Asm(items[0].line, "IM %i" % val)

    def in_op(self, items):
        return Asm(items[0].line, "IN %s,(C)" % items[1])

    def out_op(self, items):
        return Asm(items[0].line, "OUT (C),%s" % items[5])

    def in_expr(self, items):
        return Asm(items[0].line, "IN A,(N)", items[3])

    def out_expr(self, items):
        return Asm(items[0].line, "OUT (N),A", items[1])

    def single(self, items):
        return Asm(items[0].line, items[0])

    def mul_d_e(self, items):
        return Asm(items[0].line, "MUL D,E")

    def simple_instruction(self, items):
        return Asm(items[0].line, items[0])

    def add_reg16_a(self, items):
        return Asm(items[0].line, f"ADD {items[1]},A")

    def jp_c(self, items):
        return Asm(items[0].line, "JP (C)")

    def bxxxx_de_b(self, items):
        return Asm(items[0].line, f"{items[0]} DE,B")

    def add_reg_nn(self, items):
        return Asm(items[0].line, f"ADD {items[1]},NN", items[3])

    def test_nn(self, items):
        return Asm(items[0].line, "TEST N", items[1])

    def nextreg_expr(self, items):
        return Asm(items[0].line, "NEXTREG N,N", (items[1], items[3]))

    def nextreg_a(self, items):
        return Asm(items[0].line, "NEXTREG N,A", items[1])

    def push_imm(self, items):
        mknod = Expr.makenode
        cont = lambda x: Container(x, items[0].line)
        ff = mknod(cont(0xFF))
        n8 = mknod(cont(8))
        expr = mknod(
            cont("|"),
            mknod(cont("<<"), mknod(cont("&"), items[1], ff), n8),
            mknod(cont("&"), mknod(cont(">>"), items[1], n8), ff),
        )
        return Asm(items[0].line, "PUSH NN", expr)

    def expr_div_expr(self, items):
        return Expr.makenode(Container(items[1], items[1].line), items[0], items[2])

    def expr_add_minus_expr(self, items):
        return Expr.makenode(Container(items[1], items[1].line), items[0], items[2])

    def expr_lprp(self, items):
        return items[1]

    def mem_indir(self, items):
        return items[1]

    def expr_uminus(self, items):
        return Expr.makenode(Container(items[0], items[0].line), items[1])

    def expr_uplus(self, items):
        return Expr.makenode(Container(items[0], items[0].line), items[1])

    def expr_int(self, items):
        return Expr.makenode(Container(int(items[0]), items[0].line))

    def expr_label(self, items):
        return Expr.makenode(Container(MEMORY.get_label(items[0], items[0].line), items[0].line))

    def expr_paren(self, items):
        return items[1]

    def expr_addr(self, items):
        return Expr.makenode(Container(MEMORY.org, items[0].line))


def assemble(input_):
    """Assembles input string, and leave the result in the
    MEMORY global object
    """
    global MEMORY

    if MEMORY is None:
        MEMORY = Memory()

    if OPTIONS.zxnext:
        parser_ = zxnext_parser
    else:
        parser_ = parser

    logical_lines = []
    current_buffer = []
    raw_lines = input_.splitlines()
    for line in raw_lines:
        current_buffer.append(line)
        if line.rstrip(" \t").endswith("\\"):
            continue
        else:
            logical_lines.append("\n".join(current_buffer))
            current_buffer = []
    if current_buffer:
        logical_lines.append("\n".join(current_buffer))

    current_lineno = 1
    for line in logical_lines:
        LEXER.input(line + "\n")
        LEXER.lineno = current_lineno
        try:
            parser_.parse(LEXER)
            current_lineno = LEXER.lineno
        except UnexpectedInput as e:
            from .asmparse_standalone import UnexpectedToken

            if isinstance(e, UnexpectedToken):
                tok = e.token
                if tok.type == "$END":
                    OPTIONS.stderr.write("General syntax error at assembler (unexpected End of File?)")
                    gl.has_errors += 1
                elif tok.type == "NEWLINE":
                    error(current_lineno, "Syntax error. Unexpected end of line [NEWLINE]")
                else:
                    error(current_lineno, "Syntax error. Unexpected token '%s' [%s]" % (tok.value, tok.type))
            else:
                error(current_lineno, f"Syntax error at line {current_lineno}, column {e.column}")
            current_lineno += line.count("\n") + 1

    if len(MEMORY.scopes):
        error(MEMORY.scopes[-1], "Missing ENDP to close this scope")

    return gl.has_errors


def generate_binary(outputfname, format_, progname="", binary_files=None, headless_binary_files=None, emitter=None):
    """Outputs the memory binary to the
    output filename using one of the given
    formats: tap, tzx, sna, z80 or bin
    """
    global AUTORUN_ADDR

    org, binary = MEMORY.dump()
    if gl.has_errors:
        return

    if binary_files is None:
        binary_files = []

    if headless_binary_files is None:
        headless_binary_files = []

    bin_blocks = []
    for fname in binary_files:
        with src.api.utils.open_file(fname) as f:
            bin_blocks.append((os.path.basename(fname), f.read()))

    headless_bin_blocks = []
    for fname in headless_binary_files:
        with src.api.utils.open_file(fname) as f:
            headless_bin_blocks.append(f.read())

    if AUTORUN_ADDR is None:
        AUTORUN_ADDR = org

    if not progname:
        progname = os.path.basename(outputfname)[:10]

    loader_bytes = None

    if OPTIONS.use_basic_loader:
        program = basic.Basic()
        if org > 16383:  # Only for zx48k: CLEAR if above 16383
            program.add_line([["CLEAR", org - 1]])
        program.add_line([["LOAD", '""', program.token("CODE")]])

        if OPTIONS.autorun:
            program.add_line([["RANDOMIZE", program.token("USR"), AUTORUN_ADDR]])
        else:
            program.add_line([["REM"], ["RANDOMIZE", program.token("USR"), AUTORUN_ADDR]])

        loader_bytes = program.bytes

    if emitter is None:
        if format_ in ("tap", "tzx"):
            emitter = {"tap": outfmt.TAP, "tzx": outfmt.TZX}[format_]()
        elif format_ == "sna":
            emitter = outfmt.SnaEmitter()
        elif format_ == "z80":
            emitter = outfmt.Z80Emitter()
        else:
            emitter = outfmt.BinaryEmitter()

    assert isinstance(emitter, outfmt.CodeEmitter)
    emitter.emit(
        output_filename=outputfname,
        program_name=progname,
        loader_bytes=loader_bytes,
        entry_point=AUTORUN_ADDR,
        program_bytes=binary,
        aux_bin_blocks=bin_blocks,
        aux_headless_bin_blocks=headless_bin_blocks,
    )


def main(argv):
    """This is a test and will assemble the file in argv[0]"""
    init()

    if OPTIONS.stderr_filename:
        OPTIONS.stderr = open("wt", OPTIONS.stderr_filename)

    asmlex.FILENAME = OPTIONS.input_filename = argv[0]
    input_ = open(OPTIONS.input_filename, "rt").read()
    assemble(input_)
    generate_binary(OPTIONS.output_filename, OPTIONS.output_file_type)


parser = BaseLarkStandAlone(lexer=AsmLarkLexerAdapter, transformer=AsmTransformer())

zxnext_parser = ZXNextLarkStandAlone(lexer=AsmLarkLexerAdapter, transformer=AsmTransformer())
