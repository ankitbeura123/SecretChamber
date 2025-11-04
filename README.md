# Oserveroserver: A Detailed Technical Guide

Oserveroserver is a simple yet powerful broadcast chat server built with C, `libwebsockets`, and `sqlite3`. This document provides a comprehensive overview of the project, from its high-level architecture to a detailed explanation of every function in the codebase.

## Project Overview

The server facilitates real-time, bidirectional communication between clients using WebSockets. It features a role-based access control system that distinguishes between "Writers" and "Readers," ensuring a single source of truth by allowing only one Writer at a time. All chat messages are persistently stored in a local SQLite database, and a simple, intuitive web client is provided for interacting with the server.

### Key Features

*   **WebSocket-Based Communication:** Enables real-time, low-latency communication between the server and its clients.
*   **Role-Based Access Control:**
    *   **Writers:** Can send messages to all connected clients. Only one Writer is allowed at a time.
    *   **Readers:** Can only view messages. Multiple Readers are allowed, but not while a Writer is present.
*   **SQLite Message History:** Chat messages are stored in a local SQLite database, providing a persistent chat history.
*   **Simple Web Client:** A user-friendly HTML/CSS/JS client is provided for easy interaction with the server.

### Architecture

The server is built upon the following components:

*   **`libwebsockets`:** A lightweight, pure C library for modern WebSockets.
*   **`sqlite3`:** A C-language library that implements a small, fast, self-contained, high-reliability, full-featured, SQL database engine.
*   **`pthread`:** A POSIX threads library for managing concurrent operations and protecting shared data structures.

The server operates a single-threaded event loop managed by `libwebsockets`, which handles all WebSocket-related events. Client management and database operations are designed to be thread-safe, using mutexes and read-write locks to prevent data corruption.

## Server-Side Implementation (`server.c`)

The server-side logic is encapsulated within `server.c`. This section provides a detailed breakdown of its components.

### Data Structures

#### `struct client`

This structure represents a connected client and stores its state:

```c
struct client {
    struct lws *wsi;
    char username[MAX_NAME_LEN];
    char role[MAX_ROLE_LEN]; // "READER", "WRITER" or "NONE"
    struct client *next;
};
```

*   `wsi`: A pointer to the `libwebsockets` WebSocket instance, used to identify the client's connection.
*   `username`: The client's chosen username.
*   `role`: The client's role, which can be "READER", "WRITER", or "NONE".
*   `next`: A pointer to the next client in a linked list, forming a simple client list.

### Global Variables

*   `clients_head`: A pointer to the head of the linked list of connected clients.
*   `clients_mutex`: A `pthread_mutex_t` used to protect the `clients_head` linked list from concurrent access.
*   `history_lock`: A `pthread_rwlock_t` used to protect the SQLite database from concurrent reads and writes.
*   `db`: A pointer to the SQLite database connection.
*   `insert_stmt`: A pointer to a prepared SQLite statement for inserting messages into the database.
*   `select_stmt`: A pointer to a prepared SQLite statement for selecting messages from the database.

### Client Management Functions

These functions handle the lifecycle of a client connection, from adding and removing clients to searching for them and inspecting their properties.

#### `void add_client(struct lws *wsi)`

This function is called when a new client establishes a WebSocket connection. It allocates memory for a new `struct client`, initializes it with default values, and adds it to the `clients_head` linked list.

*   **Parameters:**
    *   `wsi`: The WebSocket instance of the new client.

#### `void remove_client(struct lws *wsi)`

This function is called when a client disconnects. It iterates through the `clients_head` linked list, finds the client with the matching `wsi`, and removes it from the list, freeing the associated memory.

*   **Parameters:**
    *   `wsi`: The WebSocket instance of the disconnected client.

#### `struct client* find_client(struct lws *wsi)`

This function searches for a client in the `clients_head` linked list by their `wsi`.

