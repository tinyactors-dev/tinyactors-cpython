"""The chat end to end: the charts, the processors and main.py's server,
with sockets that record what they are given (and one test over TCP)."""

import asyncio
import json
import unittest

import tinyactors

import main
from processors import Database, Gateway


class Socket:
    """Stands in for a client's socket: records the frames written to it."""

    def __init__(self):
        self.frames = []
        self.closed = None

    def write(self, text):
        self.frames.append(json.loads(text))

    def close(self, reason):
        self.closed = reason


class Chat(unittest.IsolatedAsyncioTestCase):
    @classmethod
    def setUpClass(cls):
        cls.charts = main.compile_charts()            # once, for every host of the tests

    @classmethod
    def tearDownClass(cls):
        cls.charts.close()

    async def asyncSetUp(self):
        self.database = Database()
        self.database.add_workspace("acme", ["ada"])
        self.gateway = Gateway()
        self.clock = tinyactors.SimulatedClock()
        self.store = tinyactors.MemoryStore()
        self.errors = []
        self.host = main.open_host(self.charts, self.database, self.gateway, clock=self.clock,
                                   store=self.store, resident=3,
                                   on_handler_error=lambda error, effect: self.errors.append(error))
        await self.host.__aenter__()
        main.start(self.host, self.database)
        await self.host.idle()

    async def asyncTearDown(self):
        await self.host.__aexit__(None, None, None)
        self.database.close()
        self.assertEqual(self.errors, [])

    def connect(self, user):
        connection = self.host.spawn("connection", params={"user": user, "workspace": "acme"})
        socket = Socket()
        self.gateway.attach(connection, user, socket)
        return connection, socket

    async def create_room(self, connection, socket, name="general"):
        connection.send("client.create", {"name": name})
        await self.host.idle()
        self.assertEqual(socket.frames[-1], ["room.created", {"room": name}])

    async def test_an_owner_creates_a_room_others_join_and_hear_each_other(self):
        (ada, ada_socket), (bob, bob_socket) = self.connect("ada"), self.connect("bob")
        await self.create_room(ada, ada_socket)
        self.assertEqual(self.database.rooms(), [("acme", "general")])   # the room saved itself

        bob.send("client.create", {"name": "random"})
        for connection in (ada, bob):
            connection.send("client.join", {"room": "general"})
        await self.host.idle()
        self.assertIn(["room.refused", {"name": "random", "reason": "not an owner"}], bob_socket.frames)
        self.assertIn(["room.joined", {"room": "general", "history": []}], bob_socket.frames)

        bob.send("client.post", {"room": "general", "text": "hi"})
        await self.host.idle()
        expected = ["room.message", {"room": "general", "user": "bob", "text": "hi"}]
        self.assertEqual(ada_socket.frames[-1], expected)
        self.assertEqual(bob_socket.frames[-1], expected)
        self.assertEqual(self.database.history("acme", "general"), [expected[1]])

    async def test_room_names_need_no_encoding_in_an_address(self):
        ada, socket = self.connect("ada")
        for name in ("room/general", "Général", ""):
            ada.send("client.create", {"name": name})
        await self.host.idle()
        self.assertEqual([frame[1]["reason"] for frame in socket.frames], ["invalid name"] * 3)
        address = tinyactors.Address.from_name("chat", "room.acme.general")
        self.assertEqual(address.text, "tinyactors://chat/@room.acme.general")  # the name as written

    async def test_a_hibernated_workspace_wakes_when_an_event_comes_for_it(self):
        # Strategy A: the workspace never ends; past `resident` it is a
        # blob in the store, and core wakes it by its name.
        (ada, socket), *others = [self.connect(user) for user in ("ada", "bob", "cy", "dan")]
        await self.host.idle()
        workspace = self.host.actor("workspace.acme")
        self.assertIn(workspace.id, self.store.blobs)  # the coldest of five actors, three resident
        await self.create_room(ada, socket)
        self.assertEqual(await workspace.ask("rooms"), ("rooms", {"rooms": ["general"]}))

    async def test_rooms_from_the_database_announce_themselves_to_their_workspace(self):
        self.database.connection.execute("INSERT INTO rooms VALUES ('acme', 'old')")
        self.database.connection.execute("INSERT INTO messages (workspace, room, user, text) "
                                         "VALUES ('acme', 'old', 'ada', 'remember me')")
        await self.host.__aexit__(None, None, None)
        self.host = main.open_host(self.charts, self.database, self.gateway, clock=self.clock,
                                   store=tinyactors.MemoryStore(), resident=3)
        await self.host.__aenter__()
        main.start(self.host, self.database)                 # a restart, from the database
        await self.host.idle()
        self.assertEqual(await self.host.actor("workspace.acme").ask("rooms"), ("rooms", {"rooms": ["old"]}))
        bob, socket = self.connect("bob")
        bob.send("client.join", {"room": "old"})
        await self.host.idle()
        self.assertEqual(socket.frames[-1], ["room.joined", {"room": "old", "history": [
            {"room": "old", "user": "ada", "text": "remember me"}]}])

    async def test_a_join_to_a_room_that_is_not_there_is_the_senders_error(self):
        # Strategy C: core gives the sender error.communication at once.
        bob, socket = self.connect("bob")
        bob.send("client.join", {"room": "nowhere"})
        await self.host.idle()
        self.assertEqual(socket.frames, [["error", {"reason": "no such room"}]])

    async def test_a_direct_message_is_routed_by_the_inbox(self):
        # Strategy B: the recipient's connections may not exist; the
        # processor looks them up and forwards, or keeps the message.
        (ada, _), (bob, bob_socket) = self.connect("ada"), self.connect("bob")
        ada.send("client.dm", {"to": "bob", "text": "psst"})
        ada.send("client.dm", {"to": "cy", "text": "later"})
        await self.host.idle()
        self.assertEqual(bob_socket.frames, [["dm", {"from": "ada", "to": "bob", "text": "psst"}]])
        self.assertEqual(self.database.unread("cy"), [{"from": "ada", "to": "cy", "text": "later"}])

    async def test_a_missed_heartbeat_closes_the_socket_and_leaves_the_room(self):
        ada, ada_socket = self.connect("ada")
        bob, bob_socket = self.connect("bob")
        await self.create_room(ada, ada_socket)
        for connection in (ada, bob):
            connection.send("client.join", {"room": "general"})
        await self.host.idle()
        ending = ada.done
        self.clock.increment(30)
        bob.send("client.heartbeat", {})
        await self.host.idle()
        self.clock.increment(31)                        # ada's minute is up, bob's is not
        await self.host.idle()
        self.assertEqual((ada_socket.closed, bob_socket.closed), ("timeout", None))
        self.assertIsNone(await ending)                 # it ended, and left its room
        room = self.host.actor("room.acme.general")
        self.assertEqual(await room.ask("members"), ("members", {"members": ["bob"]}))

    async def test_a_member_whose_socket_vanished_is_dropped(self):
        ada, ada_socket = self.connect("ada")
        bob, _ = self.connect("bob")
        await self.create_room(ada, ada_socket)
        for connection in (ada, bob):
            connection.send("client.join", {"room": "general"})
        await self.host.idle()
        self.gateway.detach(bob.id)                     # gone without a goodbye
        ada.send("client.post", {"room": "general", "text": "anyone?"})
        await self.host.idle()
        room = self.host.actor("room.acme.general")
        self.assertEqual(await room.ask("kick", {"user": "nobody"}), ("kicked", {"members": ["ada"]}))


