# Oserveroserver

A simple broadcast chat server using C, `libwebsockets`, and `sqlite3`.

## Features

-   **WebSocket-based:** For real-time, bidirectional communication.
-   **Role-based access control:**
    -   **Writers:** Can send messages. Only one writer is allowed at a time to ensure a "single source of truth."
    -   **Readers:** Can only view messages. Multiple readers are allowed, but not while a writer is present.
-   **SQLite message history:** Chat messages are stored in a local SQLite database file (`chat_history.sqlite` by default). History is loaded for new clients and cleared on server startup.
-   **Simple web client:** A basic HTML/CSS/JS client is provided for interacting with the server.

## Building and Running

**Dependencies:**

-   `libwebsockets-dev` (or equivalent for your OS)
-   `libsqlite3-dev` (or equivalent for your OS)
-   `gcc` or `clang`
-   `pkg-config`

**Build command:**

```bash
cd oserveroserver
gcc server.c -o server $(pkg-config --cflags --libs libwebsockets sqlite3)
```

**Running the server:**

```bash
./server [database_file.sqlite]
```

If no database file is specified, it defaults to `chat_history.sqlite`.

## Client

To use the client, open the `oserveroserver/index.html` file in a web browser. You will be prompted to enter a username and select a role (Reader or Writer) before connecting.