*   **Parameters:**
    *   `wsi`: The WebSocket instance of the client to find.
*   **Returns:** A pointer to the `struct client` if found, or `NULL` otherwise.

#### `void count_roles(int *readers, int *writers)`

This function counts the number of connected clients with the "READER" and "WRITER" roles.

*   **Parameters:**
    *   `readers`: A pointer to an integer where the number of readers will be stored.
    *   `writers`: A pointer to an integer where the number of writers will be stored.

#### `struct lws **collect_wsis(int *out_count)`

This function collects the WebSocket instances (`wsi`) of all connected clients into a dynamically allocated array.

*   **Parameters:**
    *   `out_count`: A pointer to an integer where the number of collected `wsi`s will be stored.
*   **Returns:** A dynamically allocated array of `struct lws *` pointers, which must be freed by the caller.

### Database Interaction Functions

These functions manage the SQLite database, from initialization and closing to inserting and retrieving chat messages.

#### `int init_db(const char *filename)`

This function initializes the SQLite database. It opens the database file, creates the `messages` table if it doesn't exist, and prepares the `insert_stmt` and `select_stmt` for later use.

*   **Parameters:**
    *   `filename`: The name of the SQLite database file.
*   **Returns:** `0` on success, or `-1` on failure.

#### `void close_db()`

This function finalizes the prepared SQLite statements and closes the database connection.

#### `int db_insert_message(const char *username, const char *message)`

This function inserts a new message into the `messages` table.

*   **Parameters:**
    *   `username`: The username of the sender.
    *   `message`: The content of the message.
*   **Returns:** `0` on success, or `-1` on failure.

#### `char *db_get_history_snapshot(int limit)`

This function retrieves a snapshot of the most recent chat messages from the database.

*   **Parameters:**
    *   `limit`: The maximum number of messages to retrieve.
*   **Returns:** A dynamically allocated string containing the chat history, with each message on a new line. The caller must free this string.

### WebSocket Communication Functions

These functions handle the sending of messages to clients over WebSockets.

#### `void broadcast_text(const char *message)`

This function sends a text message to all connected clients.

*   **Parameters:**
    *   `message`: The message to broadcast.

#### `void send_to_client(struct client *c, const char *msg)`

This function sends a text message to a single, specific client.

*   **Parameters:**
    *   `c`: A pointer to the `struct client` to send the message to.
    *   `msg`: The message to send.

### Role Management Logic

These functions enforce the server's role-based access control rules.

#### `int active_readers()`

This function returns the number of currently active readers.

*   **Returns:** The number of clients with the "READER" role.

#### `int active_writers()`

This function returns the number of currently active writers.

*   **Returns:** The number of clients with the "WRITER" role.

#### `int can_admit_as_reader()`

This function checks if a new client can be admitted as a reader. A client can be admitted as a reader only if there are no active writers.

*   **Returns:** `1` if a reader can be admitted, `0` otherwise.

#### `int can_admit_as_writer()`

This function checks if a new client can be admitted as a writer. A client can be admitted as a writer only if there are no other active writers and no active readers.

*   **Returns:** `1` if a writer can be admitted, `0` otherwise.

### WebSocket Event Handling

#### `void handle_message(struct lws *wsi, void *in, size_t len)`

This function is the central message handler for the WebSocket server. It receives all incoming messages and delegates them to the appropriate processing function based on their content.

*   **Parameters:**
    *   `wsi`: The WebSocket instance of the client that sent the message.
    *   `in`: A pointer to the incoming message data.
    *   `len`: The length of the incoming message data.

#### `void process_username_message(struct client *c, char *msg)`

This function processes a "username" message from a client, updating the client's username.

*   **Parameters:**
    *   `c`: A pointer to the `struct client`.
    *   `msg`: The incoming message.

#### `void process_role_message(struct client *c, char *msg)`

This function processes a "role" message from a client, assigning the client a role if the server's rules permit it.

