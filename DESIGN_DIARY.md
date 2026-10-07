# Design Diary: NetMessenger

**Registration Number:** IT23584822
**Port:** 10822
**NID:** 5848
**Language / Platform:** C (POSIX sockets), CentOS Linux
**Period:** 03.10.2026 to 07.10.2026

---

## 03.10.2026: Initial project structure

**Goal:** Set up the repository and decide how the project is organised before writing any networking code.

**Work done**
- Created the repository and the files `server_4822.c`, `client_4822.c`, `Makefile_4822` and `README.md`.
- Fixed the project constants in one place: port `10822`, `NID:5848`, buffer sizes, `STORAGE_ROOT` (`./storage/IT23584822`) and `LOG_FILE` (`netmsg_IT23584822.log`).
- Decided to compile with `-Wall -Wextra` from the start so that warnings are fixed early.

**Design decisions**
- Server and client are two separate programs, using the client-server model over TCP.
- Every server reply ends with `NID:5848` so replies are easy to identify.

**Commit:** Initial project structure

---

## 04.10.2026: Basic TCP server and client, registration and user management

**Goal:** Get a client talking to the server, then identify each user by name.

**Work done**
- Server: `socket()`, `bind()`, `listen()`, `accept()` on port 10822, with `SO_REUSEADDR` set so the server can restart immediately.
- Client: `connect()` to the host and port given on the command line, then a loop that reads typed commands and sends them to the server.
- Added `REGISTER <username>` and `LIST`.
- Added a `Client` structure holding the socket, username, registered flag and current room. Unused slots are marked with `fd = -1`.

**Design decisions**
- **Line-based text protocol:** each command is one line ending in `\n`, which is simple to read and easy to test by hand.
- **Registration is required first:** any command other than `REGISTER` from an unregistered client returns `ERR 002 NOT_REGISTERED`.
- **Unique usernames:** a duplicate returns `ERR 001 USERNAME_TAKEN`, and registering twice returns `ERR 003 ALREADY_REGISTERED`.
- Usernames are limited to 49 characters, so `sscanf` uses `%49s` to prevent buffer overflow.

**Problems met:** TCP is a byte stream, so one `recv()` can contain half a command or several. I made a note to buffer input per client (done on 05.10).

**Commits:** Basic TCP server and client; Registration and user management

---

## 05.10.2026: Multiple-client concurrency, broadcast and private messaging

**Goal:** Serve many clients at the same time, then let them message each other.

**Work done**
- Replaced the one-client loop with `select()` on a master `fd_set`: new connections are accepted when the listening socket is readable, and each existing client socket is read when it has data.
- Added a per-client input buffer. A command is processed only once a full line (`\n`) has arrived, and several commands in one `recv()` are all handled.
- Added `BCAST <message>` and `PMSG <user> <message>`.
- Added join and leave notifications (`MSG JOIN <user>`, `MSG LEAVE <user>`).
- When a client disconnects, `remove_client()` closes its socket, clears its slot and removes it from the `fd_set`.

**Design decisions**
- **`select()` instead of threads or `fork()`:** one process keeps all client data in one array with no locking, which is easier to get right. The trade-off is that one slow handler delays everyone, which is acceptable for this project.
- **Delivery format:** `MSG BCAST <sender> <text>` and `MSG PRIV <sender> <text>`, so the client can show who sent what.
- The sender gets `OK SENT NID:5848`. A private message to an unknown user returns `ERR 002 USER_NOT_FOUND`.
- Over-long commands are rejected with `ERR 006 COMMAND_TOO_LONG` rather than overflowing a buffer.

**Problems met:** Finding a client by username means scanning the array. That is fine for this number of clients, so I kept it simple instead of adding a hash table.

**Commits:** Multiple-client concurrency; Broadcast and private messaging

---

## 06.10.2026: Chat rooms, file transfer, error handling and logging

**Goal:** Add rooms and file sharing, then make the server robust and traceable.

