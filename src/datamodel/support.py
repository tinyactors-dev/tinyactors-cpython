# The Python half of the extension: the exceptions, a diagnostic record,
# and the "python" datamodel's restrictive rewrite and chart classes.
# It is not a module of the wheel: expressions.c embeds this text (the
# build writes it into build/<interpreter>/support.inc) and runs it once
# per interpreter when the module is executed; C reads the names below
# from its namespace. The rewrite runs once per chart, at chart_attach,
# and once per `.py` file a resolver reads; nothing here runs per event.
# AGENTS.md, "The "python" datamodel", holds the allow-list.
import ast
import copy


class TinyactorsError(Exception):
    """An error the core reported, the one exception type the host raises
    for its own reasons.

    It comes from a call the host refused (a full pool, a closed host, a
    session that does not exist), from a thin view read after its batch was
    acknowledged, from a fault of a run, or as a typed method's error
    (given to on_error, or raised by the driver without one). Branch on
    `status`, the core's name for a bare status ("TA_ERROR_STALE",
    "TA_ERROR_STATE", ...); a typed method's error also has `code`,
    `reason`, `path` and `message` as the host's reply wrote them.
    MemoryError is raised instead when the host ran out of a pool its
    config sized."""

    def __init__(self, message="", code=None, reason=None, path=None, status=None):
        super().__init__(message)
        self.message = message
        self.code = code
        self.reason = reason
        self.path = path
        self.status = status


class CompileError(TinyactorsError):
    """compile_charts or compile_system found an error in the documents.

    `diagnostics` is every Diagnostic of the compilation (errors, warnings
    and notes, each with its file, line and column), so an editor
    integration, a build step or a test can show them all at once; str()
    of the error names the first. Nothing was compiled: fix the documents
    and compile again. Catch it where charts are compiled (at startup, in
    a build or deploy step), not around the host, which never raises
    it."""

    def __init__(self, message, diagnostics):
        super().__init__(message, status="TA_ERROR_INVALID")
        self.diagnostics = diagnostics


class ResolveError(TinyactorsError):
    """Raise it from a resolver, resolver(base, source), to refuse a
    document a chart references with src=: not found, not allowed (a path
    outside the directory your charts live in), not valid.

    A resolver is how charts that are split over files get compiled: the
    compiler calls it for each static src= it does not have among the
    sources. Raising ResolveError makes the refusal a diagnostic like any
    other, at `line` and `column` when given (a resolver that checks a
    file's content knows where it is wrong), so compile_charts or
    compile_system raises CompileError with it among the others. Any other
    exception a resolver raises is a bug in the resolver and propagates
    from the compile call."""

    def __init__(self, message, line=None, column=None):
        super().__init__(message, status="TA_ERROR_INVALID")
        self.line = line
        self.column = column


class Diagnostic:
    """One message of the compiler about a document: in
    Compiled.diagnostics (warnings and notes of a compilation that
    succeeded) and CompileError.diagnostics. Show them where a person
    edits charts (a terminal, an editor, a CI log) or assert on them in a
    test.

    `severity` is "error", "warning" or "note", `code` the compiler's
    code for the kind of problem, `file`, `line` and `column` where it is
    (the source's name as given to compile_charts), `message` the text.
    repr() reads like a compiler's output line."""

    __slots__ = {
        "severity": "\"error\", \"warning\" or \"note\".",
        "code": "The compiler's code for the kind of problem: match on it rather than on the message.",
        "file": "The document's name, as given to compile_charts or returned by a resolver.",
        "line": "The line in that document, from 1.",
        "column": "The column in that line, from 1.",
        "message": "What the compiler says, in one sentence.",
    }

    def __init__(self, severity, code, file, line, column, message):
        self.severity = severity
        self.code = code
        self.file = file
        self.line = line
        self.column = column
        self.message = message

    def __repr__(self):
        return "%s:%d:%d: %s: %s" % (self.file, self.line, self.column, self.severity, self.message)


