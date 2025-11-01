#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sqlite3.h>
#include <libwebsockets.h>
#include <time.h>
#include <ctype.h>

#define PORT 8080
#define MAX_NAME_LEN 64
#define MAX_ROLE_LEN 16
#define MAX_MSG_LEN 4096
#define HISTORY_LIMIT 500

struct client {
    struct lws *wsi;
    char username[MAX_NAME_LEN];
    char role[MAX_ROLE_LEN]; // "READER", "WRITER" or "NONE"
    struct client *next;
};

static struct client *clients_head = NULL;
static pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_rwlock_t history_lock = PTHREAD_RWLOCK_INITIALIZER;

static sqlite3 *db = NULL;
static sqlite3_stmt *insert_stmt = NULL;
static sqlite3_stmt *select_stmt = NULL;

static void broadcast_text(const char *message);

static void add_client(struct lws *wsi) {
    struct client *c = calloc(1, sizeof(struct client));
    if (!c) return;
    c->wsi = wsi;
    snprintf(c->username, MAX_NAME_LEN, "Anonymous");
    snprintf(c->role, MAX_ROLE_LEN, "NONE"); // No role until set
    pthread_mutex_lock(&clients_mutex);
    c->next = clients_head;
    clients_head = c;
    pthread_mutex_unlock(&clients_mutex);
}

static void remove_client(struct lws *wsi) {
    pthread_mutex_lock(&clients_mutex);
    struct client **p = &clients_head;
    while (*p) {
        if ((*p)->wsi == wsi) {
            struct client *tofree = *p;
            *p = tofree->next;
            free(tofree);
            break;
        }
        p = &(*p)->next;
    }
    pthread_mutex_unlock(&clients_mutex);
}

static struct client* find_client(struct lws *wsi) {
    struct client *found = NULL;
    pthread_mutex_lock(&clients_mutex);
    struct client *p = clients_head;
    while (p) {
        if (p->wsi == wsi) {
            found = p;
            break;
        }
        p = p->next;
    }
    pthread_mutex_unlock(&clients_mutex);
    return found;
}

static void count_roles(int *readers, int *writers) {
    int r=0, w=0;
    pthread_mutex_lock(&clients_mutex);
    struct client *p = clients_head;
    while (p) {
        if (strcasecmp(p->role, "WRITER") == 0) w++;
        else if (strcasecmp(p->role, "READER") == 0) r++;
        p = p->next;
    }
    pthread_mutex_unlock(&clients_mutex);
    if (readers) *readers = r;
    if (writers) *writers = w;
}

static struct lws **collect_wsis(int *out_count) {
    *out_count = 0;
    pthread_mutex_lock(&clients_mutex);
    int count = 0;
    struct client *p = clients_head;
    while (p) { count++; p = p->next; }
    if (count == 0) { pthread_mutex_unlock(&clients_mutex); return NULL; }
    struct lws **arr = malloc(sizeof(struct lws *) * count);
    if (!arr) { pthread_mutex_unlock(&clients_mutex); return NULL; }
    int i = 0;
    p = clients_head;
    while (p) {
        arr[i++] = p->wsi;
        p = p->next;
    }
    pthread_mutex_unlock(&clients_mutex);
    *out_count = count;
    return arr;
}

static int init_db(const char *filename) {
    int rc = sqlite3_open(filename, &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Cannot open sqlite db '%s': %s\n", filename, sqlite3_errmsg(db));
        return -1;
    }
    char *errmsg = NULL;
    rc = sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Warning: failed to set WAL mode: %s\n", errmsg ? errmsg : "unknown");
        sqlite3_free(errmsg);
    }
    const char *create_sql =
        "CREATE TABLE IF NOT EXISTS messages ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "username TEXT NOT NULL, "
        "message TEXT NOT NULL, "
        "ts DATETIME DEFAULT (strftime('%Y-%m-%d %H:%M:%f','now'))"
        ");";
    rc = sqlite3_exec(db, create_sql, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Failed to create table: %s\n", errmsg ? errmsg : "unknown");
        sqlite3_free(errmsg);
        sqlite3_close(db);
        db = NULL;
        return -1;
    }

    // ADD THIS: clear the history table so history only persists per run
    rc = sqlite3_exec(db, "DELETE FROM messages;", NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Failed to clear table: %s\n", errmsg ? errmsg : "unknown");
        sqlite3_free(errmsg);
        // it's ok to keep going
    }

    const char *insert_sql = "INSERT INTO messages (username, message) VALUES (?, ?);";
    rc = sqlite3_prepare_v2(db, insert_sql, -1, &insert_stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Failed to prepare insert stmt: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        db = NULL;
        return -1;
    }
    const char *select_sql =
        "SELECT username, message, ts FROM messages "
        "ORDER BY id DESC LIMIT ?;";
    rc = sqlite3_prepare_v2(db, select_sql, -1, &select_stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Failed to prepare select stmt: %s\n", sqlite3_errmsg(db));
        sqlite3_finalize(insert_stmt);
        insert_stmt = NULL;
        sqlite3_close(db);
        db = NULL;
        return -1;
    }
    return 0;
}


