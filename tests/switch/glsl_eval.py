"""A small GLSL subset interpreter for host tests of generated shader text.

It runs the functions of a GLSL text on the CPU with IEEE single precision
(every float result is rounded to binary32, overflow gives infinity). min, max
and clamp follow the Maxwell FMNMX rule (a NaN operand yields the other one);
pow is exp2(y * log2(x)) like the hardware; fma rounds once (FFMA). Enough of
GLSL 4.3 for the deko3d per-pixel lighting text (gfx_deko3d_lighting.cpp) and
the TEV stages of the generated fragment texts (test_tev_passthrough.py):
structs, uniform blocks, inputs, functions with in/out parameters,
if/for/return/continue/break, the vector types with swizzles, and the
built-ins those texts use. For the grass level-of-detail text
(tools/oot3d/deko3d_grass/grass_lod_common.glsl, see test_grass_lod.py) also:
uint scalars that stay unsigned (logical >>, 32-bit wrap), hex literals, the
bitwise compound assignments, uniform blocks without an instance name,
floatBitsToUint and uintBitsToFloat.

    program = Program(text)
    program.globals["pica_view"] = Vec([0.0, 0.0, -1.0])
    result = program.call("pica_surface_primary", Vec([0.5, 0.5, 0.5]), ...)
"""
import math
from fractions import Fraction
import re
import struct

# ---- binary32 arithmetic ------------------------------------------------------

_F32_MAX = 3.4028234663852886e38


def f32(value):
    value = float(value)
    if math.isnan(value) or math.isinf(value):
        return value
    if abs(value) > _F32_MAX:
        # Round to nearest: values within half an ulp of FLT_MAX stay finite.
        if abs(value) >= 3.4028235677973366e38:
            return math.copysign(math.inf, value)
        return math.copysign(_F32_MAX, value)
    return struct.unpack("<f", struct.pack("<f", value))[0]


def _div(a, b):
    if b == 0.0:
        if a == 0.0 or math.isnan(a):
            return math.nan
        return math.copysign(math.inf, a) * math.copysign(1.0, b)
    return f32(a / b)


def _mul(a, b):
    try:
        return f32(a * b)
    except (OverflowError, ValueError):
        return math.nan


def _round_f32(exact):
    """A rational rounded once to binary32 (nearest, ties to even)."""
    if exact == 0:
        return 0.0
    sign = -1.0 if exact < 0 else 1.0
    magnitude = abs(exact)
    exponent = magnitude.numerator.bit_length() - magnitude.denominator.bit_length()
    if Fraction(2) ** exponent > magnitude:
        exponent -= 1
    exponent = max(exponent, -126)               # subnormals keep the 2^-149 step
    step = Fraction(2) ** (exponent - 23)
    mantissa = round(magnitude / step)           # Fraction rounds half to even
    return f32(sign * float(mantissa * step))    # exact in binary64; f32 handles overflow


def _fma(a, b, c):
    """fma() with one rounding (FFMA)."""
    if any(math.isnan(v) or math.isinf(v) for v in (a, b, c)):
        return f32(a * b + c) if not ((a == 0.0 and math.isinf(b)) or (b == 0.0 and math.isinf(a))) else math.nan
    return _round_f32(Fraction(a) * Fraction(b) + Fraction(c))


def _fmin(a, b):
    if math.isnan(a):
        return b
    if math.isnan(b):
        return a
    return min(a, b)


def _fmax(a, b):
    if math.isnan(a):
        return b
    if math.isnan(b):
        return a
    return max(a, b)


def _log2(x):
    if math.isnan(x) or x < 0.0:
        return math.nan
    if x == 0.0:
        return -math.inf
    if math.isinf(x):
        return math.inf
    return f32(math.log2(x))


def _exp2(x):
    if math.isnan(x):
        return math.nan
    if x == -math.inf:
        return 0.0
    if x > 128.0:
        return math.inf
    return f32(2.0 ** x)


