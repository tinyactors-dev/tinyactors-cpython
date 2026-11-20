# The workspace's helpers: plain functions of the values the chart passes
# them, so the chart reads as what happens and this file as how.

def valid_name(name):
    # A room's name is part of its actor's name, and so of its address:
    # these characters need no percent-encoding there.
    return isinstance(name, str) and 0 < len(name) <= 64 and all(c in "abcdefghijklmnopqrstuvwxyz0123456789-" for c in name)


def pending(creating, name):
    return any(entry["name"] == name for entry in creating.values())


def may_create(owners, rooms, creating, event):
    name = event.data["name"]
    return event.data["user"] in owners and valid_name(name) and name not in rooms and not pending(creating, name)


def refusal(owners, rooms, creating, event):
    name = event.data["name"]
    if event.data["user"] not in owners:
        return {"name": name, "reason": "not an owner"}
    if not valid_name(name):
        return {"name": name, "reason": "invalid name"}
    return {"name": name, "reason": "exists"}


def room_spawn(workspace, event):
    name = event.data["name"]
    return {"chart": "room", "name": "room." + workspace + "." + name,
            "params": {"workspace": workspace, "name": name}}


def request(event):
    return {"name": event.data["name"], "origin": event.origin}


def added(items, item):
    return items if item in items else items + [item]


def with_entry(table, key, value):
    return {**table, key: value}


def without_entry(table, key):
    return {k: v for k, v in table.items() if k != key}
