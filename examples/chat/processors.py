"""The chat's I/O processors: one object per external system the actors
talk to, each with receive(send) switching on send.event."""

import json
import sqlite3

import tinyactors


class Gateway:
    """chat:gateway: the open sockets, one per connection actor.

    A socket is any object with write(text) and close(reason): main.py's
    wraps a TCP stream, the test's records what it is given."""

    def __init__(self):
        self.sockets = {}                      # connection session -> socket
        self.users = {}                        # user -> {connection session}

    def attach(self, connection, user, socket):
        self.sockets[connection.id] = socket
        self.users.setdefault(user, set()).add(connection.id)

    def detach(self, session):
        self.sockets.pop(session, None)
        for sessions in self.users.values():
            sessions.discard(session)

    def connections(self, user):
        """The sessions of `user`'s open connections."""
        return sorted(s for s in self.users.get(user, ()) if s in self.sockets)

    def receive(self, send):
        match send.event:
            case "deliver":                    # to the sending connection's own socket
                socket = self.sockets.get(send.sender.id)
                if socket is None:
                    send.fail()
                    return
                socket.write(json.dumps([send.data["event"], send.data["data"]]))
            case "broadcast":                  # a room's message: one send, many sockets
                text = json.dumps([send.data["event"], send.data["data"]])
                gone = []
                for member in send.data["to"]:   # #_scxml_<connection>: a fragment of its address
                    address = tinyactors.Address("tinyactors://%s/%s" % (send.host.system, member))
                    socket = self.sockets.get(address.session)
                    if socket is None:
                        gone.append(member)
                    else:
                        socket.write(text)
                if gone:                       # the room drops them
                    send.answer("members.gone", {"members": gone})
            case "close":
                socket = self.sockets.get(send.sender.id)
                self.detach(send.sender.id)
                if socket is not None:
                    socket.close(send.data["reason"])
            case _:
                send.fail()


class Database:
    """chat:db: the chat's own data, in SQLite: workspaces, rooms, messages
    and unread direct messages. What the actors keep is in memory or in
    the host's store; what must survive the process is here."""

    def __init__(self, path=":memory:"):
        self.connection = sqlite3.connect(path, isolation_level=None)
        self.connection.executescript("""
            CREATE TABLE IF NOT EXISTS workspaces (id TEXT PRIMARY KEY, owners TEXT NOT NULL);
            CREATE TABLE IF NOT EXISTS rooms (workspace TEXT, name TEXT, PRIMARY KEY (workspace, name));
            CREATE TABLE IF NOT EXISTS messages (id INTEGER PRIMARY KEY, workspace TEXT, room TEXT,
                                                 user TEXT, text TEXT);
            CREATE TABLE IF NOT EXISTS unread (id INTEGER PRIMARY KEY, sender TEXT, recipient TEXT,
                                               text TEXT);
        """)

    def receive(self, send):
        data = send.data
        match send.event:
            case "room.save":
                self.connection.execute("INSERT OR IGNORE INTO rooms VALUES (?, ?)",
                                        (data["workspace"], data["name"]))
            case "message.insert":
                self.connection.execute("INSERT INTO messages (workspace, room, user, text) VALUES (?, ?, ?, ?)",
                                        (data["workspace"], data["room"], data["user"], data["text"]))
            case _:
                send.fail()

    def close(self):
        self.connection.close()

    # What the application reads: at startup, and in the tests.

    def add_workspace(self, id, owners):
        self.connection.execute("INSERT OR REPLACE INTO workspaces VALUES (?, ?)", (id, json.dumps(owners)))

    def workspaces(self):
        return [(id, json.loads(owners)) for id, owners in
                self.connection.execute("SELECT id, owners FROM workspaces ORDER BY id")]

    def rooms(self):
        return self.connection.execute("SELECT workspace, name FROM rooms ORDER BY workspace, name").fetchall()

    def history(self, workspace, room, limit=200):
        rows = self.connection.execute("SELECT user, text FROM messages WHERE workspace = ? AND room = ? "
                                       "ORDER BY id DESC LIMIT ?", (workspace, room, limit)).fetchall()
        return [{"room": room, "user": user, "text": text} for user, text in reversed(rows)]

    def add_unread(self, message):
        self.connection.execute("INSERT INTO unread (sender, recipient, text) VALUES (?, ?, ?)",
                                (message["from"], message["to"], message["text"]))

    def unread(self, user):
        return [{"from": sender, "to": user, "text": text} for sender, text in self.connection.execute(
            "SELECT sender, text FROM unread WHERE recipient = ? ORDER BY id", (user,))]


class Inbox:
    """chat:inbox: direct messages, routed by the application (strategy
    B). A user is not an actor: the recipient's connections are, and there
    may be none. The processor looks them up in the gateway and forwards
    the message to each; with none, it keeps the message as unread."""

    def __init__(self, gateway, database):
        self.gateway = gateway
        self.database = database

    def receive(self, send):
        if send.event != "dm":
            send.fail()
            return
        delivered = 0
        for session in self.gateway.connections(send.data["to"]):
            try:
                send.host.send(session, "dm", send.data)
                delivered += 1
            except tinyactors.StaleError:      # it ended meanwhile
                self.gateway.detach(session)
        if not delivered:
            self.database.add_unread(send.data)
