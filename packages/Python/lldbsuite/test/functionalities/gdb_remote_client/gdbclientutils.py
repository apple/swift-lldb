import os
import os.path
import subprocess
import threading
import socket
import lldb
from lldbsuite.test.lldbtest import *
from lldbsuite.test import lldbtest_config


def checksum(message):
    """
    Calculate the GDB server protocol checksum of the message.

    The GDB server protocol uses a simple modulo 256 sum.
    """
    check = 0
    for c in message:
        check += ord(c)
    return check % 256


def frame_packet(message):
    """
    Create a framed packet that's ready to send over the GDB connection
    channel.

    Framing includes surrounding the message between $ and #, and appending
    a two character hex checksum.
    """
    return "$%s#%02x" % (message, checksum(message))


def escape_binary(message):
    """
    Escape the binary message using the process described in the GDB server
    protocol documentation.

    Most bytes are sent through as-is, but $, #, and { are escaped by writing
    a { followed by the original byte mod 0x20.
    """
    out = ""
    for c in message:
        d = ord(c)
        if d in (0x23, 0x24, 0x7d):
            out += chr(0x7d)
            out += chr(d ^ 0x20)
        else:
            out += c
    return out


def hex_encode_bytes(message):
    """
    Encode the binary message by converting each byte into a two-character
    hex string.
    """
    out = ""
    for c in message:
        out += "%02x" % ord(c)
    return out


def hex_decode_bytes(hex_bytes):
    """
    Decode the hex string into a binary message by converting each two-character
    hex string into a single output byte.
    """
    out = ""
    hex_len = len(hex_bytes)
    while i < hex_len - 1:
        out += chr(int(hex_bytes[i:i + 2]), 16)
        i += 2
    return out


class MockGDBServerResponder:
    """
    A base class for handing client packets and issuing server responses for
    GDB tests.

    This handles many typical situations, while still allowing subclasses to
    completely customize their responses.

    Most subclasses will be interested in overriding the other() method, which
    handles any packet not recognized in the common packet handling code.
    """

    registerCount = 40
    packetLog = None

    def __init__(self):
        self.packetLog = []

    def respond(self, packet):
        """
        Return the unframed packet data that the server should issue in response
        to the given packet received from the client.
        """
        self.packetLog.append(packet)
        if packet == "g":
            return self.readRegisters()
        if packet[0] == "G":
            return self.writeRegisters(packet[1:])
        if packet[0] == "p":
            return self.readRegister(int(packet[1:], 16))
        if packet[0] == "P":
            register, value = packet[1:].split("=")
            return self.readRegister(int(register, 16), value)
        if packet[0] == "m":
            addr, length = [int(x, 16) for x in packet[1:].split(',')]
            return self.readMemory(addr, length)
        if packet[0] == "M":
            location, encoded_data = packet[1:].split(":")
            addr, length = [int(x, 16) for x in location.split(',')]
            return self.writeMemory(addr, encoded_data)
        if packet[0:7] == "qSymbol":
            return self.qSymbol(packet[8:])
        if packet[0:10] == "qSupported":
            return self.qSupported(packet[11:].split(";"))
        if packet == "qfThreadInfo":
            return self.qfThreadInfo()
        if packet == "qC":
            return self.qC()
        if packet == "?":
            return self.haltReason()
        if packet[0] == "H":
            return self.selectThread(packet[1], int(packet[2:], 16))
        if packet[0:6] == "qXfer:":
            obj, read, annex, location = packet[6:].split(":")
            offset, length = [int(x, 16) for x in location.split(',')]
            data, has_more = self.qXferRead(obj, annex, offset, length)
            if data is not None:
                return self._qXferResponse(data, has_more)
            return ""
        return self.other(packet)

    def readRegisters(self):
        return "00000000" * self.registerCount

    def readRegister(self, register):
        return "00000000"

    def writeRegisters(self, registers_hex):
        return "OK"

    def writeRegister(self, register, value_hex):
        return "OK"

    def readMemory(self, addr, length):
        return "00" * length

    def writeMemory(self, addr, data_hex):
        return "OK"

    def qSymbol(self, symbol_args):
        return "OK"

    def qSupported(self, client_supported):
        return "PacketSize=3fff;QStartNoAckMode+"

    def qfThreadInfo(self):
        return "l"

    def qC(self):
        return "QC0"

    def haltReason(self):
        # SIGINT is 2, return type is 2 digit hex string
        return "S02"

    def qXferRead(self, obj, annex, offset, length):
        return None, False

    def _qXferResponse(self, data, has_more):
        return "%s%s" % ("m" if has_more else "l", escape_binary(data))

    def selectThread(self, op, thread_id):
        return "OK"

    def other(self, packet):
        # empty string means unsupported
        return ""