class Server(unittest.IsolatedAsyncioTestCase):
    async def test_two_clients_over_tcp(self):
        ready = asyncio.get_running_loop().create_future()
        server = asyncio.create_task(main.main(0, ":memory:", 10, ready=ready.set_result))
        port = await asyncio.wait_for(ready, 5)

        async def client(user):
            reader, writer = await asyncio.open_connection("127.0.0.1", port)
            writer.write(json.dumps({"user": user, "workspace": "acme"}).encode() + b"\n")
            return reader, writer

        async def say(writer, event, data):
            writer.write(json.dumps([event, data]).encode() + b"\n")
            await writer.drain()

        async def read(reader):
            return json.loads(await asyncio.wait_for(reader.readline(), 5))

        (ada_reader, ada), (bob_reader, bob) = await client("ada"), await client("bob")
        await say(ada, "create", {"name": "general"})
        self.assertEqual(await read(ada_reader), ["room.created", {"room": "general"}])
        for reader, writer in ((ada_reader, ada), (bob_reader, bob)):
            await say(writer, "join", {"room": "general"})
            self.assertEqual((await read(reader))[0], "room.joined")
        await say(ada, "post", {"room": "general", "text": "hello"})
        expected = ["room.message", {"room": "general", "user": "ada", "text": "hello"}]
        self.assertEqual(await read(bob_reader), expected)
        self.assertEqual(await read(ada_reader), expected)
        for writer in (ada, bob):
            writer.close()
        server.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await server


if __name__ == "__main__":
    unittest.main()
