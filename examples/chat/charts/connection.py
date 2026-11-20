# The connection's helpers. Actor names use only characters that need no
# percent-encoding in an address (letters, digits, "-", ".", "_", "~"),
# so the address is the name as written.

def room_address(workspace, room):
    return "tinyactors://chat/@room." + workspace + "." + room


def workspace_address(workspace):
    return "tinyactors://chat/@workspace." + workspace


def added(items, item):
    return items if item in items else items + [item]


def removed(items, item):
    return [i for i in items if i != item]


def frame(event, data):
    return {"event": event, "data": data}