class MockGDBServer:
    """
    A simple TCP-based GDB server that can test client behavior by receiving
    commands and issuing custom-tailored responses.

    Responses are generated via the .responder property, which should be an
    instance of a class based on MockGDBServerResponder.
    """

    responder = None
    port = 0
    _socket = None
    _client = None
    _thread = None
    _receivedData = None
    _receivedDataOffset = None
    _shouldSendAck = True
    _isExpectingAck = False

    def __init__(self, port = 0):
        self.responder = MockGDBServerResponder()
        self.port = port
        self._socket = socket.socket()

    def start(self):
        # Block until the socket is up, so self.port is available immediately.
        # Then start a thread that waits for a client connection.
        addr = ("127.0.0.1", self.port)
        self._socket.bind(addr)
        self.port = self._socket.getsockname()[1]
        self._socket.listen(0)
        self._thread = threading.Thread(target=self._run)
        self._thread.start()

    def stop(self):
        self._socket.close()
        self._thread.join()
        self._thread = None

    def _run(self):
        # For testing purposes, we only need to worry about one client
        # connecting just one time.
        try:
            # accept() is stubborn and won't fail even when the socket is
            # shutdown, so we'll use a timeout
            self._socket.settimeout(2.0)
            client, client_addr = self._socket.accept()
            self._client = client
            # The connected client inherits its timeout from self._socket,
            # but we'll use a blocking socket for the client
            self._client.settimeout(None)
        except:
            return
        self._shouldSendAck = True
        self._isExpectingAck = False
        self._receivedData = ""
        self._receivedDataOffset = 0
        data = None
        while True:
            try:
                data = self._client.recv(4096)
                if data is None or len(data) == 0:
                    break
            except Exception as e:
                self._client.close()
                break
            self._receive(data)

    def _receive(self, data):
        """
        Collects data, parses and responds to as many packets as exist.
        Any leftover data is kept for parsing the next time around.
        """
        self._receivedData += data
        try:
            packet = self._parsePacket()
            while packet is not None:
                self._handlePacket(packet)
                packet = self._parsePacket()
        except self.InvalidPacketException:
            self._client.close()

    def _parsePacket(self):
        """
        Reads bytes from self._receivedData, returning:
        - a packet's contents if a valid packet is found
        - the PACKET_ACK unique object if we got an ack
        - None if we only have a partial packet

        Raises an InvalidPacketException if unexpected data is received
        or if checksums fail.

        Once a complete packet is found at the front of self._receivedData,
        its data is removed form self._receivedData.
        """
        data = self._receivedData
        i = self._receivedDataOffset
        data_len = len(data)
        if data_len == 0:
            return None
        if i == 0:
            # If we're looking at the start of the received data, that means
            # we're looking for the start of a new packet, denoted by a $.
            # It's also possible we'll see an ACK here, denoted by a +
            if data[0] == '+':
                self._receivedData = data[1:]
                return self.PACKET_ACK
            if data[0] == '$':
                i += 1
            else:
                raise self.InvalidPacketException(
                        "Unexexpected leading byte: %s" % data[0])

        # If we're looking beyond the start of the received data, then we're
        # looking for the end of the packet content, denoted by a #.
        # Note that we pick up searching from where we left off last time
        while i < data_len and data[i] != '#':
            i += 1

        # If there isn't enough data left for a checksum, just remember where
        # we left off so we can pick up there the next time around
        if i > data_len - 3:
            self._receivedDataOffset = i
            return None

        # If we have enough data remaining for the checksum, extract it and
        # compare to the packet contents
        packet = data[1:i]
        i += 1
        try:
            check = int(data[i:i + 2], 16)
        except ValueError:
            raise self.InvalidPacketException("Checksum is not valid hex")
        i += 2
        if check != checksum(packet):
            raise self.InvalidPacketException(
                    "Checksum %02x does not match content %02x" %
                    (check, checksum(packet)))
        # remove parsed bytes from _receivedData and reset offset so parsing
        # can start on the next packet the next time around
        self._receivedData = data[i:]
        self._receivedDataOffset = 0
        return packet

    def _handlePacket(self, packet):
        if packet is self.PACKET_ACK:
            # If we are expecting an ack, we'll just ignore it because there's
            # nothing else we're supposed to do.
            #
            # However, if we aren't expecting an ack, it's likely the initial
            # ack that lldb client sends, and observations of real servers
            # suggest we're supposed to ack back.
            if not self._isExpectingAck:
                self._client.sendall('+')
            return
        response = ""
        # We'll handle the ack stuff here since it's not something any of the
        # tests will be concerned about, and it'll get turned off quicly anyway.
        if self._shouldSendAck:
            self._client.sendall('+')
            self._isExpectingAck = True
        if packet == "QStartNoAckMode":
            self._shouldSendAck = False
            response = "OK"
        elif self.responder is not None:
            # Delegate everything else to our responder
            response = self.responder.respond(packet)
        # Handle packet framing since we don't want to bother tests with it.
        framed = frame_packet(response)
        self._client.sendall(framed)

    PACKET_ACK = object()

    class InvalidPacketException(Exception):
        pass