def _rsq(x):
    if math.isnan(x) or x < 0.0:
        return math.nan
    if x == 0.0:
        return math.inf
    if math.isinf(x):
        return 0.0
    return f32(1.0 / math.sqrt(x))


def _sqrt(x):
    if math.isnan(x) or x < 0.0:
        return math.nan
    return f32(math.sqrt(x))


# ---- values -------------------------------------------------------------------

class Vec:
    """A float, int, uint or bool vector (kind 'f', 'i', 'u', 'b')."""

    __slots__ = ("c", "kind")

    def __init__(self, components, kind="f"):
        self.c = list(components)
        self.kind = kind

    def copy(self):
        return Vec(self.c, self.kind)

    def __repr__(self):
        return f"Vec({self.c}, {self.kind!r})"


def _copy(value):
    if isinstance(value, Vec):
        return value.copy()
    if isinstance(value, dict):
        return {k: _copy(v) for k, v in value.items()}
    if isinstance(value, list):
        return [_copy(v) for v in value]
    return value


_SWIZZLE = {"x": 0, "y": 1, "z": 2, "w": 3, "r": 0, "g": 1, "b": 2, "a": 3, "s": 0, "t": 1, "p": 2, "q": 3}
_VECTOR_TYPES = {
    "vec2": ("f", 2), "vec3": ("f", 3), "vec4": ("f", 4),
    "ivec2": ("i", 2), "ivec3": ("i", 3), "ivec4": ("i", 4),
    "uvec2": ("u", 2), "uvec3": ("u", 3), "uvec4": ("u", 4),
    "bvec2": ("b", 2), "bvec3": ("b", 3), "bvec4": ("b", 4),
}
_SCALAR_TYPES = {"float": "f", "int": "i", "uint": "u", "bool": "b"}


class _UInt(int):
    """A uint scalar: its kind survives arithmetic (logical >>, unsigned wrap)."""
    __slots__ = ()


def _convert(value, kind):
    if kind == "f":
        return f32(value) if not isinstance(value, bool) else (1.0 if value else 0.0)
    if kind == "i":
        if isinstance(value, float):
            if math.isnan(value) or math.isinf(value):
                return 0
            value = int(value)
        value = int(value) & 0xFFFFFFFF
        return value - (1 << 32) if value >= 1 << 31 else value
    if kind == "u":
        if isinstance(value, float):
            if math.isnan(value) or value <= 0.0:
                return 0
            if math.isinf(value):
                return 0xFFFFFFFF
            value = int(value)
        return _UInt(int(value) & 0xFFFFFFFF)
    return bool(value)


def _zero(type_name, structs, array=None):
    if array is not None:
        return [_zero(type_name, structs) for _ in range(array)]
    if type_name in _VECTOR_TYPES:
        kind, n = _VECTOR_TYPES[type_name]
        return Vec([_convert(0, kind)] * n, kind)
    if type_name in _SCALAR_TYPES:
        return _convert(0, _SCALAR_TYPES[type_name])
    if type_name in structs:
        return {name: _zero(t, structs, a) for t, name, a in structs[type_name]}
    raise ValueError(f"unknown type {type_name}")


def _kind_of(value):
    if isinstance(value, Vec):
        return value.kind
    if isinstance(value, bool):
        return "b"
    if isinstance(value, float):
        return "f"
    if isinstance(value, _UInt):
        return "u"
    return "i"


# ---- parsing --------------------------------------------------------------------

_TOKEN = re.compile(r"""
    (?P<num>0[xX][0-9a-fA-F]+[uU]?|(\d+\.\d*|\.\d+|\d+)([eE][+-]?\d+)?[uUfF]?)
  | (?P<id>[A-Za-z_]\w*)
  | (?P<op><<=|>>=|\+\+|--|&&|\|\||==|!=|<=|>=|<<|>>|\+=|-=|\*=|/=|&=|\|=|\^=|%=|[-+*/%<>=!&|^~?:;,.(){}\[\]])
  | (?P<ws>\s+)
""", re.X)


