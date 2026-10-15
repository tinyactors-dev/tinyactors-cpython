"""Every public item of the module explains itself: the module, each
function and class, and each public method and field of a class has a
docstring that says when and how to use it."""

import unittest

import tinyactors

# A field's one line can be short; a module, function, class or method says more.
SHORT, LONG = 20, 60


def public(names):
    return [n for n in names if not n.startswith("_") or n in ("__enter__", "__exit__", "__aenter__", "__aexit__")]


class Docs(unittest.TestCase):
    def test_the_module_explains_its_structure(self):
        doc = tinyactors.__doc__
        for word in ("Host", "segment", "image", "actor", "chart", "compile_charts", "handler"):
            self.assertIn(word, doc)

    def test_every_public_item_has_a_docstring(self):
        missing = []
        for name in public(dir(tinyactors)):
            item = getattr(tinyactors, name)
            if isinstance(item, (int, str)):
                continue
            doc = (item.__doc__ or "").strip()
            if len(doc) < LONG:
                missing.append(name)
            if not isinstance(item, type):
                continue
            for attribute, member in vars(item).items():
                if attribute not in public([attribute]) or attribute in ("__init__", "__module__", "__dict__",
                                                                          "__weakref__", "__slots__", "__doc__"):
                    continue
                text = (getattr(member, "__doc__", None) or "").strip()
                slots = getattr(item, "__slots__", None)
                if not text and isinstance(slots, dict):  # a Python class's fields: help() reads __slots__
                    text = slots.get(attribute, "")
                callable_member = callable(member) or isinstance(member, classmethod)
                if len(text) < (LONG if callable_member and not attribute.startswith("__") else SHORT):
                    missing.append("%s.%s" % (name, attribute))
        self.assertEqual(missing, [])


if __name__ == "__main__":
    unittest.main()