static void close_db() {
    if (insert_stmt) { sqlite3_finalize(insert_stmt); insert_stmt = NULL; }
    if (select_stmt) { sqlite3_finalize(select_stmt); select_stmt = NULL; }
    if (db) { sqlite3_close(db); db = NULL; }
}

static int db_insert_message(const char *username, const char *message) {
    if (!db || !insert_stmt) return -1;
    int rc;
    sqlite3_reset(insert_stmt);
    sqlite3_clear_bindings(insert_stmt);
    rc = sqlite3_bind_text(insert_stmt, 1, username ? username : "Anonymous", -1, SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) return -1;
    rc = sqlite3_bind_text(insert_stmt, 2, message ? message : "", -1, SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) return -1;
    rc = sqlite3_step(insert_stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "Failed to insert message: %s\n", sqlite3_errmsg(db));
        return -1;
    }
    return 0;
}

static char *db_get_history_snapshot(int limit) {
    if (!db || !select_stmt) return NULL;
    int rc;
    sqlite3_reset(select_stmt);
    sqlite3_clear_bindings(select_stmt);
    rc = sqlite3_bind_int(select_stmt, 1, limit);
    if (rc != SQLITE_OK) return NULL;
    size_t arr_cap = 64;
    size_t arr_len = 0;
    char **rows = malloc(sizeof(char*) * arr_cap);
    if (!rows) return NULL;
    while ((rc = sqlite3_step(select_stmt)) == SQLITE_ROW) {
        const unsigned char *uname = sqlite3_column_text(select_stmt, 0);
        const unsigned char *msg = sqlite3_column_text(select_stmt, 1);
        // Removed timestamp as requested
        const char *u = uname ? (const char*)uname : "Anonymous";
        const char *m = msg ? (const char*)msg : "";
        size_t needed = strlen(u) + 2 + strlen(m) + 1;
        char *line = malloc(needed);
        if (!line) continue;
        snprintf(line, needed, "%s: %s", u, m);
        if (arr_len + 1 > arr_cap) {
            arr_cap *= 2;
            char **tmp = realloc(rows, sizeof(char*) * arr_cap);
            if (!tmp) break;
            rows = tmp;
        }
        rows[arr_len++] = line;
    }
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {}
    size_t total = 0;
    for (size_t i = 0; i < arr_len; ++i) total += strlen(rows[i]) + 1;
    char *out = malloc(total + 1);
    if (!out) {
        for (size_t i=0;i<arr_len;i++) free(rows[i]);
        free(rows);
        return NULL;
    }
    out[0] = '\0';
    for (ssize_t i = arr_len - 1; i >= 0; --i) {
        strcat(out, rows[i]);
        if (i > 0) strcat(out, "\n");
    }
    for (size_t i=0;i<arr_len;i++) free(rows[i]);
    free(rows);
    return out;
}

static void broadcast_counts() {
    int readers=0, writers=0;
    count_roles(&readers, &writers);
    char buf[128];
    snprintf(buf, sizeof(buf), "SYSTEM_COUNTS:%d:%d", readers, writers);
    broadcast_text(buf);
}

static void send_to_client(struct client *c, const char *msg) {
    if (!c || !c->wsi || !msg) return;
    size_t msg_len = strlen(msg);
    unsigned char *buf = malloc(LWS_PRE + msg_len);
    if (!buf) return;
    memcpy(buf + LWS_PRE, msg, msg_len);
    lws_write(c->wsi, buf + LWS_PRE, msg_len, LWS_WRITE_TEXT);
    free(buf);
}

static void broadcast_text(const char *message) {
    if (!message) return;
    size_t msg_len = strlen(message);
    unsigned char *buf = malloc(LWS_PRE + msg_len);
    if (!buf) return;
    memcpy(buf + LWS_PRE, message, msg_len);
    int count = 0;
    struct lws **wsis = collect_wsis(&count);
    for (int i = 0; i < count; i++) {
        struct lws *wsi = wsis[i];
        if (wsi) {
            lws_write(wsi, buf + LWS_PRE, msg_len, LWS_WRITE_TEXT);
        }
    }
    free(wsis);
    free(buf);
}

static int active_readers() {
    int r=0,w=0;
    count_roles(&r, &w);
    return r;
}

static int active_writers() {
    int r=0,w=0;
    count_roles(&r, &w);
    return w;
}

static int can_admit_as_reader() {
    return active_writers() == 0;
}

static int can_admit_as_writer() {
    return active_writers() == 0 && active_readers() == 0;
}

