"""What the tests share: charts read from fixture files, varied with
ElementTree, and a host made from them.

Every chart a test compiles is a file under tests/fixtures/<test module>/,
named <chart>.<datamodel>.scxml; a `.py` file a chart reads through `src`
sits beside it. A test that needs the chart as it is reads it with
`fixture`. A test that needs a variation (another name, an attribute
changed, an element added) loads it with `load`, changes the tree with
ElementTree (`find`, `element`, and the element's own methods: `set`,
`append`, `insert`) and compiles `text` of it. Tests never write XML as
string literals."""

import pathlib
import xml.etree.ElementTree as ET

import tinyactors

# The type of the I/O processor host_with configures; the fixtures name it
# in their <send type="urn:test:app">.
APP = "urn:test:app"
SCXML = "http://www.w3.org/2005/07/scxml"
FIXTURES = pathlib.Path(__file__).parent / "fixtures"

# A chart changed and written again keeps SCXML as its default namespace.
ET.register_namespace("", SCXML)


def fixture(path):
    """The text of tests/fixtures/<path>, verbatim."""
    return (FIXTURES / path).read_text(encoding="utf-8")


def load(path, **attributes):
    """The <scxml> element of the fixture chart at `path`, with each of
    `attributes` set on it (`name="b"`, `datamodel="ecmascript"`)."""
    root = ET.fromstring(fixture(path))
    for name, value in attributes.items():
        root.set(name, value)
    return root


def find(root, path):
    """The first element under `root` that the ElementTree path matches, the
    path's names in the SCXML namespace: `find(root, ".//state[@id='s']")`."""
    found = root.find(path, {"": SCXML})
    if found is None:
        raise LookupError("no %s in the chart" % path)
    return found


def element(tag, *children, **attributes):
    """A new SCXML element with these attributes and children:
    `element("transition", element("log", label="l", expr="n"), event="go")`."""
    node = ET.Element("{%s}%s" % (SCXML, tag), attributes)
    node.extend(children)
    return node


def text(root):
    """The chart `root` as a document's text."""
    return ET.tostring(root, encoding="unicode")


def host_with(documents, processors=None, sync=False, **options):
    """A host on a simulated clock with one I/O processor of type APP, the
    documents compiled into one image and added."""
    config = {"systemID": "t", "system": {"ticksPerSecond": 1000},
              "ioProcessors": [{"type": APP, "sync": sync}]}
    options.setdefault("clock", tinyactors.SimulatedClock())
    host = tinyactors.Host(config, processors if processors is not None else {}, **options)
    with tinyactors.compile_charts(documents) as compiled:
        host.add_image(compiled.image)
    return host


class Logs:
    """A log handler that keeps (label, text) of every line."""

    def __init__(self):
        self.lines = []

    def __call__(self, host, entry):
        self.lines.append((entry.label, entry.text))

    def labelled(self, label):
        return [text for each, text in self.lines if each == label]