for _type in (TinyactorsError, CompileError, ResolveError, Diagnostic):
    _type.__module__ = "tinyactors"

# ---- the rewrite ---------------------------------------------------------

BUILTINS = ("len", "min", "max", "sum", "abs", "round", "sorted", "reversed", "enumerate", "zip",
            "range", "any", "all", "isinstance", "hasattr", "str", "int", "float", "bool", "list", "dict",
            "tuple", "set", "frozenset", "map", "filter")
# The session's read-only system variables, properties of its class.
SYSTEM = ("_sessionid", "_name", "_ioprocessors")
# The hidden parameter every compiled function takes: the session.
SESSION = "_ta_s"
VALUE = "_ta_v"

_EXPRESSIONS = (ast.Expression, ast.Constant, ast.BinOp, ast.UnaryOp, ast.BoolOp, ast.Compare,
                ast.IfExp, ast.Subscript, ast.Slice, ast.ListComp, ast.SetComp, ast.DictComp,
                ast.GeneratorExp, ast.comprehension, ast.JoinedStr, ast.FormattedValue, ast.Call,
                ast.keyword, ast.Attribute, ast.Name, ast.List, ast.Tuple, ast.Dict, ast.Set,
                ast.Starred, ast.Load, ast.Store, ast.operator, ast.unaryop, ast.boolop, ast.cmpop)
_STATEMENTS = (ast.Module, ast.Assign, ast.AugAssign, ast.If, ast.For, ast.While, ast.Pass,
               ast.Expr, ast.Break, ast.Continue, ast.Return, ast.FunctionDef, ast.arguments,
               ast.arg)
_NAMES = {ast.Import: "import", ast.ImportFrom: "import", ast.Global: "global",
          ast.Nonlocal: "nonlocal", ast.Lambda: "lambda", ast.Await: "await", ast.Yield: "yield",
          ast.YieldFrom: "yield", ast.NamedExpr: "the walrus (:=)", ast.Delete: "del",
          ast.Try: "try", ast.Raise: "raise", ast.With: "with", ast.ClassDef: "class",
          ast.AsyncFunctionDef: "async def", ast.AsyncFor: "async for", ast.AsyncWith: "async with"}


