"""The "python" datamodel (AGENTS.md, "The "python" datamodel"): the
session's scope, the rewrite, _event, capture and In()."""

import signal
import unittest

import tinyactors
from support import APP, Logs, find, fixture, host_with, load, text


def run(document, events=(), params=None, processors=None):
    """Spawn the chart, send it the events, run until idle: its logs."""
    logs = Logs()
    host = host_with([("c.scxml", document)], processors, log=logs)
    session = host.spawn(host.chart("c"), params=params)
    for event in events:
        host.send(session, *event) if isinstance(event, tuple) else host.send(session, event)
    host.run_until_idle()
    host.close()
    return logs


class Scope(unittest.TestCase):
    def test_an_undeclared_location_is_error_execution(self):
        logs = run(fixture("test_datamodel/undeclared-location.python.scxml"), ["go"])
        self.assertEqual(logs.labelled("n"), ["0"])  # the rest of the block did not run

    def test_foreach_and_script_declare_variables(self):
        """A <foreach>'s item and index and the names a <script> assigns
        outside its defs are the chart's variables, None until set (SCXML
        4.6), and hibernate with the rest."""
        logs, blobs = Logs(), {}
        with host_with([("c.scxml", fixture("test_datamodel/declared.python.scxml"))], log=logs,
                       wake=lambda host, session, migration: host.wake(session, blobs.pop(session))) as host:
            session = host.spawn(host.chart("c"))
            host.run_until_idle()
            blobs[session] = host.hibernate(session)
            host.send(session, "report")
            host.run_until_idle()
        self.assertEqual(logs.labelled("before"), ["[None, None]"])
        self.assertEqual(logs.labelled("after"), ["[4, 20, 20, 1]"])
        self.assertEqual(logs.labelled("report"), ["[4, 20, 20, 1]"])

    def test_a_system_variable_is_read_only(self):
        for location in ("_sessionid", "_event", "_name"):
            with self.subTest(location=location):
                document = load("test_datamodel/read-only.python.scxml")
                find(document, ".//assign").set("location", location)
                logs = run(text(document), ["go"])
                self.assertEqual(logs.labelled("e"), ["refused"])

    def test_comprehensions_see_session_variables(self):
        logs = run(fixture("test_datamodel/comprehensions.python.scxml"))
        self.assertEqual(logs.lines, [("l", "[10, 20]"), ("g", "23"), ("f", "10-2")])

    def test_refused_constructs_are_error_execution(self):
        for expression in ("lambda: 1", "__import__('os')", "open('x')", "k.__class__", "(y := 1)"):
            with self.subTest(expression=expression):
                document = load("test_datamodel/refused-construct.python.scxml")
                find(document, ".//log[@label='v']").set("expr", expression)
                logs = run(text(document))
                self.assertEqual(logs.labelled("e"), ["refused"])
                self.assertEqual(logs.labelled("v"), [])

    def test_script_defs_are_methods(self):
        logs = run(fixture("test_datamodel/script-defs.python.scxml"))
        self.assertEqual(logs.labelled("r"), ["9"])

    def test_params_override_data(self):
        logs = run(fixture("test_datamodel/params.python.scxml"), params={"n": 7})
        self.assertEqual(logs.labelled("n"), ["7"])

    def test_content_that_is_no_expression_is_text(self):
        logs = run(fixture("test_datamodel/content-text.python.scxml"))
        self.assertEqual(logs.lines, [("t", "hello world"), ("l", "[1, 2]")])

    def test_system_variables(self):
        logs = run(fixture("test_datamodel/system-variables.python.scxml"))
        self.assertEqual(logs.labelled("n"), ["c"])
        self.assertEqual(logs.labelled("s"), ["1"])
        self.assertIn("scxml", logs.labelled("p")[0])


class Events(unittest.TestCase):
    def test_event_fields(self):
        logs = run(fixture("test_datamodel/event-fields.python.scxml"), [("go", {"k": 1})])
        self.assertEqual(logs.labelled("e"), ["('go', 'external', None, {'k': 1})"])

    def test_event_is_immutable(self):
        logs = run(fixture("test_datamodel/event-immutable.python.scxml"), ["go"])
        self.assertEqual(logs.labelled("e"), ["refused"])

    def test_in_and_foreach(self):
        logs = run(fixture("test_datamodel/in-foreach.python.scxml"))
        self.assertEqual(logs.lines, [("in", "(True, False)"), ("t", "8")])

    def test_captured_data_is_a_copy(self):
        got = []

        def processor(host, send):
            got.append(send.data)

        run(fixture("test_datamodel/captured-data.python.scxml"), processors={APP: processor})
        self.assertEqual(got, [{"v": [1, 2, 3]}])  # the session's later change is not seen

    def test_a_value_outside_the_table_does_not_cross(self):
        logs = run(fixture("test_datamodel/outside-table.python.scxml"), processors={APP: lambda host, send: None})
        self.assertEqual(logs.labelled("e"), ["refused"])


class Interrupts(unittest.TestCase):
    def test_a_base_exception_is_error_execution_and_raised_after_the_run(self):
        document = fixture("test_datamodel/endless-script.python.scxml")
        logs = Logs()
        host = host_with([("c.scxml", document)], log=logs)
        session = host.spawn(host.chart("c"))
        host.send(session, "go")

        class Stop(KeyboardInterrupt):
            pass

        def interrupt(number, frame):
            raise Stop()

        previous = signal.signal(signal.SIGALRM, interrupt)
        try:
            signal.setitimer(signal.ITIMER_REAL, 0.05)
            with self.assertRaises(Stop):
                host.run_until_idle()
        finally:
            signal.setitimer(signal.ITIMER_REAL, 0)
            signal.signal(signal.SIGALRM, previous)
        host.run_until_idle()
        self.assertEqual(logs.labelled("e"), ["execution"])
        host.close()


if __name__ == "__main__":
    unittest.main()