*   **Parameters:**
    *   `c`: A pointer to the `struct client`.
    *   `msg`: The incoming message.

#### `void process_history_request(struct client *c)`

This function processes a "get_history" request from a client, sending them a snapshot of the chat history.

*   **Parameters:**
    *   `c`: A pointer to the `struct client`.

#### `void process_chat_message(struct client *c, char *msg)`

This function processes a chat message from a client, broadcasting it to all other clients if the client has the "WRITER" role.

*   **Parameters:**
    *   `c`: A pointer to the `struct client`.
    *   `msg`: The incoming message.

#### `void handle_disconnection(struct lws *wsi)`

This function handles a client disconnection event, removing the client from the client list and notifying other clients if the disconnected client was a writer.

*   **Parameters:**
    *   `wsi`: The WebSocket instance of the disconnected client.

#### `int ws_callback(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len)`

This is the main `libwebsockets` callback function that handles all WebSocket events. It is the heart of the server's logic, processing everything from new connections and disconnections to incoming messages.

*   **Parameters:**
    *   `wsi`: The WebSocket instance associated with the event.
    *   `reason`: The reason for the callback (e.g., `LWS_CALLBACK_ESTABLISHED`, `LWS_CALLBACK_RECEIVE`, `LWS_CALLBACK_CLOSED`).
    *   `user`: A user-supplied pointer (not used in this server).
    *   `in`: A pointer to the incoming data.
    *   `len`: The length of the incoming data.
*   **Returns:** `0` on success.

### Main Function and Server Setup

#### `int main(int argc, char **argv)`

This is the entry point of the server application. It performs the following steps:

1.  Parses the command-line arguments to get the database file name.
2.  Initializes the database by calling `init_db()`.
3.  Sets up the `libwebsockets` context and protocol.
4.  Starts the `libwebsockets` event loop.
5.  Cleans up by calling `lws_context_destroy()` and `close_db()` when the server exits.

## Client-Side Implementation (`index.html`)

The client-side implementation is a single HTML file (`index.html`) that contains the HTML structure, CSS styling, and JavaScript logic for the chat client.

### HTML Structure

The HTML structure consists of the following main elements:

*   **A modal for joining the chat:** This modal prompts the user to enter a username and select a role (Reader or Writer).
*   **A chat container:** This is where the chat messages are displayed.
*   **An input group:** This contains a textarea for typing messages and a "Send" button.
*   **A status bar:** This displays the connection status and the number of connected readers and writers.
*   **A "Download Chat" button:** This allows the user to download the chat history as a text file.

### JavaScript Logic

The JavaScript code handles all client-side functionality, including:

*   **WebSocket Connection:** It establishes a WebSocket connection to the server at `ws://localhost:8080/chat-protocol`.
*   **User Authentication:** It sends the user's chosen username and role to the server upon connection.
*   **Message Handling:** It handles incoming messages from the server, parsing them and displaying them in the chat container. It also handles system messages, such as role confirmations and denials.
*   **Sending Messages:** It sends messages to the server when the user clicks the "Send" button.
*   **UI Updates:** It updates the UI based on the connection status, the user's role, and the number of connected clients.
*   **Chat History Download:** It allows the user to download the chat history as a text file.

## Building and Running

### Dependencies

To build and run the server, you will need the following dependencies:

*   `libwebsockets-dev` (or equivalent for your OS)
*   `libsqlite3-dev` (or equivalent for your OS)
*   `gcc` or `clang`
*   `pkg-config`

### Build Command

```bash
cd oserveroserver
gcc server.c -o server $(pkg-config --cflags --libs libwebsockets sqlite3)
```

### Running the Server

```bash
./server [database_file.sqlite]
```

If no database file is specified, it defaults to `chat_history.sqlite`.

## Using the Client

To use the client, open the `oserveroserver/index.html` file in a web browser. You will be prompted to enter a username and select a role (Reader or Writer) before connecting.