class Rewrite:
    """Checks one source against the allow-list and rewrites the names of
    the session into attributes of the hidden parameter: `tries < 3`
    becomes `_ta_s.tries < 3`. A refusal is a SyntaxError at the node,
    with the document's line (`line` is where the source starts)."""

    def __init__(self, names, file, line):
        self.names = names  # the session's: <data> ids, _event, methods
        self.file = file
        self.line = line
        self.system = set()  # the system variables the source reads
        self.methods = set()  # the chart's methods: names, not locations

    def refuse(self, node, message):
        line = getattr(node, "lineno", 1)
        error = SyntaxError(message, (self.file, line + self.line - 1, getattr(node, "col_offset", 0) + 1, None))
        raise error

    def check(self, node):
        if not isinstance(node, _EXPRESSIONS + _STATEMENTS):
            self.refuse(node, "%s is not allowed in a chart" % _NAMES.get(type(node), type(node).__name__))

    def session(self, node, name, context):
        return ast.copy_location(ast.Attribute(ast.copy_location(ast.Name(SESSION, ast.Load()), node), name, context), node)

    # expressions, in the scope `local` (names bound in a def or comprehension)
    def expression(self, node, local):
        self.check(node)
        if isinstance(node, ast.Name):
            if node.id in local:
                return node
            if node.id.startswith("_ta_"):
                self.refuse(node, "the name %s is reserved" % node.id)
            if node.id in self.names or node.id in SYSTEM:
                if node.id in SYSTEM:
                    self.system.add(node.id)
                return self.session(node, node.id, ast.Load())
            if node.id == "In" or node.id in BUILTINS:
                return node
            self.refuse(node, "the name %s is not declared" % node.id)
        if isinstance(node, ast.Attribute):
            if node.attr.startswith("__") and node.attr.endswith("__") or node.attr.startswith("_ta_"):
                self.refuse(node, "the attribute %s is not allowed" % node.attr)
            node.value = self.expression(node.value, local)
            return node
        if isinstance(node, (ast.ListComp, ast.SetComp, ast.GeneratorExp, ast.DictComp)):
            inner = set(local)
            for generator in node.generators:
                self.check(generator)
                generator.iter = self.expression(generator.iter, inner)
                self.bind(generator.target, inner)
                generator.ifs = [self.expression(test, inner) for test in generator.ifs]
            if isinstance(node, ast.DictComp):
                node.key = self.expression(node.key, inner)
                node.value = self.expression(node.value, inner)
            else:
                node.elt = self.expression(node.elt, inner)
            return node
        for field, value in ast.iter_fields(node):
            if isinstance(value, ast.AST):
                setattr(node, field, self.expression(value, local))
            elif isinstance(value, list):
                setattr(node, field, [self.expression(item, local) if isinstance(item, ast.AST) else item for item in value])
        return node

    # a comprehension's or a for loop's local target in a def
    def bind(self, target, local):
        self.check(target)
        if isinstance(target, ast.Name):
            local.add(target.id)
        elif isinstance(target, (ast.Tuple, ast.List)):
            for element in target.elts:
                self.bind(element, local)
        else:
            self.refuse(target, "a comprehension binds names only")

    # an assignment's target: a <data> id, its items or attributes; in a
    # def, a local name too
    def target(self, node, local, in_def):
        self.check(node)
        if isinstance(node, ast.Name):
            if node.id in local:
                return node
            if node.id in SYSTEM or node.id == "_event":
                self.refuse(node, "the system variable %s is read-only" % node.id)
            if node.id in self.names and not node.id in self.methods:
                return self.session(node, node.id, ast.Store())
            if in_def and not node.id.startswith("_ta_"):
                local.add(node.id)
                return node
            self.refuse(node, "assignment to %s, which is not declared" % node.id)
        if isinstance(node, (ast.Attribute, ast.Subscript)):
            if isinstance(node, ast.Attribute) and (node.attr.startswith("__") and node.attr.endswith("__") or node.attr.startswith("_ta_")):
                self.refuse(node, "the attribute %s is not allowed" % node.attr)
            node.value = self.expression(node.value, local)
            if isinstance(node, ast.Subscript):
                node.slice = self.expression(node.slice, local)
            node.ctx = ast.Store()
            return node
        if isinstance(node, (ast.Tuple, ast.List)):
            node.elts = [self.target(element, local, in_def) for element in node.elts]
            node.ctx = ast.Store()
            return node
        self.refuse(node, "this is not a location")

    def statements(self, body, local, in_def):
        return [self.statement(node, local, in_def) for node in body]

    def statement(self, node, local, in_def):
        self.check(node)
        if isinstance(node, ast.Assign):
            node.value = self.expression(node.value, local)
            node.targets = [self.target(target, local, in_def) for target in node.targets]
        elif isinstance(node, ast.AugAssign):
            node.value = self.expression(node.value, local)
            node.target = self.target(node.target, local, in_def)
        elif isinstance(node, (ast.If, ast.While)):
            node.test = self.expression(node.test, local)
            node.body = self.statements(node.body, local, in_def)
            node.orelse = self.statements(node.orelse, local, in_def)
        elif isinstance(node, ast.For):
            node.iter = self.expression(node.iter, local)
            node.target = self.target(node.target, local, in_def)
            node.body = self.statements(node.body, local, in_def)
            node.orelse = self.statements(node.orelse, local, in_def)
        elif isinstance(node, ast.Expr):
            node.value = self.expression(node.value, local)
        elif isinstance(node, ast.Return):
            if not in_def:
                self.refuse(node, "return outside a def")
            if node.value is not None:
                node.value = self.expression(node.value, local)
        elif isinstance(node, ast.FunctionDef):
            self.refuse(node, "def is allowed only at the top level of a script")
        return node

    # a top-level def of a script: a method of the chart's class, its
    # first parameter the session
    def method(self, node):
        if node.decorator_list or node.returns:
            self.refuse(node, "a def has no decorators or annotations")
        arguments = node.args
        if arguments.posonlyargs or arguments.vararg or arguments.kwonlyargs or arguments.kwarg:
            self.refuse(node, "a def takes plain parameters")
        local = set()
        for argument in arguments.args:
            if argument.annotation is not None:
                self.refuse(argument, "a def has no decorators or annotations")
            local.add(argument.arg)
        arguments.defaults = [self.expression(value, set()) for value in arguments.defaults]
        arguments.args.insert(0, ast.copy_location(ast.arg(SESSION), node))
        node.body = self.statements(node.body, local, True)
        return node