static int ws_callback(struct lws *wsi, enum lws_callback_reasons reason,
                       void *user, void *in, size_t len) {
    (void)user;
    switch (reason) {
        case LWS_CALLBACK_ESTABLISHED: {
            add_client(wsi);
            break;
        }
        case LWS_CALLBACK_RECEIVE: {
            char *msg = malloc(len + 1);
            if (!msg) break;
            memcpy(msg, in, len);
            msg[len] = '\0';
            struct client *c = find_client(wsi);
            if (!c) { free(msg); break; }

            if (strncmp(msg, "username:", 9) == 0) {
                char *uname = msg + 9;
                while (*uname == ' ' || *uname == '\t') uname++;
                snprintf(c->username, MAX_NAME_LEN, "%s", uname);
            } else if (strncmp(msg, "role:", 5) == 0) {
                char *r = msg + 5;
                while (*r == ' ' || *r == '\t') r++;
                if (strcasecmp(r, "WRITER") == 0) {
                    if (can_admit_as_writer()) {
                        snprintf(c->role, MAX_ROLE_LEN, "WRITER");
                        // Send history BEFORE confirming role
                        pthread_rwlock_rdlock(&history_lock);
                        char *snap = db_get_history_snapshot(HISTORY_LIMIT);
                        pthread_rwlock_unlock(&history_lock);
                        if (snap) {
                            send_to_client(c, snap);
                            free(snap);
                        }
                        // Confirm role
                        send_to_client(c, "ROLE_CONFIRMED:writer");
                        char sysmsg[200];
                        snprintf(sysmsg, sizeof(sysmsg), "System: %s joined as Writer", c->username);
                        broadcast_text(sysmsg);
                    } else {
                        send_to_client(c, "ROLE_DENIED:A writer or readers are already inside.");
                    }
                } else {
                    if (can_admit_as_reader()) {
                        snprintf(c->role, MAX_ROLE_LEN, "READER");
                        pthread_rwlock_rdlock(&history_lock);
                        char *snap = db_get_history_snapshot(HISTORY_LIMIT);
                        pthread_rwlock_unlock(&history_lock);
                        if (snap) {
                            send_to_client(c, snap);
                            free(snap);
                        }
                        send_to_client(c, "ROLE_CONFIRMED:reader");
                        char sysmsg[200];
                        snprintf(sysmsg, sizeof(sysmsg), "System: %s joined as Reader", c->username);
                        broadcast_text(sysmsg);
                    } else {
                        send_to_client(c, "ROLE_DENIED:A writer is already inside.");
                    }
                }
                broadcast_counts();
            } else if (strncmp(msg, "get_history", 11) == 0) {
                pthread_rwlock_rdlock(&history_lock);
                char *snap = db_get_history_snapshot(HISTORY_LIMIT);
                pthread_rwlock_unlock(&history_lock);
                if (snap) {
                    send_to_client(c, snap);
                    free(snap);
                } else {
                    send_to_client(c, "");
                }
            } else {
                if (strcasecmp(c->role, "WRITER") != 0) {
                    char err[200];
                    snprintf(err, sizeof(err), "System: You are a READER — you cannot send messages.");
                    send_to_client(c, err);
                } else {
                    char out[MAX_MSG_LEN];

                    // ✅ Removed timestamp here
                    snprintf(out, sizeof(out), "%s: %s",
                             c->username[0] ? c->username : "Anon", msg);

                    pthread_rwlock_wrlock(&history_lock);
                    if (db_insert_message(c->username, msg) != 0) {
                        fprintf(stderr, "Warning: failed to insert message into DB\n");
                    }
                    pthread_rwlock_unlock(&history_lock);

                    broadcast_text(out);
                }
            }
            free(msg);
            break;
        }
        case LWS_CALLBACK_CLOSED: {
            struct client *c = find_client(wsi);
            if (c && strcasecmp(c->role, "WRITER") == 0) {
                char sysmsg[200];
                snprintf(sysmsg, sizeof(sysmsg), "System: %s disconnected.", c->username);
                remove_client(wsi);
                broadcast_text(sysmsg);
                broadcast_counts();
            } else {
                remove_client(wsi);
                broadcast_counts();
            }
            break;
        }
        default:
            break;
    }
    return 0;
}




static const struct lws_protocols protocols[] = {
    {
        "chat-protocol",
        ws_callback,
        0,
        4096,
    },
    { NULL, NULL, 0, 0 }
};

int main(int argc, char **argv) {
    const char *dbfile = "chat_history.sqlite";
    if (argc > 1) dbfile = argv[1];
    if (init_db(dbfile) != 0) {
        fprintf(stderr, "Failed to initialize database. Exiting.\n");
        return 1;
    }
    struct lws_context_creation_info info;
    struct lws_context *context;
    memset(&info, 0, sizeof(info));
    info.port = PORT;
    info.protocols = protocols;
    info.gid = -1;
    info.uid = -1;

    context = lws_create_context(&info);
    if (!context) {
        fprintf(stderr, "lws init failed\n");
        close_db();
        return 1;
    }
    printf("Broadcast server (SQLite-backed) started on :%d\n", PORT);
    printf("DB file: %s\n", dbfile);
    printf("Waiting for connections...\n");
    int n = 0;
    while (n >= 0) {
        n = lws_service(context, 1000);
    }
    lws_context_destroy(context);
    close_db();
    return 0;
}