def tokenize(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    text = re.sub(r"^\s*#[^\n]*", " ", text, flags=re.M)
    tokens = []
    position = 0
    while position < len(text):
        match = _TOKEN.match(text, position)
        if match is None:
            raise SyntaxError(f"bad character {text[position]!r} at {position}")
        position = match.end()
        if match.lastgroup == "ws":
            continue
        tokens.append(match.group())
    return tokens


class _Stream:
    def __init__(self, tokens):
        self.tokens = tokens
        self.at = 0

    def peek(self, offset=0):
        index = self.at + offset
        return self.tokens[index] if index < len(self.tokens) else None

    def take(self, expected=None):
        token = self.peek()
        if expected is not None and token != expected:
            raise SyntaxError(f"expected {expected!r}, got {token!r} at token {self.at}")
        self.at += 1
        return token

    def accept(self, token):
        if self.peek() == token:
            self.at += 1
            return True
        return False


_QUALIFIERS = {"in", "out", "inout", "const", "precise", "highp", "mediump", "lowp", "flat", "smooth", "uniform",
               "invariant", "noperspective"}

# Binary operators by precedence (low to high).
_BINARY = [["||"], ["^^"], ["&&"], ["|"], ["^"], ["&"], ["==", "!="], ["<", ">", "<=", ">="], ["<<", ">>"],
           ["+", "-"], ["*", "/", "%"]]


class Program:
    def __init__(self, text):
        self.structs = {}
        self.functions = {}
        self.globals = {}
        self.global_types = {}
        self._parse(_Stream(tokenize(text)))

    # -- declarations --
    def _is_type(self, token):
        return token in _VECTOR_TYPES or token in _SCALAR_TYPES or token in self.structs or token == "void"

    def _skip_layout(self, s):
        if s.peek() == "layout":
            s.take()
            depth = 0
            while True:
                token = s.take()
                if token == "(":
                    depth += 1
                elif token == ")":
                    depth -= 1
                    if depth == 0:
                        break

    def _parse(self, s):
        while s.peek() is not None:
            self._skip_layout(s)
            qualifiers = set()
            while s.peek() in _QUALIFIERS:
                qualifiers.add(s.take())
            token = s.peek()
            if token == "struct":
                s.take()
                name = s.take()
                self.structs[name] = self._members(s)
                s.take(";")
                continue
            if "uniform" in qualifiers and not self._is_type(token) and s.peek(1) == "{":
                block = s.take()
                members = self._members(s)
                self.structs[block] = members
                if s.accept(";"):
                    # No instance name: the members are globals.
                    for member_type, member, array in members:
                        self.globals[member] = _zero(member_type, self.structs, array)
                        self.global_types[member] = member_type
                    continue
                instance = s.take()
                s.take(";")
                self.globals[instance] = _zero(block, self.structs)
                self.global_types[instance] = block
                continue
            type_name = s.take()
            if "uniform" in qualifiers and type_name.startswith("sampler"):
                s.take()
                s.take(";")
                continue
            name = s.take()
            if s.peek() == "(":
                params = self._params(s)
                if s.accept(";"):
                    continue  # prototype
                start = s.at
                self._skip_block(s)
                self.functions[name] = (type_name, params, s.tokens[start:s.at])
                continue
            array = None
            if s.accept("["):
                array = int(s.take())
                s.take("]")
            value = _zero(type_name, self.structs, array)
            if s.accept("="):
                value = self._eval_constant(s, type_name)
            s.take(";")
            self.globals[name] = value
            self.global_types[name] = type_name

    def _eval_constant(self, s, type_name):
        start = s.at
        depth = 0
        while not (depth == 0 and s.peek() == ";"):
            token = s.take()
            depth += token in "([{"
            depth -= token in ")]}"
        expression = _Stream(s.tokens[start:s.at])
        return self._coerce(_Interpreter(self, {}).expression(expression), type_name)

    def _members(self, s):
        members = []
        s.take("{")
        while not s.accept("}"):
            while s.peek() in _QUALIFIERS:
                s.take()
            type_name = s.take()
            while True:
                name = s.take()
                array = None
                if s.accept("["):
                    array = int(s.take())
                    s.take("]")
                members.append((type_name, name, array))
                if not s.accept(","):
                    break
            s.take(";")
        return members

    def _params(self, s):
        params = []
        s.take("(")
        while not s.accept(")"):
            mode = "in"
            while s.peek() in _QUALIFIERS:
                token = s.take()
                if token in ("out", "inout"):
                    mode = token
            type_name = s.take()
            if type_name == "void":
                continue
            name = s.take()
            params.append((mode, type_name, name))
            s.accept(",")
        return params

    @staticmethod
    def _skip_block(s):
        s.take("{")
        depth = 1
        while depth:
            token = s.take()
            depth += token == "{"
            depth -= token == "}"

    def _coerce(self, value, type_name):
        if type_name in _SCALAR_TYPES and not isinstance(value, (Vec, dict, list)):
            return _convert(value, _SCALAR_TYPES[type_name])
        if type_name in _VECTOR_TYPES and isinstance(value, Vec):
            kind, n = _VECTOR_TYPES[type_name]
            return Vec([_convert(v, kind) for v in value.c[:n]], kind)
        return _copy(value)

    def call(self, name, *args):
        """Calls a function with values (out parameters are not returned)."""
        return _Interpreter(self, {}).call_user(name, list(args))


class _Return(Exception):
    def __init__(self, value):
        self.value = value


class _Break(Exception):
    pass


class _Continue(Exception):
    pass


class _Ref:
    """An assignable location: a container and a key, optionally a swizzle."""

    def __init__(self, container, key, swizzle=None):
        self.container = container
        self.key = key
        self.swizzle = swizzle

    def get(self):
        value = self.container[self.key]
        if self.swizzle is None:
            return value
        if len(self.swizzle) == 1:
            return value.c[self.swizzle[0]]
        return Vec([value.c[i] for i in self.swizzle], value.kind)

    def set(self, value):
        if self.swizzle is None:
            old = self.container[self.key]
            if isinstance(old, Vec) and isinstance(value, Vec):
                value = Vec([_convert(v, old.kind) for v in value.c], old.kind)
            elif not isinstance(old, (Vec, dict, list)) and not isinstance(value, (Vec, dict, list)):
                value = _convert(value, _kind_of(old))
            self.container[self.key] = _copy(value)
            return
        target = self.container[self.key]
        components = value.c if isinstance(value, Vec) else [value] * len(self.swizzle)
        for i, index in enumerate(self.swizzle):
            target.c[index] = _convert(components[i], target.kind)


class _Interpreter:
    def __init__(self, program, scope):
        self.program = program
        self.scopes = [scope]

    # -- scopes --
    def lookup(self, name):
        for scope in reversed(self.scopes):
            if name in scope:
                return _Ref(scope, name)
        if name in self.program.globals:
            return _Ref(self.program.globals, name)
        raise NameError(name)

    # -- functions --
    def call_user(self, name, args):
        return_type, params, body = self.program.functions[name]
        scope = {}
        for (mode, type_name, param), arg in zip(params, args):
            if mode == "out":
                scope[param] = _zero(type_name, self.program.structs)
            else:
                scope[param] = self.program._coerce(arg.get() if isinstance(arg, _Ref) else arg, type_name)
        inner = _Interpreter(self.program, scope)
        result = None
        try:
            inner.block(_Stream(body))
        except _Return as done:
            result = done.value
        if return_type != "void" and result is not None:
            result = self.program._coerce(result, return_type)
        for (mode, _, param), arg in zip(params, args):
            if mode in ("out", "inout") and isinstance(arg, _Ref):
                arg.set(scope[param])
        return result

    # -- statements --
    def block(self, s):
        s.take("{")
        self.scopes.append({})
        try:
            while not s.accept("}"):
                self.statement(s)
        finally:
            self.scopes.pop()

    def _skip_statement(self, s):
        if s.peek() == "{":
            Program._skip_block(s)
            return
        if s.peek() == "if":
            s.take()
            self._skip_parens(s)
            self._skip_statement(s)
            if s.accept("else"):
                self._skip_statement(s)
            return
        if s.peek() == "for":
            s.take()
            self._skip_parens(s)
            self._skip_statement(s)
            return
        depth = 0
        while True:
            token = s.take()
            depth += token in "(["
            depth -= token in ")]"
            if token == ";" and depth == 0:
                return

    @staticmethod
    def _skip_parens(s):
        s.take("(")
        depth = 1
        while depth:
            token = s.take()
            depth += token == "("
            depth -= token == ")"

    def statement(self, s):
        token = s.peek()
        if token == "{":
            self.block(s)
            return
        if token == ";":
            s.take()
            return
        if token == "if":
            s.take()
            s.take("(")
            condition = self.expression(s)
            s.take(")")
            if condition:
                self.statement(s)
                if s.accept("else"):
                    self._skip_statement(s)
            else:
                self._skip_statement(s)
                if s.accept("else"):
                    self.statement(s)
            return
        if token == "for":
            self._for(s)
            return
        if token == "return":
            s.take()
            value = None if s.peek() == ";" else self.expression(s)
            s.take(";")
            raise _Return(value)
        if token == "continue":
            s.take()
            s.take(";")
            raise _Continue()
        if token == "break":
            s.take()
            s.take(";")
            raise _Break()
        if self._declaration(s):
            return
        self.expression(s)
        s.take(";")

    def _declaration(self, s):
        start = s.at
        while s.peek() in _QUALIFIERS:
            s.take()
        if not (self.program._is_type(s.peek()) and re.fullmatch(r"[A-Za-z_]\w*", s.peek(1) or "")):
            s.at = start
            return False
        type_name = s.take()
        while True:
            name = s.take()
            array = None
            if s.accept("["):
                array = int(s.take())
                s.take("]")
            value = _zero(type_name, self.program.structs, array)
            self.scopes[-1][name] = value
            if s.accept("="):
                self.scopes[-1][name] = self.program._coerce(self.assignment(s), type_name)
            if not s.accept(","):
                break
        s.take(";")
        return True

    def _for(self, s):
        s.take("for")
        s.take("(")
        self.scopes.append({})
        try:
            if not s.accept(";"):
                if not self._declaration(s):
                    self.expression(s)
                    s.take(";")
            condition_at = s.at
            depth = 0
            while not (depth == 0 and s.peek() == ";"):
                token = s.take()
                depth += token == "("
                depth -= token == ")"
            s.take(";")
            step_at = s.at
            self._skip_until_close(s)
            body_at = s.at
            self._skip_statement(s)
            end_at = s.at
            for _ in range(1 << 16):
                s.at = condition_at
                if s.peek() != ";" and not self.expression(s):
                    break
                s.at = body_at
                try:
                    self.statement(s)
                except _Break:
                    break
                except _Continue:
                    pass
                s.at = step_at
                if s.peek() != ")":
                    self.expression(s)
            else:
                raise RuntimeError("loop did not end")
            s.at = end_at
        finally:
            self.scopes.pop()

    @staticmethod
    def _skip_until_close(s):
        depth = 1
        while True:
            token = s.take()
            depth += token == "("
            depth -= token == ")"
            if depth == 0:
                return

    # -- expressions --
    def expression(self, s):
        return self.assignment(s)

    def assignment(self, s):
        target = self.ternary(s, want_ref=True)
        op = s.peek()
        if op in ("=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=") and isinstance(target, _Ref):
            s.take()
            value = self.assignment(s)
            if op != "=":
                value = self.binary(op[:-1], target.get(), value)
            target.set(value)
            return target.get()
        if isinstance(target, _Ref):
            return target.get()
        return target

    def ternary(self, s, want_ref=False):
        condition = self.binary_level(s, 0, want_ref)
        if s.peek() != "?":
            return condition
        if isinstance(condition, _Ref):
            condition = condition.get()
        s.take("?")
        # Both operands are evaluated (the select the hardware does for
        # these texts), only the chosen one is returned.
        first = self._value(self.assignment(s))
        s.take(":")
        second = self._value(self.assignment(s))
        return first if condition else second

    @staticmethod
    def _value(value):
        return value.get() if isinstance(value, _Ref) else value

    def binary_level(self, s, level, want_ref=False):
        if level == len(_BINARY):
            return self.unary(s, want_ref)
        left = self.binary_level(s, level + 1, want_ref)
        while s.peek() in _BINARY[level]:
            op = s.take()
            left = self._value(left)
            if op in ("&&", "||"):
                # Both sides are evaluated (the texts have no side effects there).
                right = self._value(self.binary_level(s, level + 1))
                left = (bool(left) and bool(right)) if op == "&&" else (bool(left) or bool(right))
                continue
            right = self._value(self.binary_level(s, level + 1))
            left = self.binary(op, left, right)
        return left

    def unary(self, s, want_ref=False):
        token = s.peek()
        if token in ("-", "!", "+", "~"):
            s.take()
            value = self._value(self.unary(s))
            if token == "-":
                return self._map(lambda a: f32(-a) if isinstance(a, float) else _convert(-a, _kind_of(value)), value)
            if token == "!":
                return not value
            return value
        if token in ("++", "--"):
            s.take()
            ref = self.unary(s, want_ref=True)
            ref.set(self.binary(token[0], ref.get(), 1))
            return ref.get()
        return self.postfix(s, want_ref)

    def postfix(self, s, want_ref=False):
        value = self.primary(s)
        while True:
            token = s.peek()
            if token == ".":
                s.take()
                name = s.take()
                if isinstance(value, _Ref) and isinstance(value.get(), dict):
                    value = _Ref(value.get(), name)
                elif isinstance(self._value(value), dict):
                    value = self._value(value)[name]
                else:
                    swizzle = [_SWIZZLE[ch] for ch in name]
                    if isinstance(value, _Ref) and value.swizzle is None:
                        value = _Ref(value.container, value.key, swizzle)
                    else:
                        vector = self._value(value)
                        value = vector.c[swizzle[0]] if len(swizzle) == 1 else Vec(
                            [vector.c[i] for i in swizzle], vector.kind)
            elif token == "[":
                s.take()
                index = int(self._value(self.expression(s)))
                s.take("]")
                container = self._value(value)
                if isinstance(container, Vec):
                    if isinstance(value, _Ref) and value.swizzle is None:
                        value = _Ref(value.container, value.key, [index])
                    else:
                        value = container.c[index]
                elif isinstance(value, _Ref):
                    value = _Ref(container, index)
                else:
                    value = container[index]
            elif token in ("++", "--"):
                s.take()
                old = value.get()
                value.set(self.binary(token[0], old, 1))
                value = old
            else:
                break
        if not want_ref:
            return self._value(value)
        return value

    def primary(self, s):
        token = s.take()
        if token == "(":
            value = self.expression(s)
            s.take(")")
            return value
        if re.fullmatch(r"0[xX][0-9a-fA-F]+[uU]?", token):
            return _convert(int(token.rstrip("uU"), 16), "u" if token[-1] in "uU" else "i")
        if re.fullmatch(r"(\d+\.\d*|\.\d+|\d+)([eE][+-]?\d+)?[uUfF]?", token):
            if token[-1] in "uU":
                return _convert(int(token[:-1]), "u")
            if "." in token or "e" in token or "E" in token or token[-1] in "fF":
                return f32(float(token.rstrip("fF")))
            return _convert(int(token), "i")
        if token in ("true", "false"):
            return token == "true"
        if s.peek() == "(":
            args = self._arguments(s, token)
            return self.call(token, args)
        return self.lookup(token)

    def _arguments(self, s, name):
        s.take("(")
        args = []
        user = name in self.program.functions
        outs = [mode != "in" for mode, _, _ in self.program.functions[name][1]] if user else []
        while not s.accept(")"):
            index = len(args)
            if user and index < len(outs) and outs[index]:
                args.append(self.ternary(s, want_ref=True))
            else:
                args.append(self._value(self.assignment(s)))
            s.accept(",")
        return args

    # -- calls --
    def call(self, name, args):
        if name in self.program.functions:
            return self.call_user(name, args)
        if name in _VECTOR_TYPES:
            kind, n = _VECTOR_TYPES[name]
            flat = []
            for arg in args:
                flat.extend(arg.c if isinstance(arg, Vec) else [arg])
            if len(flat) == 1:
                flat = flat * n
            return Vec([_convert(v, kind) for v in flat[:n]], kind)
        if name in _SCALAR_TYPES:
            arg = args[0]
            return _convert(arg.c[0] if isinstance(arg, Vec) else arg, _SCALAR_TYPES[name])
        if name in self.program.structs:
            members = self.program.structs[name]
            return {member: _copy(arg) for (_, member, _), arg in zip(members, args)}
        builtin = getattr(self, "_b_" + name, None)
        if builtin is None:
            raise NameError(f"built-in {name}")
        return builtin(*args)

    # -- arithmetic --
    @staticmethod
    def _map(fn, *values):
        size = max((len(v.c) for v in values if isinstance(v, Vec)), default=0)
        if size == 0:
            return fn(*values)
        kind = next(v.kind for v in values if isinstance(v, Vec))
        lanes = [[v.c[i] if isinstance(v, Vec) else v for v in values] for i in range(size)]
        results = [fn(*lane) for lane in lanes]
        if results and isinstance(results[0], bool):
            kind = "b"
        return Vec(results, kind)

    def binary(self, op, a, b):
        def scalar(x, y):
            if op in ("==", "!=", "<", ">", "<=", ">="):
                if op == "==":
                    return x == y
                if op == "!=":
                    return x != y
                if math.isnan(float(x)) or math.isnan(float(y)):
                    return False
                return {"<": x < y, ">": x > y, "<=": x <= y, ">=": x >= y}[op]
            floating = isinstance(x, float) or isinstance(y, float)
            if floating:
                x, y = float(x), float(y)
                if op == "+":
                    return f32(x + y) if not (math.isinf(x) and math.isinf(y) and x != y) else math.nan
                if op == "-":
                    return f32(x - y) if not (math.isinf(x) and math.isinf(y) and x == y) else math.nan
                if op == "*":
                    if (x == 0.0 and math.isinf(y)) or (y == 0.0 and math.isinf(x)):
                        return math.nan
                    return _mul(x, y)
                if op == "/":
                    return _div(x, y)
                raise ValueError(op)
            kind = "u" if _kind_of(x) == "u" or _kind_of(y) == "u" else "i"
            if op == "+":
                return _convert(x + y, kind)
            if op == "-":
                return _convert(x - y, kind)
            if op == "*":
                return _convert(x * y, kind)
            if op == "/":
                return _convert(int(x / y) if y else 0, kind)
            if op == "%":
                return _convert(x % y if y else 0, kind)
            if op == "&":
                return _convert(x & y, kind)
            if op == "|":
                return _convert(x | y, kind)
            if op == "^":
                return _convert(x ^ y, kind)
            if op == "<<":
                return _convert(x << y, kind)
            if op == ">>":
                return _convert(x >> y, kind)
            raise ValueError(op)

        if op in ("==", "!=") and isinstance(a, Vec) and isinstance(b, Vec):
            same = all(scalar(x, y) if op == "==" else not scalar(x, y) for x, y in zip(a.c, b.c))
            return same if op == "==" else not same
        return self._map(scalar, a, b)

    # -- built-ins --
    def _b_dot(self, a, b):
        total = 0.0
        for x, y in zip(a.c, b.c):
            total = self.binary("+", total, self.binary("*", x, y))
        return total

    def _b_min(self, a, b):
        if _kind_of(a) in "iu" and _kind_of(b) in "iu":
            return self._map(min, a, b)
        return self._map(lambda x, y: _fmin(float(x), float(y)), a, b)

    def _b_max(self, a, b):
        if _kind_of(a) in "iu" and _kind_of(b) in "iu":
            return self._map(max, a, b)
        return self._map(lambda x, y: _fmax(float(x), float(y)), a, b)

    def _b_clamp(self, x, low, high):
        return self._b_min(self._b_max(x, low), high)

    def _b_fma(self, a, b, c):
        return self._map(lambda x, y, z: _fma(float(x), float(y), float(z)), a, b, c)

    def _b_abs(self, x):
        return self._map(abs, x)

    def _b_floor(self, x):
        return self._map(lambda v: v if math.isnan(v) or math.isinf(v) else float(math.floor(v)), x)

    def _b_pow(self, x, y):
        return self._map(lambda a, b: _exp2(self.binary("*", b, _log2(a))), x, y)

    def _b_inversesqrt(self, x):
        return self._map(_rsq, x)

    def _b_sqrt(self, x):
        return self._map(_sqrt, x)

    def _b_length(self, x):
        return _sqrt(self._b_dot(x, x)) if isinstance(x, Vec) else abs(x)

    def _b_normalize(self, x):
        return self.binary("*", x, _rsq(self._b_dot(x, x)))

    def _b_smoothstep(self, edge0, edge1, x):
        def one(e0, e1, v):
            t = _fmin(_fmax(_div(f32(v - e0), f32(e1 - e0)), 0.0), 1.0)
            return f32(f32(t * t) * f32(3.0 - f32(2.0 * t)))
        return self._map(one, edge0, edge1, x)

    def _b_mix(self, x, y, a):
        if isinstance(a, bool) or (isinstance(a, Vec) and a.kind == "b"):
            return self._map(lambda p, q, t: q if t else p, x, y, a)
        return self.binary("+", x, self.binary("*", self.binary("-", y, x), a))

    def _b_floatBitsToUint(self, x):
        return self._map(lambda v: struct.unpack("<I", struct.pack("<f", v))[0], x) if isinstance(x, Vec) else \
            struct.unpack("<I", struct.pack("<f", x))[0]

    def _b_uintBitsToFloat(self, x):
        def one(v):
            return struct.unpack("<f", struct.pack("<I", v & 0xFFFFFFFF))[0]
        return Vec([one(v) for v in x.c], "f") if isinstance(x, Vec) else one(x)

    def _b_any(self, x):
        return any(x.c)

    def _b_all(self, x):
        return all(x.c)

    def _b_lessThan(self, a, b):
        return self._map(lambda x, y: self.binary("<", x, y), a, b)

    def _b_greaterThan(self, a, b):
        return self._map(lambda x, y: self.binary(">", x, y), a, b)

    def _b_lessThanEqual(self, a, b):
        return self._map(lambda x, y: self.binary("<=", x, y), a, b)

    def _b_greaterThanEqual(self, a, b):
        return self._map(lambda x, y: self.binary(">=", x, y), a, b)

    def _b_equal(self, a, b):
        return self._map(lambda x, y: x == y, a, b)

    def _b_notEqual(self, a, b):
        return self._map(lambda x, y: x != y, a, b)


def is_finite(value):
    if isinstance(value, Vec):
        return all(is_finite(v) for v in value.c)
    if isinstance(value, float):
        return math.isfinite(value)
    return True