class GDBRemoteTestBase(TestBase):
    """
    Base class for GDB client tests.

    This class will setup and start a mock GDB server for the test to use.
    It also provides assertPacketLogContains, which simplifies the checking
    of packets sent by the client.
    """

    NO_DEBUG_INFO_TESTCASE = True
    mydir = TestBase.compute_mydir(__file__)
    server = None
    temp_files = None

    def setUp(self):
        TestBase.setUp(self)
        self.temp_files = []
        self.server = MockGDBServer()
        self.server.start()

    def tearDown(self):
        for temp_file in self.temp_files:
            self.RemoveTempFile(temp_file)
        # TestBase.tearDown will kill the process, but we need to kill it early
        # so its client connection closes and we can stop the server before
        # finally calling the base tearDown.
        if self.process() is not None:
            self.process().Kill()
        self.server.stop()
        self.temp_files = []
        TestBase.tearDown(self)

    def createTarget(self, yaml_path):
        """
        Create a target by auto-generating the object based on the given yaml
        instructions.

        This will track the generated object so it can be automatically removed
        during tearDown.
        """
        yaml_base, ext = os.path.splitext(yaml_path)
        obj_path = "%s" % yaml_base
        self.yaml2obj(yaml_path, obj_path)
        self.temp_files.append(obj_path)
        return self.dbg.CreateTarget(obj_path)

    def connect(self, target):
        """
        Create a process by connecting to the mock GDB server.

        Includes assertions that the process was successfully created.
        """
        listener = self.dbg.GetListener()
        error = lldb.SBError()
        url = "connect://localhost:%d" % self.server.port
        process = target.ConnectRemote(listener, url, "gdb-remote", error)
        self.assertTrue(error.Success(), error.description)
        self.assertTrue(process, PROCESS_IS_VALID)

    def assertPacketLogContains(self, packets):
        """
        Assert that the mock server's packet log contains the given packets.

        The packet log includes all packets sent by the client and received
        by the server.  This fuction makes it easy to verify that the client
        sent the expected packets to the server.

        The check does not require that the packets be consecutive, but does
        require that they are ordered in the log as they ordered in the arg.
        """
        i = 0
        j = 0
        log = self.server.responder.packetLog
        while i < len(packets) and j < len(log):
            if log[j] == packets[i]:
                i += 1
            j += 1
        if i < len(packets):
            self.fail("Did not receive: %s\nLast 10 packets:\n\t%s" %
                    (packets[i], '\n\t'.join(log[-10:])))