def _parse(source, mode, file, line):
    try:
        tree = ast.parse(source.strip() if mode == "eval" else source, file, mode)
    except SyntaxError as error:
        raise SyntaxError(error.msg, (file, (error.lineno or 1) + line - 1, error.offset or 0, None)) from None
    return tree


def methods_of(source, file, line):
    """The top-level defs of a script, by name (checked when attached)."""
    return [node.name for node in _parse(source, "exec", file, line).body if isinstance(node, ast.FunctionDef)]


def check_file(source, file, names):
    """A `.py` file a resolver read: every construct against the
    allow-list; names are checked when its chart is attached."""
    tree = _parse(source, "exec", file, 1)
    rewrite = Rewrite(_Anything(), file, 1)
    for node in tree.body:
        if isinstance(node, ast.FunctionDef):
            rewrite.method(node)
        else:
            rewrite.statement(node, set(), False)


class _Anything:
    def __contains__(self, name):
        return True


def _function(body, parameters, namespace, file):
    module = ast.parse("def _ta_f(%s):\n    pass\n" % ", ".join(parameters))
    function = module.body[0]
    function.body = body or [ast.Pass()]
    ast.fix_missing_locations(module)
    code = compile(module, file, "exec")
    local = {}
    exec(code, namespace, local)
    return local["_ta_f"]


CONDITION, VALUE_KIND, LOCATION, SCRIPT, STRING, CONTENT, ITEM = 1, 2, 3, 4, 5, 6, 7


def _names_bound(target, names):
    if isinstance(target, ast.Name):
        names.add(target.id)
    elif isinstance(target, (ast.Tuple, ast.List)):
        for element in target.elts:
            _names_bound(element, names)


def _declared_by(body, names):
    """The names a script's statements assign or loop over outside its
    defs: the variables it declares, as a module's top level does."""
    for node in body:
        if isinstance(node, ast.Assign):
            for target in node.targets:
                _names_bound(target, names)
        elif isinstance(node, ast.For):
            _names_bound(node.target, names)
        if isinstance(node, (ast.If, ast.While, ast.For)):
            _declared_by(node.body, names)
            _declared_by(node.orelse, names)
    return names


