"""A chat server: workspaces, rooms and connections as actors.

Clients speak JSON lines over TCP: the first line says who they are,
{"user": "ada", "workspace": "acme"}, and every line after it is an
[event, data] pair (["join", {"room": "general"}], ["post", {"room":
"general", "text": "hi"}], ["create", {"name": "random"}], ["dm", {"to":
"bob", "text": "psst"}], ["heartbeat", {}], ...). The server answers in
the same form. Try it with two terminals of `nc localhost 8765`.

    python main.py [--port 8765] [--database chat.db] [--resident 10000]
"""

import argparse
import asyncio
import json
import pathlib
import tempfile

import tinyactors

from processors import Database, Gateway, Inbox

CHARTS = pathlib.Path(__file__).parent / "charts"


def compile_charts():
    """The charts and their .py helpers, compiled once per process."""
    return tinyactors.compile_dir(CHARTS)


def open_host(charts, database, gateway, **options):
    """A host of the chat's actors, with its processors: one per external
    system, and the binding's Spawner under the type the charts name."""
    return tinyactors.Host(charts, system="chat",
                           io_processors={"chat:gateway": gateway,
                                          "chat:db": database,
                                          "chat:inbox": Inbox(gateway, database),
                                          "spawn": tinyactors.Spawner()},
                           **options)


def start(host, database):
    """Spawn the workspaces and rooms the database holds, each under its
    name. A room announces itself to its workspace when it starts, so
    nothing here tells the workspace which rooms it has."""
    for id, owners in database.workspaces():
        host.spawn("workspace", name="workspace." + id, params={"id": id, "owners": owners})
    for workspace, room in database.rooms():
        host.spawn("room", name="room.%s.%s" % (workspace, room),
                   params={"workspace": workspace, "name": room,
                           "history": database.history(workspace, room)})


class Socket:
    """A client's TCP stream, as the gateway writes to it."""

    def __init__(self, writer):
        self.writer = writer

    def write(self, text):
        self.writer.write(text.encode() + b"\n")

    def close(self, reason):
        self.write(json.dumps(["closed", {"reason": reason}]))
        self.writer.close()


def client(host, gateway):
    """The handler of one client's connection: a connection actor for its
    lifetime, the frames it reads given to that actor as client.* events."""
    async def serve(reader, writer):
        hello = json.loads(await reader.readline())
        connection = host.spawn("connection", params={"user": hello["user"], "workspace": hello["workspace"]})
        gateway.attach(connection, hello["user"], Socket(writer))
        try:
            async for line in reader:
                event, data = json.loads(line)
                connection.send("client." + event, data)   # the chart ignores what it does not know
        except (ValueError, ConnectionError):
            pass
        finally:
            gateway.detach(connection.id)
            try:
                connection.send("socket.closed")
            except tinyactors.StaleError:                   # it ended first (a missed heartbeat)
                pass
            writer.close()
    return serve


async def main(port, database_path, resident, ready=None):
    database = Database(database_path)
    if not database.workspaces():
        database.add_workspace("acme", ["ada"])
    gateway = Gateway()
    with tempfile.TemporaryDirectory() as directory:
        # The store is a cache of this host's actors: its blobs wake only
        # in this host, so it starts empty with the process.
        store = tinyactors.SQLiteStore(str(pathlib.Path(directory, "actors.db")))
        try:
            with compile_charts() as charts:
                host = open_host(charts, database, gateway, store=store, resident=resident)
            async with host:
                start(host, database)
                server = await asyncio.start_server(client(host, gateway), "127.0.0.1", port)
                async with server:
                    if ready is not None:
                        ready(server.sockets[0].getsockname()[1])
                    await server.serve_forever()
        finally:
            store.close()
            database.close()


if __name__ == "__main__":
    arguments = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    arguments.add_argument("--port", type=int, default=8765)
    arguments.add_argument("--database", default="chat.db")
    arguments.add_argument("--resident", type=int, default=10_000)
    options = arguments.parse_args()
    try:
        asyncio.run(main(options.port, options.database, options.resident))
    except KeyboardInterrupt:
        pass
