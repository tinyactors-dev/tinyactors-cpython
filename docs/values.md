# Python values

The new C extension exposes `encode(value)` and `decode(data)`, using
core's streaming CBOR writer and reader. The codec supports exact
built-in `int` (from -2^64 to 2^64-1), `float`, `str`, `bytes`, `list`,
and `dict` with `str` keys, plus `True`, `False`, `None`, `UNDEFINED`, and
`Address`. Lists and maps preserve order. Wire nesting is checked by core
(cbor.h, TA_CBOR_DEPTH_MAX). Shared subtrees
are allowed; cycles are refused by the nesting limit.

Encoding refuses unsupported Python types with `TypeError`, integers
outside the wire range with `OverflowError`, and lone surrogates with
`UnicodeError`. Decoding accepts exact built-in `bytes` containing exactly
one CBOR item; other input types raise `TypeError` in this bootstrap.
Malformed input and unsupported tags or simple values
raise `ValueError`. Repeated map keys keep the last value, as a Python
dictionary does. The codec accepts neutral values, not arbitrary CBOR tags.

## Actor references

Use `tinyactors.Address(text)` when the value must keep an actor reference
across a process boundary. The constructor takes one positional exact
`str`, validates its UTF-8 representation and core's text-length limit,
and retains it. It does not parse a URI, find an actor or validate a route.
An ordinary string is still encoded as ordinary text.

```python
from tinyactors import Address, decode, encode

player = Address("tinyactors://tab-7f3/#_scxml_4")
restored = decode(encode({"player": player}))["player"]
assert restored == player
assert restored.text == str(player)
```

An `Address` is immutable and cannot be subclassed. Its read-only `.text`
and `str(value)` return the original text. Its representation is
`Address('...')`; equality and hashing use the text among `Address` values
of the same exact type. An address is unequal to a plain string;
ordering is unsupported. It can be a Python dictionary/set key, but the
codec's maps require ordinary `str` keys. Tag 32 is decoded only when its
immediate child is text; nested tags and non-text children are refused.

## Undefined values

Use `tinyactors.UNDEFINED` to preserve CBOR undefined where `None` means
CBOR null. Decoding returns that module's singleton. Compare it with
`is`; use the exported object directly rather than constructing its type.
It is preserved in lists and maps, without dropping entries.

```python
from tinyactors import UNDEFINED, decode, encode

assert decode(encode(UNDEFINED)) is UNDEFINED
assert decode(encode([UNDEFINED, None]))[0] is UNDEFINED
```

The singleton's representation is `UNDEFINED`. Its type cannot be
constructed or subclassed. `Address` and `UNDEFINED` exports are created
separately for each module instance, including across interpreters.
Values from independently created module instances are not interchangeable.
Rebinding a module export does not alter
the identities kept by that module's codec state.

## Ownership and use

| Object | Owner | Lifetime |
|---|---|---|
| Address/undefined types and singleton | Module state | Created during import; visited and cleared by module GC |
| Address text | Address instance | Retained at construction or decode; released with the instance |
| Writer/reader context | One encode/decode call | Borrows module state and input during the call; discarded before return |
| Encoded bytes or decoded values | Python caller | Independent of the input message and temporary writer |

Each list and dictionary is snapshotted separately during encoding. Do not
mutate a nested value during conversion if you need a consistent whole
value. In-process actor inputs use structs and payload handles; this codec
belongs to config, control messages and actual process boundaries.

The tests consume the shared suite's `values.txt`: its Python expressions,
applicable roundtrips and malformed cases. Reference and undefined identity
tests additionally check that a paired conversion cannot hide a wrong type
mapping. Both the abi3 interpreter and free-threaded build run these tests.

Use the normally imported module. If a custom import loader exposes its
methods before executing the module, `encode` and `decode` raise
`RuntimeError` until codec initialization is complete.

## Values between the application and actors

The same table holds for what crosses between the application and a
host, without CBOR inside the process. Data given to `Host.send`,
`Host.spawn`'s params and `send.answer` becomes a payload of the target
session's datamodel: for a `"python"` chart, a copy (lists and dicts
copied, the immutable values shared, anything else refused with
`TypeError`); for an ECMAScript chart, its CBOR, read by quickjs. What a
chart sends the application (`send.data`) is the `"python"` payload's
value itself, or quickjs's payload decoded on first read. A `"python"`
chart's `<send>` data, `<donedata>` and invoke params are copied the same
way when they are captured, so actors share no mutable value; a value
outside the table (a tuple, a set, an object) fails the capture with
`error.execution`. Toward another system the value is the remote I/O
processor's to encode; `tinyactors.encode` gives the value table's CBOR.