### Chat rooms
- Added `JOIN <room>`, `LEAVE <room>`, `ROOMS` and `RMSG <room> <message>`.
- A room exists while at least one client is in it, so rooms are stored as a field on each client instead of as a separate structure. `ROOMS` lists the distinct rooms in use.
- Messaging a room that does not exist returns `ERR 003 ROOM_NOT_FOUND`.

### File transfer
- Protocol: the sender issues `SENDFILE <user|room> <filename> <size>` and then sends exactly `<size>` raw bytes.
- The server switches that client into a "receiving file" state and copies exactly `<size>` bytes to disk. Any bytes left over in the same `recv()` are treated as the next command.
- The file is stored at `storage/IT23584822/<sender>/<filename>`, then forwarded to the target user (or to every member of the room).
- Receivers get a header `FILE_FROM <sender> <filename> <size>` followed by the raw bytes, and the client saves it into a local `received/` folder.
- The assignment defines sender-to-server framing but not the receiver-side header, so `FILE_FROM` is my own addition.

**File transfer problems and fixes**
- **Raw bytes vs text commands:** file data must never be parsed as a command, which is why the receiving-file state exists.
- **Path safety:** filenames are copied with a length limit, and path buffers were enlarged so a long username plus filename cannot be truncated.
- **Write failure:** if the server cannot create the file it returns `ERR 005 FILE_WRITE_ERROR`.

### Error handling and logging
- The server writes to `netmsg_IT23584822.log` in the format `[YYYY-MM-DD HH:MM:SS] [INFO|ERROR] message`.
- Logged events: server start and stop, connections, registrations, `BCAST`, `PMSG`, `JOIN`, `LEAVE`, `RMSG`, received files, `QUIT` and disconnects.
- Every `ERR ...` reply is logged automatically from `send_line()`, so no error case is forgotten.
- Unknown or malformed commands return `ERR 006 UNKNOWN_COMMAND` and the server keeps running.
- Unexpected disconnects (`recv()` returning 0) are cleaned up like a normal `QUIT`.
- `SIGPIPE` is ignored so writing to a closed socket cannot crash the server, and Ctrl+C shuts the server down cleanly and logs it.
- Fixed all compiler warnings: unused parameter, possible string truncation in message and path buffers, and an unused client function.

**Commits:** Chat rooms; File transfer; Error handling and logging

---

## 07.10.2026: Testing and documentation 

**Goal:** Prove each feature works from a clean build and finish the documentation.

**work done**
1. Clean build with `make -f Makefile_4822 clean` then `make -f Makefile_4822`, expecting zero warnings.
2. Register five clients and check `LIST` shows all of them.
3. Duplicate username returns `ERR 001`.
4. `BCAST` reaches every other client; `PMSG` reaches only the target.
5. `JOIN`, `ROOMS`, `RMSG` and `LEAVE` work, and messages to an unknown room return `ERR 003`.
6. `SENDFILE` to a user and to a room; compare sent and received files with `cmp` or `md5sum`.
7. Kill a client without `QUIT` and check the others see `MSG LEAVE` and the server stays up.
8. Send unknown commands and confirm the server stays up and logs the error.
9. Review `netmsg_IT23584822.log` for the expected entries.

**Documentation:** finalise `README.md` (build, run, commands, error codes, storage and log locations) and this diary.

**Commit:** Testing and documentation

---

## Summary of final design

| Area | Choice | Reason |
|---|---|---|
| Concurrency | Single process, `select()` | No locking, simple shared state |
| Protocol | Text lines ending in `\n`, replies end with `NID:5848` | Easy to read, test and debug |
| Input handling | Per-client buffer, process only complete lines | TCP is a stream with no message boundaries |
| Files | Declared size, then exact raw bytes | Binary-safe, no confusion with commands |
| Rooms | Room name stored on each client | Simple, no room table to maintain |
| Reliability | Log every event, ignore `SIGPIPE`, clean disconnects | Server must survive bad clients |
