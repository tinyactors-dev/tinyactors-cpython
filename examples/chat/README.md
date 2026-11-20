# A chat server

Workspaces, rooms and connections as actors: an owner creates rooms in a
workspace, users join them and post, and every member hears every post.
Direct messages go to a user's open connections, or wait as unread. A
connection closes when its client stops sending heartbeats.

    python main.py [--port 8765] [--database chat.db] [--resident 10000]

Clients speak JSON lines over TCP. The first line says who they are,
`{"user": "ada", "workspace": "acme"}`, and each line after it is an
`[event, data]` pair: `["create", {"name": "general"}]`, `["join",
{"room": "general"}]`, `["post", {"room": "general", "text": "hi"}]`,
`["dm", {"to": "bob", "text": "psst"}]`, `["heartbeat", {}]`. The server
answers in the same form. Two terminals of `nc localhost 8765` are two
clients; a new database has the workspace `acme`, owned by `ada`.
`mise run test` in `tinyactors-cpython` runs `test_chat.py`.

## The pieces

| File | What it is |
|---|---|
| `charts/workspace.scxml`, `workspace.py` | a workspace: the collection actor that decides which rooms exist |
| `charts/room.scxml`, `room.py` | a room: its members and recent messages; it saves itself and announces itself to its workspace when it starts |
| `charts/connection.scxml`, `connection.py` | one actor per open socket: what the client sends goes to the actor it is for, what actors answer goes to the client |
| `processors.py` | the I/O processors, one per external system: `chat:gateway` (the sockets), `chat:db` (SQLite), `chat:inbox` (direct messages) |
| `main.py` | the server: compile the charts once, make the host, spawn what the database holds, serve the sockets |
| `test_chat.py` | the tests: each behaviour end to end, with recording sockets, and the server over TCP |

The logic is in the charts. A chart's `.py` helper (`<script src>`)
holds the functions its expressions call, each taking the session's
values as arguments, so the chart reads as what happens and the helper
as how. The processors do I/O and nothing else: they decide nothing
about rooms or members. The binding's `Spawner`, configured under the
type `spawn`, starts a room when its workspace sends it one. A name has
one owner: the workspace is the only actor that spawns its rooms' names,
and it keeps the spawns it asked for in `creating` (keyed by the spawn's
sendid), so it never asks twice for a name.

The host keeps at most `--resident` actors in memory; the others are
blobs in an SQLite store (`store=`, `resident=`), woken when an event
comes for them. The store is a cache of this host's actors, so it
starts empty with the process; the database (`chat.db`) is what
survives a restart, and `main.start` spawns the workspaces and rooms
from it.

## A send to an actor that is not there

The README's "Sending to an actor that is not there" and
[docs/guide/missing-actors.md](../../docs/guide/missing-actors.md) describe
three ways, chosen per kind of actor. This application uses each:

- **Workspaces and rooms never end, and hibernate** (way A). Their
  charts have no `<final>`, and the actors are named
  (`workspace.acme`, `room.acme.general`). An event for one that is
  hibernated wakes it from the store: `test_a_hibernated_workspace_wakes_when_an_event_comes_for_it`.
- **Direct messages are routed by the application** (way B). A user is
  not an actor; their connections are, and there may be none. The
  connection's chart sends `dm` to `chat:inbox`, which forwards it to
  each of the recipient's connections, or keeps it as unread:
  `Inbox` in `processors.py`, `test_a_direct_message_is_routed_by_the_inbox`.
- **A join to a room that does not exist is the sender's error** (way
  C). The connection sends `join` with the id `join`, and handles
  `error.communication` for it by telling its client `no such room`:
  `connection.scxml`, `test_a_join_to_a_room_that_is_not_there_is_the_senders_error`.

## Names

Actor names use only characters that need no percent-encoding in an
address (letters, digits, `-`, `.`, `_`, `~`), with `.` between their
parts: `workspace.<id>`, `room.<workspace>.<name>`. A chart writes the
address as plain text (`room_address` in `connection.py`), and the
workspace refuses a room name with any character outside `a`-`z`,
`0`-`9` and `-` (`valid_name` in `workspace.py`).

A room keys its members by the fragment of `_event.origin`
(`sender` in `room.py`), so one connection is one member whichever form
the origin has
([Keying a table by the sender](../../docs/guide/missing-actors.md#keying-a-table-by-the-sender)).

## Asking an actor

`workspace` answers `rooms` and `room` answers `members` and `kick` to
`_event.origin`, so the application asks them with `await
actor.ask(event, data)`:

```python
room = host.actor("room.acme.general")
event, data = await asyncio.wait_for(room.ask("kick", {"user": "mallory"}), 5)
# ("kicked", {"members": ["ada", "bob"]})
```
