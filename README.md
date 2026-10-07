# NetMessenger

Project: NetMessenger
Registration Number: IT23584822
Port: 10822
NID: 5848

## Compilation

    make -f Makefile_4822

Clean build:

    make -f Makefile_4822 clean
    make -f Makefile_4822

## Run

Server:

    ./server_4822

Client:

    ./client_4822 127.0.0.1 10822

## Storage

    ./storage/IT23584822/

Received files are stored in a sub-directory named after the sender.

## Log

    netmsg_IT23584822.log

Format: `[YYYY-MM-DD HH:MM:SS] [INFO|ERROR] message`

## Commands

| Command | Description |
|---|---|
| `REGISTER <username>` | Register a username (required before any other command) |
| `LIST` | List registered users |
| `BCAST <message>` | Send a message to all users |
| `PMSG <user> <message>` | Send a private message to one user |
| `JOIN <room>` | Join (or create) a room |
| `LEAVE <room>` | Leave the current room |
| `ROOMS` | List active rooms |
| `RMSG <room> <message>` | Send a message to a room |
| `SENDFILE <user\|room> <filename>` | Send a file to a user or room |
| `QUIT` | Disconnect (`OK BYE NID:5848`) |

## Error codes

| Code | Meaning |
|---|---|
| `ERR 001 USERNAME_TAKEN` | Username already in use |
| `ERR 002 USER_NOT_FOUND` / `NOT_REGISTERED` | Unknown user, or command sent before REGISTER |
| `ERR 003 ROOM_NOT_FOUND` / `ALREADY_REGISTERED` | Unknown room, or REGISTER sent twice |
| `ERR 004 INVALID_USERNAME` | Bad username |
| `ERR 005 FILE_WRITE_ERROR` | Server could not write the file |
| `ERR 006 UNKNOWN_COMMAND` / `INVALID_*` | Malformed or unknown command |

## Robustness

- Malformed/unknown commands return an error; the server keeps running.
- Unexpected client disconnects (`recv()` returns 0) are cleaned up: socket closed, username and room cleared, fd removed from the `fd_set`.
- `SIGPIPE` is ignored; Ctrl+C on the server shuts it down cleanly and logs it.