def attach(name, data, expressions, namespace, file):
    """A chart's class and its compiled expressions. `data` holds the
    <data> ids, `expressions` (kind, source, line) per expression id.
    Returns (class, functions, system, variables): functions[e] is a
    function of the session (CONDITION, VALUE, STRING, SCRIPT, CONTENT),
    a pair (getter, setter) for a LOCATION or an ITEM, or the SyntaxError
    that refused it (an evaluation of it is error.execution); system the
    set of system variables the chart reads; variables the session's
    variables: the <data> ids, then the names a <foreach> item or index
    or a <script> outside its defs binds, which the chart declares too
    (SCXML 4.6, and as a module's top level does)."""
    for identifier in data:
        if not identifier.isidentifier() or identifier.startswith("_"):
            raise SyntaxError("the <data> id %r is not a Python name the chart may declare" % identifier)
    methods = {}
    scripts = []
    for index, (kind, source, line) in enumerate(expressions):
        if kind == SCRIPT:
            try:
                tree = _parse(source, "exec", file, line)
            except SyntaxError:
                continue
            ast.increment_lineno(tree, line - 1)
            scripts.append((index, tree))
            for node in tree.body:
                if isinstance(node, ast.FunctionDef):
                    methods[node.name] = node
    declared = set()
    for index, tree in scripts:
        _declared_by(tree.body, declared)
    for kind, source, line in expressions:
        if kind == ITEM and source.strip().isidentifier():
            declared.add(source.strip())
    declared = {identifier for identifier in declared - set(data) - set(methods)
                if not identifier.startswith("_")}
    variables = tuple(data) + tuple(sorted(declared))
    names = set(variables) | {"_event"} | set(methods)
    system = set()
    compiled_methods = {}
    for method_name, node in methods.items():
        if method_name in variables or method_name in SYSTEM or method_name.startswith("_"):
            raise SyntaxError("the def %s has the name of a variable" % method_name)
        rewrite = Rewrite(names, file, 1)
        rewrite.methods = set(methods)
        compiled_methods[method_name] = _function_from_def(rewrite.method(node), namespace, file)
        system |= rewrite.system
    functions = []
    script_trees = dict(scripts)
    for index, (kind, source, line) in enumerate(expressions):
        rewrite = Rewrite(names, file, 1 if kind == SCRIPT else line)
        rewrite.methods = set(methods)
        try:
            if kind == SCRIPT:
                tree = script_trees.get(index) or _parse(source, "exec", file, line)
                body = [rewrite.statement(node, set(), False) for node in tree.body
                        if not isinstance(node, ast.FunctionDef)]
                functions.append(_function(body, [SESSION], namespace, file))
            elif kind in (LOCATION, ITEM):
                tree = _parse(source, "eval", file, line).body
                getter = _expression(_moved(rewrite.expression(copy.deepcopy(tree), set()), line), namespace, file)
                setter = ast.copy_location(ast.Assign([_moved(rewrite.target(tree, set(), False), line)],
                                                      ast.Name(VALUE, ast.Load())), tree)
                functions.append((getter, _function([setter], [SESSION, VALUE], namespace, file)))
            else:
                try:
                    tree = _parse(source, "eval", file, line).body
                    functions.append(_expression(_moved(rewrite.expression(tree, set()), line), namespace, file))
                except SyntaxError:
                    if kind != CONTENT:
                        raise
                    functions.append(_constant(source.strip()))
            system |= rewrite.system
        except Exception as error:  # refused, or not compiled: error.execution when evaluated
            functions.append(error)
    slots = variables + ("_event", "_ta_sys")
    body = {"__slots__": slots, "__module__": "tinyactors.charts"}
    body["_sessionid"] = property(lambda session: session._ta_sys[0])
    body["_name"] = property(lambda session: session._ta_sys[1])
    body["_ioprocessors"] = property(lambda session: session._ta_sys[2])
    body.update(compiled_methods)
    chart = type(name if name.isidentifier() else "Chart", (), body)
    return chart, tuple(functions), frozenset(system), variables


def _moved(tree, line):
    """The tree at the document's line (the source starts at `line`)."""
    return ast.increment_lineno(tree, line - 1)


def _expression(tree, namespace, file):
    body = [ast.copy_location(ast.Return(tree), tree)]
    return _function(body, [SESSION], namespace, file)


def _constant(text):
    return lambda session: text


def _function_from_def(node, namespace, file):
    module = ast.Module([node], [])
    ast.fix_missing_locations(module)
    local = {}
    exec(compile(module, file, "exec"), namespace, local)
    return local[node.name]
