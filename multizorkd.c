/**
 * MojoZork; a simple, just-for-fun implementation of Infocom's Z-Machine.
 *
 * Please see the file LICENSE.txt in the source's root directory.
 *
 *  This file written by Ryan C. Gordon.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <fcntl.h>
#include <setjmp.h>
#include <assert.h>

#include "sqlite3.h"

#define MULTIZORK 1
#include "mojozork.c"

#define MULTIZORKD_VERSION "0.0.8"
#define MULTIZORKD_DEFAULT_PORT 23  /* telnet! */
#define MULTIZORKD_DEFAULT_BACKLOG 64
#define MULTIZORKD_DEFAULT_EGID 0
#define MULTIZORKD_DEFAULT_EUID 0
#define MULTIZORK_TRANSCRIPT_BASEURL "https://multizork.icculus.org"
#define MULTIZORK_BLOCKED_TIMEOUT (60 * 60 * 24)  /* 24 hours in seconds */
#define MULTIZORK_AUTOSAVE_EVERY_X_MOVES 30

typedef unsigned int uint;  // for cleaner printf casting.

// the "_t" drives me nuts.  :/
typedef uint8_t uint8;
typedef int8_t sint8;
typedef uint16_t uint16;
typedef int16_t sint16;
typedef uint32_t uint32;
typedef int32_t sint32;
typedef uint64_t uint64;
typedef int64_t sint64;
typedef size_t uintptr;

#define ARRAYSIZE(x) ( (sizeof (x)) / (sizeof ((x)[0])) )

static time_t GNow = 0;
static const char *GOriginalStoryName = NULL;
static uint8 *GOriginalStory = NULL;
static uint32 GOriginalStoryLen = 0;

static void loginfo(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("multizorkd: ");
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

#if defined(__GNUC__) || defined(__clang__)
static void panic(const char *fmt, ...) __attribute__((noreturn));
#endif
static void panic(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("multizorkd: ");
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
    exit(1);
}

typedef struct Connection Connection;

typedef enum ConnectionState
{
    CONNSTATE_READY,
    CONNSTATE_DRAINING,
    CONNSTATE_CLOSING,
    // never happens, we destroy the object once we hit this state. CONNSTATE_DISCONNECTED
} ConnectionState;

#define MULTIPLAYER_PROP_DATALEN 32  // ZORK 1 SPECIFIC MAGIC: other games (or longer player names) might need more.
typedef struct Player
{
    Connection *connection;  // null if user is disconnected; player lives on.
    sqlite3_int64 dbid;
    char username[16];
    char hash[8];
    uint32 next_logical_pc;  // next step_instance() should run this player from this z-machine program counter.
    uint32 next_logical_sp;  // next step_instance() should run this player from this z-machine stack pointer.
    uint16 next_logical_bp;  // next step_instance() should run this player from this z-machine base pointer.
    uint16 stack[2048];      // Copy of the stack to restore for next step_instance()  // !!! FIXME: make this dynamic?
    uint8 *next_inputbuf;   // where to write the next input for this player.
    uint8 next_inputbuflen;
    uint16 next_operands[2];  // to save off the READ operands for later.
    char againbuf[128];
    uint8 object_table_data[9];
    uint8 property_table_data[MULTIPLAYER_PROP_DATALEN];
    // ZORK 1 SPECIFIC MAGIC: track the TOUCHBIT for each room per-player, so they all get descriptions on their first visit.
    uint8 touchbits[32];
    // ZORK 1 SPECIFIC MAGIC: these are player-specific globals we need to manage.
    uint16 gvar_location;
    uint16 gvar_coffin_held;
    uint16 gvar_dead;
    uint16 gvar_deaths;
    uint16 gvar_lit;
    uint16 gvar_alwayslit;
    uint16 gvar_verbose;
    uint16 gvar_superbrief;
    uint16 gvar_lucky;
    uint16 gvar_loadallowed;
    // !!! FIXME: several more, probably.
    int game_over;
} Player;

typedef struct Instance
{
    ZMachineState zmachine_state;
    sqlite3_int64 dbid;
    int started;
    char hash[8];
    Player players[4];
    int num_players;
    int current_player;   //  the player we're currently running the z-machine for.
    time_t savetime;
    int moves_since_last_save;
    sqlite3_int64 crashed;
    jmp_buf jmpbuf;
} Instance;

typedef void (*InputFn)(Connection *conn, const char *str);
struct Connection
{
    int sock;
    ConnectionState state;
    InputFn inputfn;
    Instance *instance;
    char address[64];
    char username[16];
    char inputbuf[128];
    uint32 inputbuf_used;
    int overlong_input;
    char *outputbuf;
    uint32 outputbuf_len;
    uint32 outputbuf_used;
    time_t last_activity;
    int blocked;
};

static Connection **connections = NULL;
static size_t num_connections = 0;



#define MULTIZORK_DATABASE_PATH "multizork.sqlite3"

#define SQL_CREATE_TABLES \
    "create table if not exists instances (" \
    " id integer primary key," \
    " hashid text not null unique," \
    " num_players integer unsigned not null," \
    " starttime integer unsigned not null," \
    " savetime integer unsigned not null," \
    " instructions_run integer unsigned not null," \
    " dynamic_memory blob not null," \
    " story_filename text not null," \
    " crashed integer not null default 0" \
    ");" \
    " " \
    "create index if not exists instance_index on instances (hashid);" \
    " " \
    "create table if not exists players (" \
    " id integer primary key," \
    " hashid text not null unique," \
    " instance integer not null," \
    " username text not null," \
    " next_logical_pc integer unsigned not null," \
    " next_logical_sp integer unsigned not null," \
    " next_logical_bp integer unsigned not null," \
    " next_logical_inputbuf integer unsigned not null," \
    " next_logical_inputbuflen integer unsigned not null," \
    " next_operands_1 integer unsigned not null," \
    " next_operands_2 integer unsigned not null," \
    " againbuf text not null," \
    " stack blob not null," \
    " object_table_data blob not null," \
    " property_table_data blob not null," \
    " touchbits blob not null," \
    " gvar_location integer unsigned not null," \
    " gvar_coffin_held integer unsigned not null," \
    " gvar_dead integer unsigned not null," \
    " gvar_deaths integer unsigned not null," \
    " gvar_lit integer unsigned not null," \
    " gvar_alwayslit integer unsigned not null," \
    " gvar_verbose integer unsigned not null," \
    " gvar_superbrief integer unsigned not null," \
    " gvar_lucky integer unsigned not null," \
    " gvar_loadallowed integer unsigned not null," \
    /* !!! FIXME: several more, probably. */ \
    " game_over integer not null default 0" \
    ");" \
    " " \
    "create index if not exists players_index on players (hashid);" \
    " " \
    "create table if not exists transcripts (" \
    " id integer primary key," \
    " timestamp integer unsigned not null," \
    " player integer not null," \
    " texttype integer not null," \
    " content text not null" \
    ");" \
    " " \
    "create index if not exists transcript_index on transcripts (player);" \
    " " \
    "create table if not exists used_hashes (" \
    " hashid text not null unique" \
    ");" \
    " " \
    "create index if not exists used_hashes_index on used_hashes (hashid);" \
    " " \
    "create table if not exists crashes (" \
    " id integer primary key," \
    " instance integer not null," \
    " timestamp integer unsigned not null," \
    " current_player integer unsigned not null," \
    " logical_pc integer unsigned not null," \
    " errstr text not null" \
    ");" \
    " " \
    "create table if not exists blocked (" \
    " id integer primary key," \
    " address text not null," \
    " timestamp integer unsigned not null" \
    ");" \
    " " \
    "create index if not exists blocked_index on blocked (address);" \


#define SQL_TRANSCRIPT_INSERT \
    "insert into transcripts (timestamp, player, texttype, content) values ($timestamp, $player, $texttype, $content);"

#define SQL_USED_HASH_INSERT \
    "insert into used_hashes (hashid) values ($hashid);"

#define SQL_INSTANCE_INSERT \
    "insert into instances (hashid, num_players, starttime, savetime, instructions_run, dynamic_memory, story_filename)" \
    " values ($hashid, $num_players, $starttime, $savetime, $instructions_run, $dynamic_memory, $story_filename);"

#define SQL_INSTANCE_UPDATE \
    "update instances set savetime=$savetime, instructions_run=$instructions_run, crashed=$crashed, dynamic_memory=$dynamic_memory where id=$id limit 1;"

#define SQL_INSTANCE_SELECT \
    "select * from instances where id=$id limit 1;"

#define SQL_PLAYER_INSERT \
    "insert into players (hashid, instance, username, next_logical_pc, next_logical_sp, next_logical_bp," \
    " next_logical_inputbuf, next_logical_inputbuflen, next_operands_1, next_operands_2, againbuf, stack," \
    " object_table_data, property_table_data, touchbits, gvar_location, gvar_coffin_held, gvar_dead, gvar_deaths," \
    " gvar_lit, gvar_alwayslit, gvar_verbose, gvar_superbrief, gvar_lucky, gvar_loadallowed, game_over" \
    ") values ($hashid, $instance, $username, $next_logical_pc, $next_logical_sp, $next_logical_bp," \
    " $next_logical_inputbuf, $next_logical_inputbuflen, $next_operands_1, $next_operands_2, $againbuf, $stack," \
    " $object_table_data, $property_table_data, $touchbits, $gvar_location, $gvar_coffin_held, $gvar_dead, $gvar_deaths," \
    " $gvar_lit, $gvar_alwayslit, $gvar_verbose, $gvar_superbrief, $gvar_lucky, $gvar_loadallowed, $game_over);"

#define SQL_PLAYER_UPDATE \
    "update players set" \
    " next_logical_pc = $next_logical_pc, next_logical_sp = $next_logical_sp, next_logical_bp = $next_logical_bp," \
    " next_logical_inputbuf = $next_logical_inputbuf, next_logical_inputbuflen = $next_logical_inputbuflen," \
    " next_operands_1 = $next_operands_1, next_operands_2 = $next_operands_2, againbuf = $againbuf, stack = $stack," \
    " object_table_data = $object_table_data, property_table_data = $property_table_data, touchbits = $touchbits," \
    " gvar_location = $gvar_location, gvar_coffin_held = $gvar_coffin_held, gvar_dead = $gvar_dead," \
    " gvar_deaths = $gvar_deaths, gvar_lit = $gvar_lit, gvar_alwayslit = $gvar_alwayslit, gvar_verbose = $gvar_verbose," \
    " gvar_superbrief = $gvar_superbrief, gvar_lucky = $gvar_lucky, gvar_loadallowed = $gvar_loadallowed, game_over = $game_over" \
    " where id=$id limit 1;"

#define SQL_FIND_INSTANCE_BY_PLAYER_HASH \
    "select instance from players where hashid=$hashid limit 1;"

#define SQL_PLAYERS_SELECT \
    "select * from players where instance=$instance order by id limit $limit;"

#define SQL_RECAP_SELECT \
    "select content from (select id, content from transcripts where player=$player order by id desc limit $limit) order by id;"

#define SQL_CRASH_INSERT \
    "insert into crashes (instance, timestamp, current_player, logical_pc, errstr)" \
    " values ($instance, $timestamp, $current_player, $logical_pc, $errstr);"

#define SQL_BLOCKED_INSERT \
    "insert into blocked (address, timestamp) values ($address, $timestamp);"

#define SQL_BLOCKED_SELECT \
    "select timestamp from blocked where address = $address order by id desc limit 1;"

#define SQL_RECAP_TRIM \
    "delete from transcripts where player = $player and timestamp > $savetime;"


static sqlite3 *GDatabase = NULL;
static sqlite3_stmt *GStmtBegin = NULL;
static sqlite3_stmt *GStmtCommit = NULL;
static sqlite3_stmt *GStmtTranscriptInsert = NULL;
static sqlite3_stmt *GStmtUsedHashInsert = NULL;
static sqlite3_stmt *GStmtInstanceInsert = NULL;
static sqlite3_stmt *GStmtInstanceUpdate = NULL;
static sqlite3_stmt *GStmtInstanceSelect = NULL;
static sqlite3_stmt *GStmtPlayerInsert = NULL;
static sqlite3_stmt *GStmtPlayerUpdate = NULL;
static sqlite3_stmt *GStmtFindInstanceByPlayerHash = NULL;
static sqlite3_stmt *GStmtPlayersSelect = NULL;
static sqlite3_stmt *GStmtRecapSelect = NULL;
static sqlite3_stmt *GStmtCrashInsert = NULL;
static sqlite3_stmt *GStmtBlockedInsert = NULL;
static sqlite3_stmt *GStmtBlockedSelect = NULL;
static sqlite3_stmt *GStmtRecapTrim = NULL;


static void db_log_error(const char *what)
{
    loginfo("DBERROR: failed to %s! (%s)", what, sqlite3_errmsg(GDatabase));
}

static int db_set_transaction(sqlite3_stmt *stmt, const char *what)
{
    if ((sqlite3_reset(stmt) != SQLITE_OK) || (sqlite3_step(stmt) != SQLITE_DONE)) {
        db_log_error(what);
        return 0;
    }
    return 1;
}

static unsigned int db_transaction_count = 0;
static int db_begin_transaction(void)
{
    db_transaction_count++;
    if (db_transaction_count > 1) {
        return 1;
    }
    return db_set_transaction(GStmtBegin, "begin sqlite3 transaction");
}

static int db_end_transaction(void)
{
    assert(db_transaction_count > 0);
    db_transaction_count--;
    if (db_transaction_count > 0) {
        return 1;
    }
    return db_set_transaction(GStmtCommit, "commit sqlite3 transaction");
}

static int find_sql_column_by_name(sqlite3_stmt *stmt, const char *name)
{
    const int total = sqlite3_column_count(stmt);
    for (int i = 0; i < total; i++) {
        const char *colname = sqlite3_column_name(stmt, i);
        if (strcasecmp(colname, name) == 0) {
            return i;
        }
    }
    panic("Asked for unknown column '%s' in SQL statement '%s'!", name, sqlite3_sql(stmt));
    return -1;
}

static int find_sql_bind_by_name(sqlite3_stmt *stmt, const char *name)
{
    char dollarname[64];
    snprintf(dollarname, sizeof (dollarname), "$%s", name);
    const int retval = sqlite3_bind_parameter_index(stmt, dollarname);
    if (retval) {
        return retval;
    }

    panic("Asked for unknown bind '%s' in SQL statement '%s'!", name, sqlite3_sql(stmt));
    return 0;
}

#define SQLBINDINT(stmt, name, val) (sqlite3_bind_int((stmt), find_sql_bind_by_name((stmt), (name)), (val)))
#define SQLBINDINT64(stmt, name, val) (sqlite3_bind_int64((stmt), find_sql_bind_by_name((stmt), (name)), (val)))
#define SQLBINDTEXT(stmt, name, val) (sqlite3_bind_text((stmt), find_sql_bind_by_name((stmt), (name)), (val), -1, SQLITE_TRANSIENT))
#define SQLBINDBLOB(stmt, name, val, len) (sqlite3_bind_blob((stmt), find_sql_bind_by_name((stmt), (name)), (val), len, SQLITE_TRANSIENT))
#define SQLCOLUMN(typ, stmt, name) (sqlite3_column_##typ((stmt), find_sql_column_by_name((stmt), (name))))

typedef enum TranscriptTextType
{
    TT_GAME_OUTPUT = 0,
    TT_PLAYER_INPUT,
    TT_SYSTEM_MESSAGE
} TranscriptTextType;


static sqlite3_int64 db_insert_transcript(const sqlite3_int64 player_dbid, const TranscriptTextType texttype, const char *content)
{
    //"insert into transcripts (timestamp, player, texttype, content) values ($timestamp, $player, $texttype, $content);"
    const sqlite3_int64 retval =
           ( (sqlite3_reset(GStmtTranscriptInsert) == SQLITE_OK) &&
             (SQLBINDINT64(GStmtTranscriptInsert, "timestamp", (sqlite3_int64) GNow) == SQLITE_OK) &&
             (SQLBINDINT64(GStmtTranscriptInsert, "player", player_dbid) == SQLITE_OK) &&
             (SQLBINDINT(GStmtTranscriptInsert, "texttype", (int) texttype) == SQLITE_OK) &&
             (SQLBINDTEXT(GStmtTranscriptInsert, "content", content) == SQLITE_OK) &&
             (sqlite3_step(GStmtTranscriptInsert) == SQLITE_DONE) ) ? sqlite3_last_insert_rowid(GDatabase) : 0;
    if (!retval) { db_log_error("insert transcript"); }
    return retval;
}

static sqlite3_int64 db_insert_used_hash(const char *hashid, int *_notunique)
{
    //"insert into used_hashes (hashid) values ($hashid);"
    int rc = SQLITE_DONE;
    const sqlite3_int64 retval =
           ( (sqlite3_reset(GStmtUsedHashInsert) == SQLITE_OK) &&
             (SQLBINDTEXT(GStmtUsedHashInsert, "hashid", hashid) == SQLITE_OK) &&
             ((rc = sqlite3_step(GStmtUsedHashInsert)) == SQLITE_DONE) ) ? sqlite3_last_insert_rowid(GDatabase) : 0;

    *_notunique = (rc == SQLITE_CONSTRAINT);

    if ((!retval) && (!*_notunique)) {
        db_log_error("insert used hash");
    }
    return retval;
}

static sqlite3_int64 db_insert_instance(const Instance *inst)
{
    //"insert into instances (hashid, num_players, starttime, savetime, instructions_run, dynamic_memory, story_filename)"
    //" values ($hashid, $num_players, $starttime, $savetime, $instructions_run, $dynamic_memory, $story_filename);"
    const sqlite3_int64 retval =
           ( (sqlite3_reset(GStmtInstanceInsert) == SQLITE_OK) &&
             (SQLBINDTEXT(GStmtInstanceInsert, "hashid", inst->hash) == SQLITE_OK) &&
             (SQLBINDINT(GStmtInstanceInsert, "num_players", inst->num_players) == SQLITE_OK) &&
             (SQLBINDINT64(GStmtInstanceInsert, "starttime", (sqlite3_int64) GNow) == SQLITE_OK) &&
             (SQLBINDINT64(GStmtInstanceInsert, "savetime", (sqlite3_int64) GNow) == SQLITE_OK) &&
             (SQLBINDINT64(GStmtInstanceInsert, "instructions_run", (sqlite3_int64) inst->zmachine_state.instructions_run) == SQLITE_OK) &&
             (SQLBINDBLOB(GStmtInstanceInsert, "dynamic_memory", inst->zmachine_state.story, inst->zmachine_state.header.staticmem_addr) == SQLITE_OK) &&
             (SQLBINDTEXT(GStmtInstanceInsert, "story_filename", inst->zmachine_state.story_filename) == SQLITE_OK) &&
             (sqlite3_step(GStmtInstanceInsert) == SQLITE_DONE) ) ? sqlite3_last_insert_rowid(GDatabase) : 0;
    if (!retval) { db_log_error("insert instance"); }
    return retval;
}

static int db_update_instance(const Instance *inst)
{
    //"update instances set savetime=$savetime, instructions_run=$instructions_run, crashed=$crashed, dynamic_memory=$dynamic_memory where id=$id limit 1;"
    assert(inst->dbid != 0);
    const int retval =
           ( (sqlite3_reset(GStmtInstanceUpdate) == SQLITE_OK) &&
             (SQLBINDINT64(GStmtInstanceUpdate, "savetime", (sqlite3_int64) GNow) == SQLITE_OK) &&
             (SQLBINDINT64(GStmtInstanceUpdate, "instructions_run", (sqlite3_int64) inst->zmachine_state.instructions_run) == SQLITE_OK) &&
             (SQLBINDBLOB(GStmtInstanceUpdate, "dynamic_memory", inst->zmachine_state.story, inst->zmachine_state.header.staticmem_addr) == SQLITE_OK) &&
             (SQLBINDINT64(GStmtInstanceUpdate, "crashed", inst->crashed) == SQLITE_OK) &&
             (SQLBINDINT64(GStmtInstanceUpdate, "id", (sqlite3_int64) inst->dbid) == SQLITE_OK) &&
             (sqlite3_step(GStmtInstanceUpdate) == SQLITE_DONE) ) ? 1 : 0;
    if (!retval) { db_log_error("update instance"); }
    return retval;
}

static sqlite3_int64 db_insert_player(const Instance *inst, const int playernum)
{
    //"insert into players (hashid, instance, username, next_logical_pc, next_logical_sp, next_logical_bp,"
    //" next_logical_inputbuf, next_logical_inputbuflen, next_operands_1, next_operands_2, againbuf, stack,"
    //" object_table_data, property_table_data, touchbits, gvar_location, gvar_coffin_held, gvar_dead, gvar_deaths,"
    //" gvar_lit, gvar_alwayslit, gvar_verbose, gvar_superbrief, gvar_lucky, gvar_loadallowed, game_over"
    //") values ($hashid, $instance, $username, $next_logical_pc, $next_logical_sp, $next_logical_bp,"
    //" $next_logical_inputbuf, $next_logical_inputbuflen, $next_operands_1, $next_operands_2, $againbuf, $stack,"
    //" $object_table_data, $property_table_data, $touchbits, $gvar_location, $gvar_coffin_held, $gvar_dead, $gvar_deaths,"
    //" $gvar_lit, $gvar_alwayslit, $gvar_verbose, $gvar_superbrief, $gvar_lucky, $gvar_loadallowed, $game_over);"
    const Player *player = &inst->players[playernum];
    assert(player->dbid == 0);
    
    const sqlite3_int64 retval =
           ( (sqlite3_reset(GStmtPlayerInsert) == SQLITE_OK) &&
             (SQLBINDTEXT(GStmtPlayerInsert, "hashid", player->hash) == SQLITE_OK) &&
             (SQLBINDINT64(GStmtPlayerInsert, "instance", inst->dbid) == SQLITE_OK) &&
             (SQLBINDTEXT(GStmtPlayerInsert, "username", player->username) == SQLITE_OK) &&
             (SQLBINDINT(GStmtPlayerInsert, "next_logical_pc", (int) player->next_logical_pc) == SQLITE_OK) &&
             (SQLBINDINT(GStmtPlayerInsert, "next_logical_sp", (int) player->next_logical_sp) == SQLITE_OK) &&
             (SQLBINDINT(GStmtPlayerInsert, "next_logical_bp", (int) player->next_logical_bp) == SQLITE_OK) &&
             (SQLBINDINT(GStmtPlayerInsert, "next_logical_inputbuf", player->next_inputbuf ? ((int) (player->next_inputbuf - inst->zmachine_state.story)) : 0) == SQLITE_OK) &&
             (SQLBINDINT(GStmtPlayerInsert, "next_logical_inputbuflen", player->next_inputbuflen) == SQLITE_OK) &&
             (SQLBINDINT(GStmtPlayerInsert, "next_operands_1", (int) player->next_operands[0]) == SQLITE_OK) &&
             (SQLBINDINT(GStmtPlayerInsert, "next_operands_2", (int) player->next_operands[1]) == SQLITE_OK) &&
             (SQLBINDTEXT(GStmtPlayerInsert, "againbuf", player->againbuf) == SQLITE_OK) &&
             (SQLBINDBLOB(GStmtPlayerInsert, "stack", player->stack, player->next_logical_sp * 2) == SQLITE_OK) &&
             (SQLBINDBLOB(GStmtPlayerInsert, "object_table_data", player->object_table_data, sizeof (player->object_table_data)) == SQLITE_OK) &&
             (SQLBINDBLOB(GStmtPlayerInsert, "property_table_data", player->property_table_data, sizeof (player->property_table_data)) == SQLITE_OK) &&
             (SQLBINDBLOB(GStmtPlayerInsert, "touchbits", player->touchbits, sizeof (player->touchbits)) == SQLITE_OK) &&
             (SQLBINDINT(GStmtPlayerInsert, "gvar_location", (int) player->gvar_location) == SQLITE_OK) &&
             (SQLBINDINT(GStmtPlayerInsert, "gvar_coffin_held", (int) player->gvar_coffin_held) == SQLITE_OK) &&
             (SQLBINDINT(GStmtPlayerInsert, "gvar_dead", (int) player->gvar_dead) == SQLITE_OK) &&
             (SQLBINDINT(GStmtPlayerInsert, "gvar_deaths", (int) player->gvar_deaths) == SQLITE_OK) &&
             (SQLBINDINT(GStmtPlayerInsert, "gvar_lit", (int) player->gvar_lit) == SQLITE_OK) &&
             (SQLBINDINT(GStmtPlayerInsert, "gvar_alwayslit", (int) player->gvar_alwayslit) == SQLITE_OK) &&
             (SQLBINDINT(GStmtPlayerInsert, "gvar_verbose", (int) player->gvar_verbose) == SQLITE_OK) &&
             (SQLBINDINT(GStmtPlayerInsert, "gvar_superbrief", (int) player->gvar_superbrief) == SQLITE_OK) &&
             (SQLBINDINT(GStmtPlayerInsert, "gvar_lucky", (int) player->gvar_lucky) == SQLITE_OK) &&
             (SQLBINDINT(GStmtPlayerInsert, "gvar_loadallowed", (int) player->gvar_loadallowed) == SQLITE_OK) &&
             (SQLBINDINT(GStmtPlayerInsert, "game_over", player->game_over) == SQLITE_OK) &&
             (sqlite3_step(GStmtPlayerInsert) == SQLITE_DONE) ) ? sqlite3_last_insert_rowid(GDatabase) : 0;
    if (!retval) { db_log_error("insert player"); }
    return retval;
}

static int db_update_player(const Instance *inst, const int playernum)
{
    //"update players set"
    //" next_logical_pc = $next_logical_pc, next_logical_sp = $next_logical_sp, next_logical_bp = $next_logical_bp,"
    //" next_logical_inputbuf = $next_logical_inputbuf, next_logical_inputbuflen = $next_logical_inputbuflen,"
    //" next_operands_1 = $next_operands_1, next_operands_2 = $next_operands_2, againbuf = $againbuf, stack = $stack,"
    //" object_table_data = $object_table_data, property_table_data = $property_table_data, touchbits = $touchbits,"
    //" gvar_location = $gvar_location, gvar_coffin_held = $gvar_coffin_held, gvar_dead = $gvar_dead,"
    //" gvar_deaths = $gvar_deaths, gvar_lit = $gvar_lit, gvar_alwayslit = $gvar_alwayslit, gvar_verbose = $gvar_verbose,"
    //" gvar_superbrief = $gvar_superbrief, gvar_lucky = $gvar_lucky, gvar_loadallowed = $gvar_loadallowed, game_over = $game_over"
    //" where id=$id limit 1;"
    const Player *player = &inst->players[playernum];
    assert(player->dbid != 0);
    const int retval =
           ( (sqlite3_reset(GStmtPlayerUpdate) == SQLITE_OK) &&
             (SQLBINDINT(GStmtPlayerUpdate, "next_logical_pc", (int) player->next_logical_pc) == SQLITE_OK) &&
             (SQLBINDINT(GStmtPlayerUpdate, "next_logical_sp", (int) player->next_logical_sp) == SQLITE_OK) &&
             (SQLBINDINT(GStmtPlayerUpdate, "next_logical_bp", (int) player->next_logical_bp) == SQLITE_OK) &&
             (SQLBINDINT(GStmtPlayerUpdate, "next_logical_inputbuf", player->next_inputbuf ? ((int) (player->next_inputbuf - inst->zmachine_state.story)) : 0) == SQLITE_OK) &&
             (SQLBINDINT(GStmtPlayerUpdate, "next_logical_inputbuflen", player->next_inputbuflen) == SQLITE_OK) &&
             (SQLBINDINT(GStmtPlayerUpdate, "next_operands_1", (int) player->next_operands[0]) == SQLITE_OK) &&
             (SQLBINDINT(GStmtPlayerUpdate, "next_operands_2", (int) player->next_operands[1]) == SQLITE_OK) &&
             (SQLBINDTEXT(GStmtPlayerUpdate, "againbuf", player->againbuf) == SQLITE_OK) &&
             (SQLBINDBLOB(GStmtPlayerUpdate, "stack", player->stack, player->next_logical_sp * 2) == SQLITE_OK) &&
             (SQLBINDBLOB(GStmtPlayerUpdate, "object_table_data", player->object_table_data, sizeof (player->object_table_data)) == SQLITE_OK) &&
             (SQLBINDBLOB(GStmtPlayerUpdate, "property_table_data", player->property_table_data, sizeof (player->property_table_data)) == SQLITE_OK) &&
             (SQLBINDBLOB(GStmtPlayerUpdate, "touchbits", player->touchbits, sizeof (player->touchbits)) == SQLITE_OK) &&
             (SQLBINDINT(GStmtPlayerUpdate, "gvar_location", (int) player->gvar_location) == SQLITE_OK) &&
             (SQLBINDINT(GStmtPlayerUpdate, "gvar_coffin_held", (int) player->gvar_coffin_held) == SQLITE_OK) &&
             (SQLBINDINT(GStmtPlayerUpdate, "gvar_dead", (int) player->gvar_dead) == SQLITE_OK) &&
             (SQLBINDINT(GStmtPlayerUpdate, "gvar_deaths", (int) player->gvar_deaths) == SQLITE_OK) &&
             (SQLBINDINT(GStmtPlayerUpdate, "gvar_lit", (int) player->gvar_lit) == SQLITE_OK) &&
             (SQLBINDINT(GStmtPlayerUpdate, "gvar_alwayslit", (int) player->gvar_alwayslit) == SQLITE_OK) &&
             (SQLBINDINT64(GStmtPlayerUpdate, "gvar_verbose", (int) player->gvar_verbose) == SQLITE_OK) &&
             (SQLBINDINT64(GStmtPlayerUpdate, "gvar_superbrief", (int) player->gvar_superbrief) == SQLITE_OK) &&
             (SQLBINDINT64(GStmtPlayerUpdate, "gvar_lucky", (int) player->gvar_lucky) == SQLITE_OK) &&
             (SQLBINDINT64(GStmtPlayerUpdate, "gvar_loadallowed", (int) player->gvar_loadallowed) == SQLITE_OK) &&
             (SQLBINDINT(GStmtPlayerUpdate, "game_over", (int) player->game_over) == SQLITE_OK) &&
             (SQLBINDINT64(GStmtPlayerUpdate, "id", player->dbid) == SQLITE_OK) &&
             (sqlite3_step(GStmtPlayerUpdate) == SQLITE_DONE) ) ? 1 : 0;
    if (!retval) { db_log_error("update player"); }
    return retval;
}

static sqlite3_int64 db_find_instance_by_player_hash(const char *hashid)
{
    //"select instance from players where hashid=$hashid limit 1;"
    int rc = SQLITE_ERROR;
    if ( (sqlite3_reset(GStmtFindInstanceByPlayerHash) != SQLITE_OK) ||
         (SQLBINDTEXT(GStmtFindInstanceByPlayerHash, "hashid", hashid) != SQLITE_OK) ||
         ((rc = sqlite3_step(GStmtFindInstanceByPlayerHash)) != SQLITE_ROW) ) {
        if (rc != SQLITE_DONE) { db_log_error("select instance by player hash"); }
        return 0;  // error or not found.
    }
    return SQLCOLUMN(int64, GStmtFindInstanceByPlayerHash, "instance");
}

static int db_select_instance(Instance *inst, const sqlite3_int64 dbid)
{
    // this should be a fresh object returned create_instance() that we will update with database info.
    assert(!inst->started);
    assert(!inst->dbid);

    //"select * from instances where id=$id limit 1;"
    int rc = SQLITE_ERROR;
    if ( (sqlite3_reset(GStmtInstanceSelect) != SQLITE_OK) ||
         (SQLBINDINT64(GStmtInstanceSelect, "id", dbid) != SQLITE_OK) ||
         ((rc = sqlite3_step(GStmtInstanceSelect)) != SQLITE_ROW) ) {
        if (rc != SQLITE_DONE) { db_log_error("select instance"); }
        return 0;  // error or not found.
    }

    inst->dbid = dbid;
    snprintf(inst->hash, sizeof (inst->hash), "%s", SQLCOLUMN(text, GStmtInstanceSelect, "hashid"));
    inst->num_players = SQLCOLUMN(int, GStmtInstanceSelect, "num_players");
    inst->savetime = (time_t) SQLCOLUMN(int64, GStmtInstanceSelect, "savetime");
    inst->crashed = SQLCOLUMN(int64, GStmtInstanceSelect, "crashed");
    inst->zmachine_state.instructions_run = (uint32) SQLCOLUMN(int64, GStmtInstanceSelect, "instructions_run");
    const void *dynmem = SQLCOLUMN(blob, GStmtInstanceSelect, "dynamic_memory");
    size_t dynmemlen = SQLCOLUMN(bytes, GStmtInstanceSelect, "dynamic_memory");
    assert(dynmemlen == ((size_t) inst->zmachine_state.header.staticmem_addr));
    if ( ((size_t) inst->zmachine_state.header.staticmem_addr) < dynmemlen ) {
        dynmemlen = (size_t) inst->zmachine_state.header.staticmem_addr;
    }
    memcpy(inst->zmachine_state.story, dynmem, dynmemlen);
    sqlite3_reset(GStmtInstanceSelect);

    //"select * from players where instance=$instance order by id limit $limit;"
    if ( (sqlite3_reset(GStmtPlayersSelect) != SQLITE_OK) ||
         (SQLBINDINT64(GStmtPlayersSelect, "instance", dbid) != SQLITE_OK) ||
         (SQLBINDINT(GStmtPlayersSelect, "limit", inst->num_players) != SQLITE_OK) ) {
        db_log_error("select players");
        return 0;
    }

    int num_players = 0;
    while (sqlite3_step(GStmtPlayersSelect) == SQLITE_ROW) {
        assert(num_players < inst->num_players);
        Player *player = &inst->players[num_players];
        player->connection = NULL;
        player->dbid = SQLCOLUMN(int, GStmtPlayersSelect, "id");
        snprintf(player->hash, sizeof (player->hash), "%s", SQLCOLUMN(text, GStmtPlayersSelect, "hashid"));
        snprintf(player->username, sizeof (player->username), "%s", SQLCOLUMN(text, GStmtPlayersSelect, "username"));
        player->next_logical_pc = (uint32) SQLCOLUMN(int, GStmtPlayersSelect, "next_logical_pc");
        player->next_logical_sp = (uint32) SQLCOLUMN(int, GStmtPlayersSelect, "next_logical_sp");
        player->next_logical_bp = (uint32) SQLCOLUMN(int, GStmtPlayersSelect, "next_logical_bp");
        player->next_inputbuf = inst->zmachine_state.story + ((size_t) SQLCOLUMN(int, GStmtPlayersSelect, "next_logical_inputbuf"));
        player->next_inputbuflen = (uint8) SQLCOLUMN(int, GStmtPlayersSelect, "next_logical_inputbuflen");
        player->next_operands[0] = (uint16) SQLCOLUMN(int, GStmtPlayersSelect, "next_operands_1");
        player->next_operands[1] = (uint16) SQLCOLUMN(int, GStmtPlayersSelect, "next_operands_2");
        snprintf(player->againbuf, sizeof (player->againbuf), "%s", SQLCOLUMN(text, GStmtPlayersSelect, "againbuf"));
        memcpy(player->stack, SQLCOLUMN(blob, GStmtPlayersSelect, "stack"), player->next_logical_sp * 2);
        memcpy(player->object_table_data, SQLCOLUMN(blob, GStmtPlayersSelect, "object_table_data"), sizeof (player->object_table_data));
        memcpy(player->property_table_data, SQLCOLUMN(blob, GStmtPlayersSelect, "property_table_data"), sizeof (player->property_table_data));
        memcpy(player->touchbits, SQLCOLUMN(blob, GStmtPlayersSelect, "touchbits"), sizeof (player->touchbits));
        player->gvar_location = (uint16) SQLCOLUMN(int, GStmtPlayersSelect, "gvar_location");
        player->gvar_coffin_held = (uint16) SQLCOLUMN(int, GStmtPlayersSelect, "gvar_coffin_held");
        player->gvar_dead = (uint16) SQLCOLUMN(int, GStmtPlayersSelect, "gvar_dead");
        player->gvar_deaths = (uint16) SQLCOLUMN(int, GStmtPlayersSelect, "gvar_deaths");
        player->gvar_lit = (uint16) SQLCOLUMN(int, GStmtPlayersSelect, "gvar_lit");
        player->gvar_alwayslit = (uint16) SQLCOLUMN(int, GStmtPlayersSelect, "gvar_alwayslit");
        player->gvar_verbose = (uint16) SQLCOLUMN(int, GStmtPlayersSelect, "gvar_verbose");
        player->gvar_superbrief = (uint16) SQLCOLUMN(int, GStmtPlayersSelect, "gvar_superbrief");
        player->gvar_lucky = (uint16) SQLCOLUMN(int, GStmtPlayersSelect, "gvar_lucky");
        player->gvar_loadallowed = (uint16) SQLCOLUMN(int, GStmtPlayersSelect, "gvar_loadallowed");
        player->game_over = SQLCOLUMN(int, GStmtPlayersSelect, "game_over");
        num_players++;
    }

    sqlite3_reset(GStmtPlayersSelect);

    if (num_players != inst->num_players) {
        loginfo("Uhoh, instance '%s' has %d players in the database, should be %d!", inst->hash, num_players, inst->num_players);
        return 0;
    }

    return 1;
}

static void write_to_connection(Connection *conn, const char *str);

static int db_select_recap(Player *player, const int rows_of_recap)
{
    Connection *conn = player->connection;
    if (!conn) {
        return 0;
    }

    //"select content from (select id, content from transcripts where player=$player order by id desc limit $limit) order by id;"
    if ( (sqlite3_reset(GStmtRecapSelect) != SQLITE_OK) ||
         (SQLBINDINT64(GStmtRecapSelect, "player", player->dbid) != SQLITE_OK) ||
         (SQLBINDINT(GStmtRecapSelect, "limit", rows_of_recap) != SQLITE_OK) ) {
        db_log_error("select recap");
        return 0;
    }

    while (sqlite3_step(GStmtRecapSelect) == SQLITE_ROW) {
        write_to_connection(conn, (const char *) SQLCOLUMN(text, GStmtRecapSelect, "content"));
    }

    sqlite3_reset(GStmtRecapSelect);
    return 1;
}

static sqlite3_int64 db_insert_crash(const char *errstr)
{
    Instance *inst = (Instance *) GState;  // this works because zmachine_state is the first field in Instance.
    if (!inst) {
        return 0;
    }

    //"insert into crashes (instance, timestamp, current_player, logical_pc, errstr)"
    //" values ($instance, $timestamp, $current_player, $logical_pc, $errstr);"
    const sqlite3_int64 retval =
           ( (sqlite3_reset(GStmtCrashInsert) == SQLITE_OK) &&
             (SQLBINDINT64(GStmtCrashInsert, "instance", inst->dbid) == SQLITE_OK) &&
             (SQLBINDINT64(GStmtCrashInsert, "timestamp", (sqlite3_int64) GNow) == SQLITE_OK) &&
             (SQLBINDINT(GStmtCrashInsert, "logical_pc", GState->logical_pc) == SQLITE_OK) &&
             (SQLBINDINT(GStmtCrashInsert, "current_player", inst->current_player) == SQLITE_OK) &&
             (SQLBINDTEXT(GStmtCrashInsert, "errstr", errstr) == SQLITE_OK) &&
             (sqlite3_step(GStmtCrashInsert) == SQLITE_DONE) ) ? sqlite3_last_insert_rowid(GDatabase) : 0;
    if (!retval) { db_log_error("insert crash"); }
    return retval;
}

static sqlite3_int64 db_insert_blocked(const char *address)
{
    //"insert into blocked (address, timestamp) values ($address, $timestamp);"
    const sqlite3_int64 retval =
           ( (sqlite3_reset(GStmtBlockedInsert) == SQLITE_OK) &&
             (SQLBINDTEXT(GStmtBlockedInsert, "address", address) == SQLITE_OK) &&
             (SQLBINDINT64(GStmtBlockedInsert, "timestamp", (sqlite3_int64) GNow) == SQLITE_OK) &&
             (sqlite3_step(GStmtBlockedInsert) == SQLITE_DONE) ) ? sqlite3_last_insert_rowid(GDatabase) : 0;
    if (!retval) { db_log_error("insert blocked"); }
    return retval;
}

static sqlite3_int64 db_select_blocked(const char *address)
{
    //"select timestamp from blocked where address = $address order by id desc limit 1;"
    int rc = SQLITE_ERROR;
    if ( (sqlite3_reset(GStmtBlockedSelect) != SQLITE_OK) ||
         (SQLBINDTEXT(GStmtBlockedSelect, "address", address) != SQLITE_OK) ||
         ((rc = sqlite3_step(GStmtBlockedSelect)) != SQLITE_ROW) ) {
        if (rc != SQLITE_DONE) {  // DONE == address was never blocked (no results).
            db_log_error("select blocked");
        }
        return 0;  // let it through even if an error.
    }

    const sqlite3_int64 retval = SQLCOLUMN(int64, GStmtBlockedSelect, "timestamp");
    sqlite3_reset(GStmtRecapSelect);
    return retval;
}

static void db_trim_recap(Instance *inst)
{
    //"delete from transcripts where player = $player and timestamp > $savetime;"
    for (int i = 0; i < inst->num_players; i++) {
        if ( (sqlite3_reset(GStmtRecapTrim) != SQLITE_OK) ||
             (SQLBINDINT64(GStmtRecapTrim, "player", inst->players[i].dbid) != SQLITE_OK) ||
             (SQLBINDINT64(GStmtRecapTrim, "savetime", (sqlite3_int64) inst->savetime) != SQLITE_OK) ||
             (sqlite3_step(GStmtRecapTrim) != SQLITE_DONE) ) {
            db_log_error("trim recap");
        }
    }
}

static void db_init(void)
{
    char *errmsg = NULL;

    if (sqlite3_initialize() != SQLITE_OK) {
        panic("sqlite3_initialize failed!");
    }

    if (sqlite3_open(MULTIZORK_DATABASE_PATH, &GDatabase) != SQLITE_OK) {
        panic("Couldn't open '%s'!", MULTIZORK_DATABASE_PATH);
    }

    if (sqlite3_exec(GDatabase, SQL_CREATE_TABLES, NULL, NULL, &errmsg) != SQLITE_OK) {
        panic("Couldn't create database tables! %s", errmsg);
    }

    if (sqlite3_prepare_v2(GDatabase, "begin transaction;", -1, &GStmtBegin, NULL) != SQLITE_OK) {
        panic("Failed to create BEGIN TRANSACTION SQL statement! %s", sqlite3_errmsg(GDatabase));
    }

    if (sqlite3_prepare_v2(GDatabase, "end transaction;", -1, &GStmtCommit, NULL) != SQLITE_OK) {
        panic("Failed to create END TRANSACTION SQL statement! %s", sqlite3_errmsg(GDatabase));
    }

    if (sqlite3_prepare_v2(GDatabase, SQL_TRANSCRIPT_INSERT, -1, &GStmtTranscriptInsert, NULL) != SQLITE_OK) {
        panic("Failed to create transcript insert SQL statement! %s", sqlite3_errmsg(GDatabase));
    }

    if (sqlite3_prepare_v2(GDatabase, SQL_USED_HASH_INSERT, -1, &GStmtUsedHashInsert, NULL) != SQLITE_OK) {
        panic("Failed to create used hash insert SQL statement! %s", sqlite3_errmsg(GDatabase));
    }

    if (sqlite3_prepare_v2(GDatabase, SQL_INSTANCE_INSERT, -1, &GStmtInstanceInsert, NULL) != SQLITE_OK) {
        panic("Failed to create instance insert SQL statement! %s", sqlite3_errmsg(GDatabase));
    }

    if (sqlite3_prepare_v2(GDatabase, SQL_INSTANCE_UPDATE, -1, &GStmtInstanceUpdate, NULL) != SQLITE_OK) {
        panic("Failed to create instance update SQL statement! %s", sqlite3_errmsg(GDatabase));
    }

    if (sqlite3_prepare_v2(GDatabase, SQL_INSTANCE_SELECT, -1, &GStmtInstanceSelect, NULL) != SQLITE_OK) {
        panic("Failed to create instance select SQL statement! %s", sqlite3_errmsg(GDatabase));
    }

    if (sqlite3_prepare_v2(GDatabase, SQL_PLAYER_INSERT, -1, &GStmtPlayerInsert, NULL) != SQLITE_OK) {
        panic("Failed to create player insert SQL statement! %s", sqlite3_errmsg(GDatabase));
    }

    if (sqlite3_prepare_v2(GDatabase, SQL_PLAYER_UPDATE, -1, &GStmtPlayerUpdate, NULL) != SQLITE_OK) {
        panic("Failed to create player update SQL statement! %s", sqlite3_errmsg(GDatabase));
    }

    if (sqlite3_prepare_v2(GDatabase, SQL_FIND_INSTANCE_BY_PLAYER_HASH, -1, &GStmtFindInstanceByPlayerHash, NULL) != SQLITE_OK) {
        panic("Failed to create find-instance-by-player-hash SQL statement! %s", sqlite3_errmsg(GDatabase));
    }

    if (sqlite3_prepare_v2(GDatabase, SQL_PLAYERS_SELECT, -1, &GStmtPlayersSelect, NULL) != SQLITE_OK) {
        panic("Failed to create select players SQL statement! %s", sqlite3_errmsg(GDatabase));
    }

    if (sqlite3_prepare_v2(GDatabase, SQL_RECAP_SELECT, -1, &GStmtRecapSelect, NULL) != SQLITE_OK) {
        panic("Failed to create select recap SQL statement! %s", sqlite3_errmsg(GDatabase));
    }

    if (sqlite3_prepare_v2(GDatabase, SQL_CRASH_INSERT, -1, &GStmtCrashInsert, NULL) != SQLITE_OK) {
        panic("Failed to create crash insert SQL statement! %s", sqlite3_errmsg(GDatabase));
    }

    if (sqlite3_prepare_v2(GDatabase, SQL_BLOCKED_INSERT, -1, &GStmtBlockedInsert, NULL) != SQLITE_OK) {
        panic("Failed to create blocked insert SQL statement! %s", sqlite3_errmsg(GDatabase));
    }

    if (sqlite3_prepare_v2(GDatabase, SQL_BLOCKED_SELECT, -1, &GStmtBlockedSelect, NULL) != SQLITE_OK) {
        panic("Failed to create blocked select SQL statement! %s", sqlite3_errmsg(GDatabase));
    }

    if (sqlite3_prepare_v2(GDatabase, SQL_RECAP_TRIM, -1, &GStmtRecapTrim, NULL) != SQLITE_OK) {
        panic("Failed to create recap trim SQL statement! %s", sqlite3_errmsg(GDatabase));
    }
}

static void db_quit(void)
{
    #define FINALIZE_DB_STMT(x) if (x) { sqlite3_finalize(x); x = NULL; }
    FINALIZE_DB_STMT(GStmtBegin);
    FINALIZE_DB_STMT(GStmtCommit);
    FINALIZE_DB_STMT(GStmtTranscriptInsert);
    FINALIZE_DB_STMT(GStmtUsedHashInsert);
    FINALIZE_DB_STMT(GStmtInstanceInsert);
    FINALIZE_DB_STMT(GStmtInstanceUpdate);
    FINALIZE_DB_STMT(GStmtInstanceSelect);
    FINALIZE_DB_STMT(GStmtPlayerInsert);
    FINALIZE_DB_STMT(GStmtPlayerUpdate);
    FINALIZE_DB_STMT(GStmtPlayersSelect);
    FINALIZE_DB_STMT(GStmtFindInstanceByPlayerHash);
    FINALIZE_DB_STMT(GStmtRecapSelect);
    FINALIZE_DB_STMT(GStmtCrashInsert);
    FINALIZE_DB_STMT(GStmtBlockedInsert);
    FINALIZE_DB_STMT(GStmtBlockedSelect);
    FINALIZE_DB_STMT(GStmtRecapTrim);
    #undef FINALIZE_DB_STMT

    if (GDatabase) {
        sqlite3_close(GDatabase);
        GDatabase = NULL;
    }

    sqlite3_shutdown();
}

static int generate_unique_hash(char *hash)  // `hash` points to up to 8 bytes of space.
{
    // this is kinda cheesy, but it's good enough.
    static const char chartable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    int notunique = 0;
    do {
        for (size_t i = 0; i < 6; i++) {
            hash[i] = chartable[((size_t) random()) % (sizeof (chartable)-1)];
        }
        hash[6] = '\0';

        const int rc = db_insert_used_hash(hash, &notunique);
        if (!rc && !notunique) {
            return 0;  // database problem
        }
    } while (notunique); // not unique? Try again.

    return 1;  // we're good.
}

static size_t count_newlines(const char *str, const uintptr slen)
{
    size_t retval = 0;
    for (size_t i = 0; i < slen; i++) {
        if (str[i] == '\n') {
            retval++;
        }
    }
    return retval;
}

// This queues a string for sending over the connection's socket when possible.
static void write_to_connection_slen(Connection *conn, const char *str, const uintptr slen)
{
    if (!conn || (conn->state != CONNSTATE_READY)) {
        return;
    }

    const size_t needed_space = slen + count_newlines(str, slen);
    const size_t avail = conn->outputbuf_len - conn->outputbuf_used;
    if (avail < needed_space) {
        void *ptr = realloc(conn->outputbuf, conn->outputbuf_len + needed_space + 1);
        if (!ptr) {
            panic("Uhoh, out of memory in write_to_connection");  // !!! FIXME: we could handle this more gracefully.
        }
        conn->outputbuf = (char *) ptr;
        conn->outputbuf_len += needed_space;
    }

    // replace "\n" with "\r\n" because telnet is terrible.
    for (size_t i = 0; i < slen; i++) {
        const char ch = str[i];
        if ( (ch == '\n') && ((i == 0) || (str[i-1] != '\r')) ) {
            conn->outputbuf[conn->outputbuf_used++] = '\r';
            conn->outputbuf[conn->outputbuf_used++] = '\n';
        } else {
            conn->outputbuf[conn->outputbuf_used++] = ch;
        }
    }
    conn->outputbuf[conn->outputbuf_used] = '\0';  // make sure we're always null-terminated.
}

static void write_to_connection(Connection *conn, const char *str)
{
    write_to_connection_slen(conn, str, strlen(str));
}

static void free_instance(Instance *inst);

static void drop_connection(Connection *conn)
{
    if (conn->state != CONNSTATE_READY) {
        return;  // already dropping.
    }

    loginfo("Starting drop of connection for socket %d", conn->sock);
    write_to_connection(conn, "\n\n");  // make sure we are a new line.
    conn->state = CONNSTATE_DRAINING;   // flush any pending output to the socket first.

    Instance *inst = conn->instance;
    int players_still_connected = 0;
    if (inst != NULL) {
        char msg[128];
        snprintf(msg, sizeof (msg), "\n\n*** %s has disconnected. If they come back, we'll let you know. ***\n\n\n>", conn->username);
        for (size_t i = 0; i < ARRAYSIZE(inst->players); i++) {
            Connection *c = inst->players[i].connection;
            if (c == conn) {
                inst->players[i].connection = NULL;  // no longer connected to this instance.
            } else if (c != NULL) {
                players_still_connected++;
                write_to_connection(c, msg);
            }
        }
        conn->instance = NULL;  // no longer part of an instance.

        if (!players_still_connected) {
            free_instance(inst);  // no one's still connected? Archive and free the instance.
        }
    }
}

static void broadcast_to_instance(Instance *inst, const char *str)
{
    if (inst) {
        for (size_t i = 0; i < ARRAYSIZE(inst->players); i++) {
            Player *player = &inst->players[i];
            write_to_connection(player->connection, str);
            if (i != inst->current_player) {  // these will transcribe with rest of buffer generated during inpfn_ingame.
                db_insert_transcript(player->dbid, TT_SYSTEM_MESSAGE, str);
            }
        }
    }
}

static void broadcast_to_room(Instance *inst, const uint16 room, const char *str)
{
    if (inst) {
        for (size_t i = 0; i < ARRAYSIZE(inst->players); i++) {
            Player *player = &inst->players[i];
            if (player->gvar_location == room) {
                write_to_connection(player->connection, str);
                if (i != inst->current_player) {  // these will transcribe with rest of buffer generated during inpfn_ingame.
                    db_insert_transcript(player->dbid, TT_SYSTEM_MESSAGE, str);
                }
            }
        }
    }
}

static Player *get_current_player(Instance *inst)
{
    assert(inst->current_player >= 0);
    assert(inst->current_player < ARRAYSIZE(inst->players));
    return &inst->players[inst->current_player];
}

// MojoZork Z-Machine overrides...

// ZORK 1 SPECIFIC MAGIC:
// Z-Machine version 3 (what Zork 1 uses) refers to objects by an 8-bit index,
//  with 0 being a "null" object, so you can address 255 objects. Zork 1
//  uses 250, so we have slots for adding players, but the object table has
//  space for _exactly_ 250 objects, with the property data starting in the
//  next byte. Since the object table is in dynamic memory, generally we
//  can't just move things around and it's legal for a Z-Machine program to
//  edit the object table directly by poking dynamic memory if it wants.
//  Fortunately, Zork 1 doesn't do this; it only uses the official opcodes
//  to access the object table. This means we don't have to override _any_
//  opcodes to add these extra objects, we just need to provid our own
//  implementation of MojoZork's getObjectPtr(), which all of those opcodes
//  already use to do their work. If we get a request for an object > 250,
//  we provide it a pointer to data we've set up outside the Z-Machine memory
//  map, where those extra objects live. Other games will use more objects
//  or perhaps touch the object table in dynamic memory directly, or just
//  touch dynamic memory above the object table, period, and that may need
//  way more tapdancing.
//
// ZORK 1 SPECIFIC MAGIC:
//  Rather than copy players objects around for each step_instance, we also
//  just provide the current player's pointer whenever the Z-Machine requests
//  object #4 ("cretin", the player). Other games might use a different index
//  for the player.
#define ZORK1_PLAYER_OBJID 4  // ZORK 1 SPECIFIC MAGIC
#define ZORK1_EXTERN_MEM_OBJS_BASE 251  // ZORK 1 SPECIFIC MAGIC

static uint16 remap_objectid(const uint16 objid)
{
    Instance *inst = (Instance *) GState;  // this works because zmachine_state is the first field in Instance.
    const uint16 external_mem_objects_base = ZORK1_EXTERN_MEM_OBJS_BASE;  // ZORK 1 SPECIFIC MAGIC
    return (objid == ZORK1_PLAYER_OBJID) ? (external_mem_objects_base + inst->current_player) : objid;  // ZORK 1 SPECIFIC MAGIC
}

// see comments on getObjectProperty
static uint8 *get_virtualized_mem_ptr(const uint16 offset)
{
    const uint16 fake_prop_base_addr = (uint16) (0x10000 - (MULTIPLAYER_PROP_DATALEN * 5));
    uint8 *ptr;
    if (offset < fake_prop_base_addr) {
        ptr = GState->story + offset;
    } else {
        Instance *inst = (Instance *) GState;  // this works because zmachine_state is the first field in Instance.
        const uint16 base_offset = offset - fake_prop_base_addr;  // unconst.
        const int requested_player = (int) (base_offset / MULTIPLAYER_PROP_DATALEN);
        ptr = inst->players[requested_player].property_table_data + base_offset;
    }
    return ptr;
}

static uint8 *getObjectPtr(const uint16 _objid)
{
    const uint16 objid = remap_objectid(_objid);
    if (objid == 0) {
        GState->die("Object id #0 referenced");
    } else if ((GState->header.version <= 3) && (objid > 255)) {
        GState->die("Invalid object id referenced");
    }

    const uint16 external_mem_objects_base = ZORK1_EXTERN_MEM_OBJS_BASE;  // ZORK 1 SPECIFIC MAGIC
    uint8 *ptr;
    if (objid >= external_mem_objects_base) {  // looking for a multiplayer character
        Instance *inst = (Instance *) GState;  // this works because zmachine_state is the first field in Instance.
        const int requested_player = (int) (objid - external_mem_objects_base);
        if (requested_player >= inst->num_players) {
            GState->die("Invalid multiplayer object id referenced");
        }
        ptr = inst->players[requested_player].object_table_data;
    } else {  // looking for a standard object.
        ptr = GState->story + GState->header.objtab_addr;
        ptr += 31 * sizeof (uint16);  // skip properties defaults table
        ptr += 9 * (objid-1);  // find object in object table
    }
    return ptr;
}

// See notes on getObjectPointer(); we need to provide external memory for the multiplayer object property tables, too.
//
// ZORK 1 SPECIFIC MAGIC:
// We need memory for properties for each multiplayer object, and
//  while properties can be anywhere in the dynamic memory area,
//  there isn't any place obviously available. However, for Zork 1,
//  The dynamic+static area is less than 0xFFFF, so there are
//  parts of the address space that are off-limits to the game
//  where we can pretend to store these--in what is actually
//  code locations--and then override the parts of the Z-Machine
//  that might access these to point them elsewhere when we see
//  the fake address.
static uint8 *getObjectProperty(const uint16 _objid, const uint32 propid, uint8 *_size)
{
    const uint16 objid = remap_objectid(_objid);
    uint8 *ptr;
    if (GState->header.version <= 3) {
        const uint16 external_mem_objects_base = ZORK1_EXTERN_MEM_OBJS_BASE;  // ZORK 1 SPECIFIC MAGIC
        if (objid >= external_mem_objects_base) {  // looking for a multiplayer character
            Instance *inst = (Instance *) GState;  // this works because zmachine_state is the first field in Instance.
            const int requested_player = (int) (objid - external_mem_objects_base);
            if (requested_player >= inst->num_players) {
                GState->die("Invalid multiplayer object id referenced");
            }
            ptr = inst->players[requested_player].property_table_data;
        } else {
            ptr = getObjectPtr(objid);
            ptr += 7;  // skip to properties address field.
            const uint16 addr = READUI16(ptr);
            ptr = GState->story + addr;
        }
        ptr += (*ptr * 2) + 1;  // skip object name to start of properties.

        while (1) {
            const uint8 info = *(ptr++);
            const uint16 num = (info & 0x1F);  // 5 bits for the prop id.
            const uint8 size = ((info >> 5) & 0x7) + 1; // 3 bits for prop size.
            // these go in descending numeric order, and should fail
            //  the interpreter if missing. We use 0xFFFFFFFF internally to mean "first property".
            if ((num == propid) || (propid == 0xFFFFFFFF)) { // found it?
                if (_size) {
                    *_size = size;
                }
                return ptr;
            } else if (num < propid) { // we're past it.
                break;
            }
            ptr += size;  // try the next property.
        }
    } else {
        GState->die("write me");
    }

    return NULL;
}

static void writestr_multizork(const char *str, const uintptr slen)
{
    Instance *inst = (Instance *) GState;  // this works because zmachine_state is the first field in Instance.
    write_to_connection_slen(get_current_player(inst)->connection, str, slen);
}

// see comments on getObjectProperty
static void opcode_get_prop_addr_multizork(void)
{
    Instance *inst = (Instance *) GState;  // this works because zmachine_state is the first field in Instance.
    uint8 *store = varAddress(*(GState->pc++), 1);
    const uint16 objid = remap_objectid(GState->operands[0]);
    const uint16 propid = GState->operands[1];
    const uint16 external_mem_objects_base = ZORK1_EXTERN_MEM_OBJS_BASE;  // ZORK 1 SPECIFIC MAGIC
    const uint8 *ptr = getObjectProperty(objid, propid, NULL);
    uint16 result;

    if (objid >= external_mem_objects_base) {  // looking for a multiplayer character
        const uint16 fake_prop_base_addr = (uint16) (0x10000 - (MULTIPLAYER_PROP_DATALEN * 5));
        const int requested_player = (int) (objid - external_mem_objects_base);
        if (requested_player >= inst->num_players) {
            GState->die("Invalid multiplayer object id referenced");
        }
        result = fake_prop_base_addr + (MULTIPLAYER_PROP_DATALEN * requested_player);  // we give each player FAKE bytes at the end of the address space.
        result += (uint16) ((size_t) (ptr - inst->players[requested_player].property_table_data));
    } else {
        result = ptr ? ((uint16) (ptr-GState->story)) : 0;
    }
    WRITEUI16(store, result);
}

static void opcode_print_obj_multizork(void)
{
    Instance *inst = (Instance *) GState;  // this works because zmachine_state is the first field in Instance.
    const uint16 external_mem_objects_base = ZORK1_EXTERN_MEM_OBJS_BASE;  // ZORK 1 SPECIFIC MAGIC
    const uint16 objid = remap_objectid(GState->operands[0]);

    if (GState->header.version <= 3) {
        const uint8 *ptr;
        if (objid >= external_mem_objects_base) {  // looking for a multiplayer character
            const int requested_player = (int) (objid - external_mem_objects_base);
            if (requested_player >= inst->num_players) {
                GState->die("Invalid multiplayer object id referenced");
            }
            ptr = inst->players[requested_player].property_table_data + 1;
        } else {
            ptr = getObjectPtr(GState->operands[0]);
            ptr += 7;  // skip to properties field.
            const uint16 addr = READUI16(ptr);  // dereference to get to property table.
            ptr = GState->story + addr + 1;
        }
        print_zscii(ptr, 0);
    } else {
        GState->die("write me");
    }
}

// When we hit a READ instruction in the Z-Machine, we assume we're back at the
//  prompt waiting for the user to type their next move. At this point we stop
//  the simulation, dump any accumulated output to the user, reset the move count
//  and let the next user's move execute, until each player had their turn, then
//  the move count increments and we wait for more input from the users.
//
// In this sense, READ is the entry point to the Z-Machine, as we stop when hit
//  this instruction and then run again until the next READ.
static void opcode_read_multizork(void)
{
    Instance *inst = (Instance *) GState;  // this works because zmachine_state is the first field in Instance.
    Player *player = get_current_player(inst);

    dbg("read from input stream: text-buffer=%X parse-buffer=%X\n", (unsigned int) GState->operands[0], (unsigned int) GState->operands[1]);

    uint8 *input = GState->story + GState->operands[0];
    const uint8 inputlen = *(input++);
    dbg("max input: %u\n", (unsigned int) inputlen);
    if (inputlen < 3)
        GState->die("text buffer is too small for reading");  // happens on buffer overflow.

    const uint8 *parse = GState->story + GState->operands[1];
    const uint8 parselen = *(parse++);

    dbg("max parse: %u\n", (unsigned int) parselen);
    if (parselen < 4)
        GState->die("parse buffer is too small for reading");  // happens on buffer overflow.

    player->next_inputbuf = input;
    player->next_inputbuflen = inputlen;
    player->next_operands[0] = GState->operands[0];
    player->next_operands[1] = GState->operands[1];
    GState->logical_pc = (uint32) (GState->pc - GState->story);  // next time, run the instructions _right after_ the READ opcode.
    GState->step_completed = 1;  // time to break out of the Z-Machine simulation loop.
}

static void opcode_save_multizork(void)
{
    GState->die("SAVE opcode executed despite our best efforts. Should not have happened!");
}

static void opcode_restore_multizork(void)
{
    GState->die("RESTORE opcode executed despite our best efforts. Should not have happened!");
}

static void opcode_restart_multizork(void)
{
    GState->die("RESTART opcode executed despite our best efforts. Should not have happened!");
}

// this is called by the Z-machine when there's a fatal error. Mojozork just terminates here,
//  but we need to survive and terminate just the one instance instead of the entire server.
//  So we longjmp back to step_instance() and clean up.
#if defined(__GNUC__) || defined(__clang__)
static void die_multizork(const char *fmt, ...) __attribute__((noreturn));
#elif defined(_MSC_VER)
__declspec(noreturn) static void die_multizork(const char *fmt, ...);
#endif

static void die_multizork(const char *fmt, ...)
{
    Instance *inst = (Instance *) GState;
    char err[128];
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, sizeof (err), fmt, ap);
    va_end(ap);

    inst->crashed = db_insert_crash(err);
    if (!inst->crashed) {
        inst->crashed = -1;  // just so we're non-zero.
    }

    snprintf(msg, sizeof (msg), "!! FATAL Z-MACHINE ERROR (instance='%s', err='%s', pc=%X, instructions_run=%u) !!", inst->hash, err, (uint) GState->logical_pc, (uint) GState->instructions_run);
    loginfo(msg);
    broadcast_to_instance(inst, msg);
    longjmp(inst->jmpbuf, 1);
}

static Instance *create_instance(void)
{
    Instance *inst = (Instance *) calloc(1, sizeof (Instance));
    if (inst) {
        // !!! FIXME: if the z-machine didn't have to write to a portion of this data
        // !!! FIXME:  (if we separate out the dynamic RAM section to a different part of ZMachineState
        // !!! FIXME:  and update mojozork to work out of there) then we wouldn't need a full copy
        // !!! FIXME:  of the game data for each instance. In practice, Zork 1 is 92160 bytes and
        // !!! FIXME:  only the first 11859 bytes are dynamic, so you're looking at an almost 80 kilobyte
        // !!! FIXME:  savings per instance here. But then again...80k ain't much in modern times.
        uint8 *story = (uint8 *) malloc(GOriginalStoryLen);
        if (!story) {
            free(inst);
            return NULL;
        }

        inst->current_player = -1;
        GState = &inst->zmachine_state;
        memcpy(story, GOriginalStory, GOriginalStoryLen);
        initStory(GOriginalStoryName, story, GOriginalStoryLen);
        for (size_t i = 0; i < ARRAYSIZE(inst->players); i++) {
            inst->players[i].next_logical_pc = GState->logical_pc;  // set all players to game entry point.
        }

        // override some Z-Machine opcode handlers we need...
        GState->opcodes[18].fn = opcode_get_prop_addr_multizork;
        GState->opcodes[138].fn = opcode_print_obj_multizork;
        GState->opcodes[181].fn = opcode_save_multizork;
        GState->opcodes[182].fn = opcode_restore_multizork;
        GState->opcodes[183].fn = opcode_restart_multizork;
        GState->opcodes[228].fn = opcode_read_multizork;

        for (uint8 i = 32; i <= 127; i++)  // 2OP opcodes repeating with different operand forms.
            GState->opcodes[i] = GState->opcodes[i % 32];
        for (uint8 i = 144; i <= 175; i++)  // 1OP opcodes repeating with different operand forms.
            GState->opcodes[i] = GState->opcodes[128 + (i % 16)];
        for (uint8 i = 192; i <= 223; i++)  // 2OP opcodes repeating with VAR operand forms.
            GState->opcodes[i] = GState->opcodes[i % 32];

        GState->writestr = writestr_multizork;
        GState->die = die_multizork;
        GState = NULL;
    }
    return inst;
}

static int step_instance(Instance *inst, const int playernum, const char *input)
{
    const uint16 external_mem_objects_base = ZORK1_EXTERN_MEM_OBJS_BASE;  // ZORK 1 SPECIFIC MAGIC
    int retval = 1;
    assert(playernum >= 0);
    assert(playernum < ARRAYSIZE(inst->players));
    Player *player = &inst->players[playernum];
    if (player->connection == NULL) {
        return 1;  // player is gone, don't run anything for them.
    }

    inst->current_player = playernum;

    // run this Z-machine instance until you hit a READ instruction
    //  (which is when the game would prompt the user for their next
    //  command), or some other world-stopping event.

    assert(inst->started);
    GState = &inst->zmachine_state;

    // as a convenience, we assume GState can be cast to an Instance* at times,
    // so make sure it's the first thing in the Instance struct and no padding
    // messed with the pointers.
    assert(((ZMachineState *) ((char *) inst)) == GState);

    loginfo("STEPPING for player #%d from pc=%X", playernum, (uint) player->next_logical_pc);

    GState->logical_pc = player->next_logical_pc;
    GState->pc = GState->story + GState->logical_pc;
    GState->sp = GState->stack + player->next_logical_sp;
    GState->bp = player->next_logical_bp;
    assert(player->next_logical_sp < ARRAYSIZE(player->stack));
    memcpy(GState->stack, player->stack, player->next_logical_sp * 2);
    uint16 *globals = (uint16 *) (GState->story + GState->header.globals_addr);

    // ZORK 1 SPECIFIC MAGIC:
    // some "globals" are player-specific, so we swap them in before running.
    globals[0] = player->gvar_location;
    globals[60] = player->gvar_lucky;
    globals[61] = player->gvar_deaths;
    globals[62] = player->gvar_dead;
    globals[66] = player->gvar_lit;
    globals[70] = player->gvar_superbrief;
    globals[71] = player->gvar_verbose;
    globals[72] = player->gvar_alwayslit;
    globals[133] = player->gvar_loadallowed;
    globals[139] = player->gvar_coffin_held;

    // ZORK 1 SPECIFIC MAGIC: save the WONFLAG value before this step runs.
    const uint16 starting_wonflag = globals[140];

    // ZORK 1 SPECIFIC MAGIC:
    // re-set the TOUCHBITs for all rooms for the current player.
    uint8 *roomobjptr = getObjectPtr(1);
    for (int i = 1; i <= 250; i++, roomobjptr += 9) {
        const uint8 parent = roomobjptr ? roomobjptr[4] : 0;
        if (parent != 82) { continue; } // in Zork 1, all rooms are children of object #82.
        const uint8 *bitptr = &player->touchbits[(i-1) / 8];
        const uint8 flag = 1 << ((i-1) % 8);
        const int isset = (*bitptr & flag) ? 1 : 0;
        if (isset) {
            *roomobjptr |= (0x80 >> 3);  // in zork1, TOUCHBIT is attribute 3.
        } else {
            *roomobjptr &= ~(0x80 >> 3);  // in zork1, TOUCHBIT is attribute 3.
        }
    }

    // ZORK 1 SPECIFIC MAGIC:
    // PLAYER global points to this player's object.
    uint8 *glob111 = (uint8 *) &globals[111];
    const uint16 playerobj = external_mem_objects_base + playernum;
    WRITEUI16(glob111, playerobj);

    // ZORK 1 SPECIFIC MAGIC: Thse are places where there is a hardcoded check for the ADVENTURER object index (4).
    //  There may be others I've missed. Patch those index values to be the current multiplayer object.
    const uint8 playerobj8 = (uint8) playerobj;
    GState->story[0x6B3F] = playerobj8;  // 6b3d:  JE              G6f,#04 [TRUE] 6b47
    GState->story[0x93E4] = playerobj8;  // 93e2:  JE              G6f,#04 [FALSE] 93fd
    GState->story[0x9411] = playerobj8;  // 9410:  JE              #04,G6f [TRUE] 9424
    GState->story[0xD748] = playerobj8;  // d743:  JE              L02,#bf,#72,#04 [TRUE] d7a4
    GState->story[0xE1AF] = playerobj8;  // e1ad:  JE              G6f,#04 [FALSE] e1c0
    GState->story[0x6B88] = playerobj8;  // 6b86:  JE              G6f,#04 [FALSE] 6b0e

    // If user had hit a READ instruction. Write the user's
    //  input to Z-Machine memory, and tokenize it.
    if (player->next_inputbuf) {
        snprintf((char *) player->next_inputbuf, player->next_inputbuflen-1, "%s", input ? input : "");
        // !!! FIXME: this is a hack. Blank out invalid characters.
        for (char *ptr = (char *) player->next_inputbuf; *ptr; ptr++) {
            const char ch = *ptr;
            if ((ch >= 'A') && (ch <= 'Z')) {
                *ptr = 'a' + (ch - 'A');  // lowercase it.
            } else if (((ch >= 'a') && (ch <= 'z')) || ((ch >= '0') && (ch <= '9')) || (strchr(" .,!?_#'\"/\\-:()", ch) != NULL)) {
                /* cool */ ;
            } else {
                *ptr = ' ';  /* oh well, blank it out. */
            }
        }

        GState->operands[0] = player->next_operands[0];  // tokenizing needs this.
        GState->operands[1] = player->next_operands[1];
        GState->operand_count = 2;
        player->next_inputbuf = NULL;  // don't do this again until we hit another READ opcode.
        player->next_inputbuflen = 0;
        tokenizeUserInput();  // now the Z-Machine will get what it expects from the previous READ instruction.
    }

    // ZORK 1 SPECIFIC MAGIC: mark the current player as invisible so he doesn't "see" himself.
    GState->operands[0] = ZORK1_PLAYER_OBJID;
    GState->operands[1] = 0x07;  // INVISIBLE bit
    opcode_set_attr();
    GState->operands[0] = ZORK1_PLAYER_OBJID;
    GState->operands[1] = 0x0E;  // NDESCBIT bit
    opcode_set_attr();

    // Now run the Z-Machine!
    if (setjmp(inst->jmpbuf) == 0) {
        GState->step_completed = 0;  // opcode_quit or opcode_read, etc.
        while (!GState->step_completed) {
            runInstruction();
        }

        // save off Z-Machine state for next time.
        player->next_logical_pc = GState->logical_pc;
        player->next_logical_sp = (uint32) (GState->sp - GState->stack);
        player->next_logical_bp = GState->bp;
        assert(player->next_logical_sp < ARRAYSIZE(player->stack));
        memcpy(player->stack, GState->stack, player->next_logical_sp * 2);

        // ZORK 1 SPECIFIC MAGIC:
        // some "globals" are player-specific, so we swap them out after running.
        player->gvar_location = globals[0];
        player->gvar_lucky = globals[60];
        player->gvar_deaths = globals[61];
        player->gvar_dead = globals[62];
        player->gvar_lit = globals[66];
        player->gvar_superbrief = globals[70];
        player->gvar_verbose = globals[71];
        player->gvar_alwayslit = globals[72];
        player->gvar_loadallowed = globals[133];
        player->gvar_coffin_held = globals[139];

        // ZORK 1 SPECIFIC MAGIC:
        // save off the TOUCHBIT for the player's current location, so we know they've already been there.
        roomobjptr = getObjectPtr(1);
        for (int i = 1; i <= 250; i++, roomobjptr += 9) {
            const uint8 parent = roomobjptr ? roomobjptr[4] : 0;
            if (parent != 82) { continue; } // in Zork 1, all rooms are children of object #82.
            const int isset = ((*roomobjptr) & (0x80 >> 3)) ? 1 : 0;  // in zork1, TOUCHBIT is attribute 3.
            uint8 *bitptr = &player->touchbits[(i-1) / 8];
            const uint8 flag = 1 << ((i-1) % 8);
            if (isset) {
                *bitptr |= flag;
            } else {
                *bitptr &= ~flag;
            }
        }

        // ZORK 1 SPECIFIC MAGIC:
        // Did WONFLAG get set? Player triggered the endgame this step!
        // When the WONFLAG is initially set, we reset the West of House
        // touchbit for everyone, not just the player that triggered the
        // endgame, so anyone that wanders back to that room will be told
        // about the entrance to the stone barrow.
        if ((starting_wonflag == 0) && (globals[140] != 0)) {
            loginfo("Player #%d on instance '%s' triggered the Zork 1 endgame!", playernum, inst->hash);
            const uint8 flag = 1 << (179 % 8); // 179==West of House objid, minus 1.
            for (int i = 0; i < inst->num_players; i++) {
                uint8 *bitptr = &inst->players[i].touchbits[179 / 8];  // 179==West of House objid, minus 1.
                *bitptr &= ~flag;
            }
        }

        // ZORK 1 SPECIFIC MAGIC: mark the current player as visible so other players see him.
        GState->operands[0] = ZORK1_PLAYER_OBJID;
        GState->operands[1] = 0x07;  // INVISIBLE bit
        opcode_clear_attr();
        GState->operands[0] = ZORK1_PLAYER_OBJID;
        GState->operands[1] = 0x0E;  // NDESCBIT bit
        opcode_clear_attr();

        if (inst->zmachine_state.quit) {
            // we've hit a QUIT opcode, which means, at least for Zork 1, that the user
            //  has either beaten the game or died in an unrecoverable way.
            // Drop this player's connection. Let the others continue to play.
            // If player comes back, they'll just hit the QUIT opcode immediately and get dropped again.
            inst->zmachine_state.quit = 0;  // reset for next player.
            player->game_over = 1;  // flag this player as done.
            drop_connection(player->connection);
        }
    } else {
        // uhoh, the Z-machine called die(). Kill this instance.
        broadcast_to_instance(inst, "\n\n*** Oh no, this game instance had a fatal error, so we're jumping ship! ***\n\n\n");
        free_instance(inst);
        retval = 0;
    }

    inst->current_player = -1;
    GState = NULL;
    return retval;
}

static void db_failed_at_instance_start(Instance *inst)
{
    broadcast_to_instance(inst, "\n\n*** Oh no, we failed to set up the database, so we're jumping ship! ***\n\n\n");
    inst->dbid = 0;
    inst->started = 0;  // don't try to archive this instance.
    free_instance(inst);
}

static void inpfn_ingame(Connection *conn, const char *str);

static void start_instance(Instance *inst)
{
    // Flatten out the players list so there aren't any blanks in the middle.
    size_t num_players = 0;
    Player players[ARRAYSIZE(inst->players)];
    for (size_t i = 0; i < ARRAYSIZE(inst->players); i++) {
        Connection *conn = inst->players[i].connection;
        if (conn) {
            memcpy(&players[num_players], &inst->players[i], sizeof (Player));
            num_players++;
        }
    }
    memset(inst->players, '\0', sizeof (inst->players));
    memcpy(inst->players, players, num_players * sizeof (Player));
    inst->num_players = num_players;

    // !!! FIXME: split this out to a separate function.
    GState = &inst->zmachine_state;
    uint16 *globals = (uint16 *) (GState->story + GState->header.globals_addr);
    const uint8 *playerptr = GState->story + GState->header.objtab_addr;
    playerptr += 31 * sizeof (uint16);  // skip properties defaults table
    playerptr += 9 * (ZORK1_PLAYER_OBJID-1);  // find object in object table  // ZORK 1 SPECIFIC MAGIC

    const uint8 *propptr = playerptr + 7;  // skip to properties address field.
    const uint16 propaddr = READUI16(propptr);
    propptr = GState->story + propaddr;
    propptr += (*propptr * 2) + 1;  // skip object name to start of properties.

    uint16 propsize = 0;
    while (propptr[propsize]) {
        propsize += (((propptr[propsize] >> 5) & 0x7) + 1) + 1;
    }

    assert(propsize < sizeof (inst->players[0].property_table_data));

    const uint16 external_mem_objects_base = ZORK1_EXTERN_MEM_OBJS_BASE;  // ZORK 1 SPECIFIC MAGIC

    int dbokay = 1;
    for (size_t i = 0; i < num_players; i++) {
        Player *player = &inst->players[i];
        Connection *conn = player->connection;

        // save off the initial value of the globals we track per-player.
        player->gvar_location = globals[0];
        player->gvar_lucky = globals[60];
        player->gvar_deaths = globals[61];
        player->gvar_dead = globals[62];
        player->gvar_lit = globals[66];
        player->gvar_superbrief = globals[70];
        player->gvar_verbose = globals[71];
        player->gvar_alwayslit = globals[72];
        player->gvar_loadallowed = globals[133];
        player->gvar_coffin_held = globals[139];

        snprintf(player->username, sizeof (player->username), "%s", conn->username);

        // Copy the original player object for each player.
        memcpy(player->object_table_data, playerptr, sizeof (player->object_table_data));

        // Build a custom property table for each player.
        uint8 *propdst = player->property_table_data;
        propdst++;  // text-length (number of 2-byte words). Skip for now.

        // Encode the player's name to ZSCII. We cheat and only let you have
        // lowercase letters for now (!!! FIXME: but a better ZSCII encoder here would open options)
        uint8 numwords = 0;
        const char *str = player->username;
        while (*str) {
            const uint16 zch1 = (uint8) ((*(str++) - 'a') + 6);
            const uint16 zch2 = *str ? ((uint8) ((*(str++) - 'a') + 6)) : 5;  // 5 is a padding character at end of string.
            const uint16 zch3 = *str ? ((uint8) ((*(str++) - 'a') + 6)) : 5;  // 5 is a padding character at end of string.
            const uint16 termbit = ((*str == '\0') || (zch2 == 5) || (zch3 == 5)) ? (1 << 15) : 0;
            const uint16 zword = (zch1 << 10) | (zch2 << 5) | zch3 | termbit;
            WRITEUI16(propdst, zword);
            numwords++;
        }
        *player->property_table_data = numwords;

        assert((propsize + (numwords * 2) + 1) <= sizeof (player->property_table_data));
        memcpy(propdst, propptr, propsize);

        snprintf(player->againbuf, sizeof (player->againbuf), "verbose");  // typing "again" as the first command appears to do "verbose". Beats me.

        dbokay = dbokay && generate_unique_hash(player->hash);  // assign a hash to the player while we're here so they can rejoin.

        snprintf(player->username, sizeof (player->username), "%s", conn->username);
        write_to_connection(conn, "\n\n");
        write_to_connection(conn, "*** THE GAME IS STARTING ***\n");
        write_to_connection(conn, "You can leave at any time by typing 'quit'.\n");
        write_to_connection(conn, "You can speak to others in the same room with '!some text' or the whole game with '!!some text'.\n");
        write_to_connection(conn, "If you get disconnected or leave, you can rejoin at any time\n");
        write_to_connection(conn, " with this access code: '");
        write_to_connection(conn, player->hash);
        write_to_connection(conn, "'\n\n(Have fun!)\n\n\n");
        conn->inputfn = inpfn_ingame;
    }

    if (!dbokay) {
        db_failed_at_instance_start(inst);
        return;
    }

    inst->started = 1;

    uint8 *startroomptr = getObjectPtr(180);  // ZORK 1 SPECIFIC MAGIC: West of House room.
    GState = NULL;

    const uint8 orig_start_room_child = startroomptr[6];
    uint32 outputbuf_used_at_start[ARRAYSIZE(inst->players)];

    // run a step right now, so they get the intro text and their next input will be for the game.
    for (int i = 0; i < num_players; i++) {
        outputbuf_used_at_start[i] = inst->players[i].connection ? inst->players[i].connection->outputbuf_used : 0;
        // just this once, reset the Z-Machine between each player, so that we end up with
        //  one definite state and things like intro text gets run...
        // This just resets the dynamic memory. The rest of the address space is immutable.
        memcpy(inst->zmachine_state.story, GOriginalStory, ((size_t) inst->zmachine_state.header.staticmem_addr));

        // ZORK 1 SPECIFIC MAGIC:
        // Insert all the players into the West of House room each time. The
        //  game will move the current player there on startup, but we're
        //  resetting dynamic memory between each step here, which wipes
        //  those moves.
        GState = &inst->zmachine_state;
        for (int j = 0; j < num_players; j++) {
            Player *player = &inst->players[j];
            uint8 *ptr = player->object_table_data;
            ptr[4] = 180;  // parent is West of House room.
            ptr[5] = (j < (num_players-1)) ? (external_mem_objects_base + j + 1) : orig_start_room_child;
            assert(ptr[6] == 0);  // assume player object has no initial children

            // ZORK 1 SPECIFIC MAGIC: mark the player as visible so other players see him.
            GState->operands[0] = external_mem_objects_base + j;
            GState->operands[1] = 0x07;  // INVISIBLE bit
            opcode_clear_attr();
            GState->operands[0] = external_mem_objects_base + j;
            GState->operands[1] = 0x0E;  // NDESCBIT bit
            opcode_clear_attr();
        }
        startroomptr[6] = external_mem_objects_base;  // make players start of child list for start room.
        GState = NULL;

        // PLAYER global points to this player's object.
        uint8 *glob111 = (uint8 *) &globals[111];
        const uint16 playerobj = external_mem_objects_base + i;
        WRITEUI16(glob111, playerobj);

        // Run until the READ instruction, then gameplay officially starts.
        if (!step_instance(inst, i, NULL)) {
            break;  // instance failed, don't access it further.
        }
    }

    dbokay = dbokay && db_begin_transaction();
    if (dbokay) {
        inst->savetime = GNow;
        inst->dbid = db_insert_instance(inst);
        dbokay = dbokay && (inst->dbid != 0);
        for (int i = 0; i < num_players; i++) {
            Player *player = &inst->players[i];
            if (dbokay) {
                player->dbid = db_insert_player(inst, i);
                dbokay = dbokay && (player->dbid != 0);
                if (player->connection) {
                    dbokay = dbokay && db_insert_transcript(player->dbid, TT_GAME_OUTPUT, player->connection->outputbuf + outputbuf_used_at_start[i]);
                }
            }
        }
        if (!db_end_transaction()) {
            dbokay = 0;
        }
    }

    if (!dbokay) {
        db_failed_at_instance_start(inst);
    }
}

static void save_instance(Instance *inst)
{
    if (inst->started && inst->dbid) {  // if game started, save the state. If not, just drop the resources.
        // not much we can do if this fails...
        loginfo("Saving instance '%s'...", inst->hash);
        if (db_begin_transaction()) {
            db_update_instance(inst);
            for (int i = 0; i < inst->num_players; i++) {
                db_update_player(inst, i);
            }
            db_end_transaction();
        }
        inst->savetime = GNow;
    }
}

static void free_instance(Instance *inst)
{
    if (!inst) {
        return;
    }

    loginfo("Destroying instance '%s'", inst->hash);

    for (size_t i = 0; i < ARRAYSIZE(inst->players); i++) {
        Player *player = &inst->players[i];
        Connection *conn = player->connection;
        if (conn) {
            write_to_connection(conn, "\n\n\nTHIS INSTANCE IS BEING DESTROYED, SORRY, HANGING UP.\n\n\n\n");
            conn->instance = NULL;  // so other players aren't alerted that you're "leaving" or we double-free.
            drop_connection(conn);
        }
    }

    save_instance(inst);

    if (GState == &inst->zmachine_state) {
        GState = NULL;
    }

    free(inst->zmachine_state.story);
    free(inst->zmachine_state.story_filename);
    free(inst);
}

static Player *find_connection_player(Connection *conn, int *_playernum)
{
    Instance *inst = conn->instance;
    if (inst) {
        for (int i = 0; i < inst->num_players; i++) {
            if (inst->players[i].connection == conn) {
                if (_playernum) { *_playernum = i; }
                return &inst->players[i];
            }
        }
    }
    if (_playernum) { *_playernum = -1; }
    return NULL;
}


static void inpfn_confirm_quit(Connection *conn, const char *str)
{
    Player *player = find_connection_player(conn, NULL);
    if (strcasecmp(str, "y") == 0) {
        if (player != NULL) {
            write_to_connection(conn, "\nOkay, you can come back to this game in progress with this code:\n");
            write_to_connection(conn, "    ");
            write_to_connection(conn, player->hash);
            write_to_connection(conn, "\n\n\n");
            write_to_connection(conn, "And view transcripts from this game here:\n");
            write_to_connection(conn, "    ");
            write_to_connection(conn, MULTIZORK_TRANSCRIPT_BASEURL);
            write_to_connection(conn, "/game/");
            write_to_connection(conn, conn->instance->hash);
            write_to_connection(conn, "\n\nAnd don't forget to toss a dollar at my Patreon if you liked this:\n");
            write_to_connection(conn, "    https://patreon.com/icculus\n");
        }
        write_to_connection(conn, "\n\nGood bye!\n");
        drop_connection(conn);
    } else {
        write_to_connection(conn, "Ok.\n>");
        conn->inputfn = inpfn_ingame;
    }
}

static void inpfn_ingame(Connection *conn, const char *str)
{
    Instance *inst = conn->instance;
    uint32 newoutput_start = conn->outputbuf_used;
    int playernum;
    Player *player = find_connection_player(conn, &playernum);
    char msg[256];

    if (!player) {
        loginfo("Um, socket %d is trying to talk to instance '%s', which it is not a player on.", conn->sock, inst->hash);
        write_to_connection(conn, "\n\n*** The server appears to be confused. This is a bug on our end. Sorry, dropping you now. ***\n\n\n");
        drop_connection(conn);
        return;
    }

    if ((strcasecmp(str, "q") == 0) || (strncasecmp(str, "quit", 4) == 0)) {
        write_to_connection(conn, "Do you wish to leave the game? (Y is affirmative):");
        conn->inputfn = inpfn_confirm_quit;
        return;  // don't transcribe this part.
    }

    // we just go on without transcripts if there's a database problem. The best
    //  we could do is drop the connections and know that it probably can't archive
    //  the instance for return to later, so might as well let them play through.
    db_begin_transaction();

    // transcribe user input.
    snprintf(msg, sizeof (msg), "%s\n", str);
    db_insert_transcript(player->dbid, TT_PLAYER_INPUT, msg);

    // The Z-Machine normally handles this, but I'm not sure how at the moment,
    //  so rather than trying to track that data per-player, we just catch
    //  the AGAIN command here and replace it with the user's last in-game
    //  input.
    if (strcasecmp(str, "again") == 0) {
        str = player->againbuf;
    } else {
        snprintf(player->againbuf, sizeof (player->againbuf), "%s", str);
    }

    if (strncasecmp(str, "save", 4) == 0) {
        write_to_connection(conn, "Requests to save the game are ignored, sorry.\n>");
    } else if (strncasecmp(str, "restore", 7) == 0) {
        write_to_connection(conn, "Requests to restore the game are ignored, sorry.\n>");
    } else if (str[0] == '!') {
        if (str[1] == '!') { // broadcast to whole instance
            snprintf(msg, sizeof (msg), "\n*** %s says to the whole dungeon, \"%s\" ***\n\n>", player->username, str + 2);
            broadcast_to_instance(inst, msg);
        } else {
            snprintf(msg, sizeof (msg), "\n*** %s says to the room, \"%s\" ***\n\n>", player->username, str + 1);
            broadcast_to_room(inst, player->gvar_location, msg);
        }
        // skip this output: the broadcast_* functions already transcribed it (current_player is still -1 since we aren't stepping the instance yet).
        newoutput_start = conn->outputbuf_used;
    } else {
        const uint16 loc = player->gvar_location;
        player->gvar_location = 0;  // so we don't broadcast to ourselves.
        snprintf(msg, sizeof (msg), "\n*** %s decides to \"%s\" ***\n>", player->username, str);
        broadcast_to_room(inst, loc, msg);
        player->gvar_location = loc;
        step_instance(conn->instance, playernum, str);  // run the Z-machine with new input.

        const uint16 newloc = player->gvar_location;
        if (newloc != loc) { // player moved to a new room?
            player->gvar_location = 0;  // so we don't broadcast to ourselves.
            snprintf(msg, sizeof (msg), "\n*** %s has left the area. ***\n>", player->username);
            broadcast_to_room(inst, loc, msg);
            snprintf(msg, sizeof (msg), "\n*** %s has entered the area. ***\n>", player->username);
            broadcast_to_room(inst, newloc, msg);
            player->gvar_location = newloc;
        }
    }

    if (conn->outputbuf_used > newoutput_start) {  // new output to transcribe?
        db_insert_transcript(player->dbid, TT_GAME_OUTPUT, conn->outputbuf + newoutput_start);
    }

    db_end_transaction();

    inst->moves_since_last_save++;
    if (inst->moves_since_last_save >= MULTIZORK_AUTOSAVE_EVERY_X_MOVES) {
        save_instance(inst);
        inst->moves_since_last_save = 0;
    }
}

static void inpfn_waiting_for_players(Connection *conn, const char *str)
{
    const int go = (strcmp(str, "go") == 0);
    Instance *inst = conn->instance;
    int num_players = 0;

    if (strcmp(str, "quit") == 0) {
        write_to_connection(conn, "Okay, maybe some other time. Bye!\n");
        for (size_t i = 0; i < ARRAYSIZE(inst->players); i++) {
            Connection *c = inst->players[i].connection;
            if ((c != NULL) && (c != conn)) {
                write_to_connection(c, "\nSorry, ");
                write_to_connection(c, conn->username);
                write_to_connection(c, " decided to cancel the game. Try again later?\n");
            }
        }
        free_instance(inst);
        drop_connection(conn); // free_instance _should_ have handled this.
        return;
    }

    write_to_connection(conn, "Your current guest list is:\n\n");
    for (size_t i = 0; i < ARRAYSIZE(inst->players); i++) {
        const Connection *c = inst->players[i].connection;
        if ((c != NULL) && (c != conn)) {
            num_players++;
            write_to_connection(conn, " - ");
            write_to_connection(conn, c->username);
            write_to_connection(conn, "\n");
        }
    }

    if (num_players == 0) {
        write_to_connection(conn, " ...apparently no one! Running solo, huh? Right on.\n");
    }
    write_to_connection(conn, "\n");

    if (go) {
        write_to_connection(conn, "Okay! Here we go! Buckle up.\n");
        start_instance(inst);  // will move all players to new inputfn
    } else {
        write_to_connection(conn, "Still waiting for people to join.\n");
        write_to_connection(conn, "Type 'go' to start with those currently present.\n");
        write_to_connection(conn, "Type 'quit' to drop this game and anyone connected.\n");
    }
}

static void inpfn_player_waiting(Connection *conn, const char *str)
{
    if (strcmp(str, "quit") == 0) {
        write_to_connection(conn, "Okay, maybe some other time. Bye!");
        drop_connection(conn);
        return;
    }

    Instance *inst = conn->instance;
    write_to_connection(conn, "The current guest list is:\n\n");
    for (size_t i = 0; i < ARRAYSIZE(inst->players); i++) {
        const Connection *c = inst->players[i].connection;
        if (c != NULL) {
            write_to_connection(conn, " - ");
            write_to_connection(conn, c->username);
            write_to_connection(conn, "\n");
        }
    }
    write_to_connection(conn, "\n");

    write_to_connection(conn, "Waiting for the game to start (and maybe other people to arrive). Sit tight.\n");
    write_to_connection(conn, "If you get bored of waiting, you can type 'quit' to leave.");
}

static void inpfn_enter_instance_code_to_join(Connection *conn, const char *str)
{
    Instance *inst = NULL;

    assert(conn->instance == NULL);

    if (strcmp(str, "quit") == 0) {
        write_to_connection(conn, "Okay, maybe some other time. Bye!");
        drop_connection(conn);
        return;
    }
        
    if (strlen(str) == 6) {
        // !!! FIXME: maintaining a list of instances means up to 4x less
        // !!! FIXME:  searches than looking through the connections to
        // !!! FIXME:  find it. A hashtable even more so.
        for (size_t i = 0; i < num_connections; i++) {
            Connection *c = connections[i];
            if (c->instance && (strcmp(c->instance->hash, str) == 0)) {
                inst = c->instance;
                break;
            }
        }
    }

    if (inst == NULL) {
        write_to_connection(conn, "Sorry, I can't find that code. Try again or type 'quit'.");
    } else {
        write_to_connection(conn, "Found it!\n");

        if ((inst->started) || (inst->crashed)) {
            write_to_connection(conn, "...but it appears to have already started without you. Sorry!\n");
            write_to_connection(conn, "You can enter a different code or type 'quit'\n");
            return;
        }

        for (size_t i = 0; i < ARRAYSIZE(inst->players); i++) {
            if (inst->players[i].connection == NULL) {
                inst->players[i].connection = conn;
                conn->instance = inst;
                break;
            }
        }

        if (conn->instance == NULL) {
            write_to_connection(conn, "...but it appears to be full. Too popular!\n");
            write_to_connection(conn, "You can enter a different code or type 'quit'\n");
        } else {
            for (size_t i = 0; i < ARRAYSIZE(inst->players); i++) {
                Connection *c = inst->players[i].connection;
                if (c != conn) {
                    write_to_connection(c, "\n*** ");
                    write_to_connection(c, conn->username);
                    write_to_connection(c, " has joined this game! ***\n>");
                }
            }

            conn->inputfn = inpfn_player_waiting;
            conn->inputfn(conn, "");  // call this now just to print the guest list.

            write_to_connection(conn, "\n\nWhile we're waiting, let me say I built this for my patrons. If you like\n");
            write_to_connection(conn, "this sort of thing, please send a dollar to https://patreon.com/icculus !\n\n");
        }
    }
}

static void inpfn_new_game_or_join(Connection *conn, const char *str)
{
    if (strcmp(str, "1") == 0) {  // new game
        assert(!conn->instance);
        conn->instance = create_instance();
        if (!conn->instance) {
            write_to_connection(conn, "Uhoh, we appear to be out of memory. Try again later?\n");
            drop_connection(conn);
            return;
        }

        if (!generate_unique_hash(conn->instance->hash)) {
            write_to_connection(conn, "Uhoh, we appear to be having a database problem. Try again later?\n");
            Instance *inst = conn->instance;
            conn->instance = NULL;
            db_failed_at_instance_start(inst);
            drop_connection(conn);
            return;
        }

        loginfo("Created new instance '%s'", conn->instance->hash);

        conn->instance->players[0].connection = conn;
        write_to_connection(conn, "Okay! Tell your friends to telnet here, too, and join game '");
        write_to_connection(conn, conn->instance->hash);
        write_to_connection(conn, "'.\n\n");
        write_to_connection(conn, "We'll wait for them now.\n");
        write_to_connection(conn, "You can type 'go' to begin when enough have arrived.\n");
        write_to_connection(conn, "There's still room for three more people.\n");
        write_to_connection(conn, "Once you type 'go' no more will be admitted.\n");
        write_to_connection(conn, "Type 'quit' to drop this game and anyone connected.\n");

        write_to_connection(conn, "\n\nWhile we're waiting, let me say I built this for my patrons. If you like\n");
        write_to_connection(conn, "this sort of thing, please send a dollar to https://patreon.com/icculus !\n\n");

        conn->inputfn = inpfn_waiting_for_players;
    } else if (strcmp(str, "2") == 0) {  // join an existing game
        write_to_connection(conn, "Okay! The person that started the game has a code for you to enter.\n");
        write_to_connection(conn, "Please type it here.");
        conn->inputfn = inpfn_enter_instance_code_to_join;
    } else if (strcmp(str, "3") == 0) {  // quit
        write_to_connection(conn, "\n\nOkay, bye for now!\n\n");
        drop_connection(conn);
    } else {
        write_to_connection(conn, "Please type '1', '2', or '3'");
    }
}

static void inpfn_enter_name(Connection *conn, const char *str)
{
    if (*str == '\0') {  // just hit enter without a specific code?
        write_to_connection(conn, "You have to enter a name. Try again.");
        return;
    }

    memset(conn->username, '\0', sizeof (conn->username));
    for (int i = 0; *str && (i < sizeof (conn->username) - 1); i++) {
        while (*str) {
            const char ch = *(str++);
            if ((ch >= 'a') && (ch <= 'z')) {
                conn->username[i] = ch;
            } else if ((ch >= 'A') && (ch <= 'Z')) {
                conn->username[i] = 'a' + (ch - 'A');
            } else {
                continue;  // ignore this one, try the next char in str.
            }
            break;  // we used this char, move to the next dst char.
        }
    }

    if (*conn->username == '\0') {
        write_to_connection(conn, "Sorry, I couldn't use any of that name. Try again.");
        return;
    }

    write_to_connection(conn, "Okay, we're referring to you as '");
    write_to_connection(conn, conn->username);
    write_to_connection(conn, "' from now on.\n\n");
    write_to_connection(conn, "Now that that's settled:\n\n");
    write_to_connection(conn, "1) start a new game\n");
    write_to_connection(conn, "2) join someone else's game\n");
    write_to_connection(conn, "3) quit\n");
    conn->inputfn = inpfn_new_game_or_join;
}

static Player *reconnect_player(Connection *conn, const char *access_code)
{
    if (strlen(access_code) != 6) {
        write_to_connection(conn, "Hmm, I can't find a game with that access code.\n");
        return NULL;  // not a valid code.
    }

    // See if we're rejoining a live game...
    // !!! FIXME: maintaining a list of instances means a lot less
    // !!! FIXME:  searches than looking through the connections to
    // !!! FIXME:  find it. A hashtable even more so.
    for (size_t i = 0; i < num_connections; i++) {
        Connection *c = connections[i];
        Instance *inst = c->instance;
        if (inst) {
            for (int i = 0; i < inst->num_players; i++) {
                Player *player = &inst->players[i];
                if (strcmp(player->hash, access_code) == 0) {
                    if (player->connection != NULL) {
                        write_to_connection(conn, "Hmmm, that's a valid access code, but it's currently in use by another connection.\n");
                        return NULL;
                    }
                    player->connection = conn;   // just wire right back in and go.
                    conn->instance = inst;
                    conn->inputfn = inpfn_ingame;
                    snprintf(conn->username, sizeof (conn->username), "%s", player->username);
                    return player;
                }
            }
        }
    }

    // Not found? Might be a game we archived because everyone left.
    const sqlite3_int64 instance_dbid = db_find_instance_by_player_hash(access_code);
    if (instance_dbid == 0) {  // if zero, nope, just a bogus code...
        write_to_connection(conn, "Hmm, I can't find a game with that access code.\n");
        return NULL;
    }

    // !!! FIXME: assert this instance definitely isn't live right now.
    Instance *inst = create_instance();
    if (!inst) {
        write_to_connection(conn, "Hmm, that's a valid access code, but I seem to have run out of memory! Try again later.\n");
        return NULL;
    }

    if (!db_select_instance(inst, instance_dbid)) {
        write_to_connection(conn, "Hmm, that's a valid access code, but I had trouble starting the game! Try again later.\n");
        free_instance(inst);
        return NULL;
    }

    // !!! FIXME: crashed games should save off the current state for postmortem debugging, but
    // !!! FIXME:  shouldn't overwrite the probably-good state that's already in the database, so
    // !!! FIXME:  the game can continue from that point instead.
    if (inst->crashed) {
        write_to_connection(conn, "Hmm, that's a valid access code, but this game crashed before and can't be rejoined.\n");
        free_instance(inst);
        return NULL;
    }

    // if the server (not the instance) crashed in some way between the last
    //  save and a later move, we may have transcripts that are no longer
    //  accurate now, as we'll have rewound time in a sense. Delete any
    //  recap for this instance that are newer than the latest save time.
    db_trim_recap(inst);

    loginfo("Rehydrated archived instance '%s'", inst->hash);

    for (int i = 0; i < inst->num_players; i++) {
        Player *player = &inst->players[i];
        if (strcmp(player->hash, access_code) == 0) {
            assert(player->connection == NULL);
            player->connection = conn;   // just wire right back in and go.
            conn->inputfn = inpfn_ingame;
            conn->instance = inst;
            inst->started = 1;
            snprintf(conn->username, sizeof (conn->username), "%s", player->username);
            return player;
        }
    }

    assert(!"This shouldn't happen");
    write_to_connection(conn, "Hmm, we found that access code, but something internal went wrong. Try again later?\n");
    free_instance(inst);
    return NULL;
}

// First prompt after connecting.
static void inpfn_hello_sailor(Connection *conn, const char *str)
{
    if (*str == '\0') {  // just hit enter without a specific code?
        write_to_connection(conn, "Okay, let's get you set up.\n\n");
        write_to_connection(conn, "What's your name? Keep it simple or I'll simplify it for you.\n");
        write_to_connection(conn, "(sorry if your name isn't one word made up of english letters.\n");
        write_to_connection(conn, " This is American tech from 1980, after all.)");
        conn->inputfn = inpfn_enter_name;
    } else {
        // popular bots that troll telnet ports looking to pop a shell will send these as first commands. Dump them.
        static const char *hacker_commands[] = { "system", "shell", "sh", "enable", "admin", "root", "Administrator", "runshellcmd", "linuxshell", "start-shell", "start start-shell", "start-shell bash" };
        for (int i = 0; i < ARRAYSIZE(hacker_commands); i++) {
            if (strcmp(str, hacker_commands[i]) == 0) {
                const char *addr = conn->address;
                loginfo("Socket %d (%s) is probably malicious, blocked and dropped.", conn->sock, addr);
                conn->blocked = 1;  // drop this connection's further input.
                if ((strcmp(addr, "127.0.0.1") == 0) || (strcmp(addr, "::ffff:127.0.0.1") == 0) || (strcmp(addr, "::1") == 0)) {
                    loginfo("(not actually blocking localhost.)");
                } else {
                    db_insert_blocked(conn->address);
                }
                write_to_connection(conn, "Nice try.\n");
                drop_connection(conn);
                return;
            }
        }

        // look up player code.
        Player *player = reconnect_player(conn, str);
        if (!player) {
            write_to_connection(conn, "Try another code, or just press enter.\n");
            return;
        }

        write_to_connection(conn, "We found you! Here's where you left off:\n\n");
        db_select_recap(player, 5);
        assert(player->connection == conn);
        assert(player->connection->inputfn == inpfn_ingame);

        if (player->game_over) {  // their game has already ended, drop them.
            drop_connection(conn);
        }
    }
}

// dump whitespace on both sides of a string.
static void trim(char *str)
{
    char *i = str;
    while ((*i == ' ') || (*i == '\t')) { i++; }
    if (i != str) {
        memmove(str, i, strlen(i) + 1);
    }
    i = (str + strlen(str)) - 1;
    while ((i != str) && ((*i == ' ') || (*i == '\t'))) { i--; }
    if ((*i != ' ') && (*i != '\t')) {
        i++;
    }
    *i = '\0';
}

// this is a bit of a cheat, but it eliminates escape codes, unicode stuff,
//  etc that we aren't prepared to handle and might be passed to other
//  players maliciously.
static void sanitize_to_low_ascii(char *str)
{
    for (size_t i = 0; str[i]; i++) {
        if ((str[i] < 32) || (str[i] > 126)) {
            str[i] = ' ';
        }
    }
}

static void process_connection_command(Connection *conn)
{
    conn->inputbuf[conn->inputbuf_used] = '\0';  // null-terminate the input.
    sanitize_to_low_ascii(conn->inputbuf);
    trim(conn->inputbuf);

    loginfo("New input from socket %d%s: '%s'", conn->sock, conn->blocked ? " (blocked)" : "", conn->inputbuf);

    if (conn->blocked) {
        return;  // don't process this input further.
    }

    conn->inputfn(conn, conn->inputbuf);
    if (conn->state == CONNSTATE_READY) {
        if (conn->inputfn != inpfn_ingame) {  // if in-game, the Z-Machine writes a prompt itself.
            write_to_connection(conn, "\n>");  // prompt.
        }
    }
}

// this queues data from the actual socket, and if there's a complete command,
//  we process it in here. We only read a little at a time instead of reading
//  until the socket is empty to give everyone a chance.
static void recv_from_connection(Connection *conn)
{
    if (conn->state != CONNSTATE_READY) {
        return;
    }

    char buf[128];
    const int br = recv(conn->sock, buf, sizeof (buf), 0);
    //loginfo("Got %d from recv on socket %d", br, conn->sock);
    if (br == -1) {
        if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
            return;  // okay, just means there's nothing else to read.
        }
        loginfo("Socket %d has an error while receiving, dropping. (%s)", conn->sock, strerror(errno));
        drop_connection(conn);  // some other problem.
        return;
    } else if (br == 0) {  // socket has disconnected.
        loginfo("Socket %d has disconnected.", conn->sock);
        drop_connection(conn);
        return;
    }

    conn->last_activity = GNow;

    int avail = (int) ((sizeof (conn->inputbuf) - 1) - conn->inputbuf_used);
    for (int i = 0; i < br; i++) {
        const char ch = buf[i];
        if (((unsigned char) ch) == 255) {  // telnet Interpret As Command byte.
            i++;
            // !!! FIXME: fails on a buffer edge.
            if (i < br) {
                if (((unsigned char) buf[i]) == 253) {  // DO
                    i++;
                    // !!! FIXME: fails on a buffer edge.
                    if (i < br) {
                        const unsigned char wont[4] = { 255, 252, (unsigned char) buf[i], 0 };  // WONT do requested action.
                        write_to_connection(conn, (const char *) wont);
                    }
                } else if (((unsigned char) buf[i]) >= 250) {  // ignore everything else.
                    i++;
                }
            }
            continue;
        } else if (ch == '\n') {
            if (conn->overlong_input) {
                loginfo("Overlong input from socket %d", conn->sock);
                write_to_connection(conn, "Whoa, you're typing too much. Shorter commands, please.\n\n>");
            } else {
                process_connection_command(conn);
            }
            conn->overlong_input = 0;
            conn->inputbuf_used = 0;
            avail = sizeof (conn->inputbuf) - 1;
        } else if ((ch >= 32) && (ch < 127)) {  // basic ASCII only, sorry.
            if (!avail) {
                conn->overlong_input = 1;  // drop this command.
            } else {
                conn->inputbuf[conn->inputbuf_used++] = ch;
                avail--;
            }
        }
    }
}

// this sends data queued by write_to_connection() down the actual socket.
static void send_to_connection(Connection *conn)
{
    if (conn->outputbuf_used == 0) {
        return;  // nothing to send atm.
    } else if (conn->state > CONNSTATE_DRAINING) {
        conn->outputbuf_used = 0;  // just make sure we don't poll() for this again.
        return;
    }

    const ssize_t bw = send(conn->sock, conn->outputbuf, conn->outputbuf_used, 0);
    if ((bw == -1) && ((errno == EAGAIN) || (errno == EWOULDBLOCK))) {
        return;  // okay, just means try again later.
    } else if (bw <= 0) {
        if (bw == -1) {
            loginfo("Socket %d has an error while sending, dropping. (%s)", conn->sock, strerror(errno));
        } else {
            loginfo("Socket %d has disconnected without warning.", conn->sock);
        }
        drop_connection(conn);  // some other problem.
        if (conn->state == CONNSTATE_DRAINING) {
            conn->state = CONNSTATE_CLOSING;  // give up.
            conn->outputbuf_used = 0;
        }
        return;
    }

    // Still here? We got more data.

    assert(bw <= conn->outputbuf_used);

    if (((uint32) bw) < conn->outputbuf_used) {  /* !!! FIXME: lazy, not messing with pointers here. */
        memmove(conn->outputbuf, conn->outputbuf + bw, conn->outputbuf_used - bw);
    }

    conn->outputbuf_used -= (uint32) bw;

    if ((conn->state == CONNSTATE_DRAINING) && (conn->outputbuf_used == 0)) {
        loginfo("Finished draining output buffer for socket %d, moving to close.", conn->sock);
        conn->state = CONNSTATE_CLOSING;
    }
}

static int accept_new_connection(const int listensock)
{
    Connection *conn = NULL;
    struct sockaddr_storage addr;
    socklen_t addrlen = (socklen_t) sizeof (addr);
    const int sock = accept(listensock, (struct sockaddr *) &addr, &addrlen);
    if (sock == -1) {
        loginfo("accept() reported an error! We ignore it! (%s)", strerror(errno));
        return -1;
    }

    if (fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK) == -1) {
        loginfo("Failed to set newly-accept()'d socket as non-blocking! Dropping! (%s)", strerror(errno));
        close(sock);
        return -1;
    }

    void *ptr = realloc(connections, sizeof (*connections) * (num_connections + 1));
    if (!ptr) {
        loginfo("Uhoh, out of memory, dropping new connection in socket %d!", sock);
        close(sock);
        return -1;
    }
    connections = (Connection **) ptr;

    connections[num_connections] = (Connection *) calloc(1, sizeof (*conn));
    conn = connections[num_connections];
    if (conn == NULL) {
        loginfo("Uhoh, out of memory, dropping new connection in socket %d!", sock);
        close(sock);
        return -1;
    }

    num_connections++;

    conn->sock = sock;
    conn->inputfn = inpfn_hello_sailor;
    conn->last_activity = GNow;

    if (getnameinfo((struct sockaddr *) &addr, addrlen, conn->address, sizeof (conn->address), NULL, 0, NI_NUMERICHOST|NI_NUMERICSERV) != 0) {
        snprintf(conn->address, sizeof (conn->address), "???");
    }

    loginfo("New connection from %s (socket %d). %d current connections.", conn->address, sock, num_connections);

    const sqlite_int64 blocked_timestamp = db_select_blocked(conn->address);
    const int block_length = (int) (((sqlite_int64) GNow) - blocked_timestamp);
    if (blocked_timestamp && (block_length < MULTIZORK_BLOCKED_TIMEOUT)) {
        loginfo("Address %s (socket %d) is blocked for %d more seconds, dropping.", conn->address, sock, MULTIZORK_BLOCKED_TIMEOUT - block_length);
        write_to_connection(conn, "Sorry, this address is currently blocked.\n");
        drop_connection(conn);
    } else {
        write_to_connection(conn, "\n" MULTIZORK_TRANSCRIPT_BASEURL "\n");
        write_to_connection(conn, "(version " MULTIZORKD_VERSION " built " __DATE__ " " __TIME__ ".)\n\n\n");
        write_to_connection(conn, "Hello sailor!\n\nIf you are returning, go ahead and type in your access code.\nOtherwise, just press enter.\n\n>");
    }

    return sock;
}

static int prep_listen_socket(const int port, const int backlog)
{
    char service[32];
    const int one = 1;
    struct addrinfo hints;
    struct addrinfo *ainfo = NULL;

    memset(&hints, '\0', sizeof (hints));
    hints.ai_family = AF_INET6;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_V4MAPPED | AI_NUMERICSERV | AI_PASSIVE;    // AI_PASSIVE for the "any" address.
    snprintf(service, sizeof (service), "%u", (uint) port);
    const int gairc = getaddrinfo(NULL, service, &hints, &ainfo);
    if (gairc != 0) {
        loginfo("getaddrinfo() failed to find where we should bind! (%s)", gai_strerror(gairc));
        return -1;
    }

    for (struct addrinfo *i = ainfo; i != NULL; i = i->ai_next) {
        const int fd = socket(i->ai_family, i->ai_socktype, i->ai_protocol);
        if (fd == -1) {
            loginfo("socket() didn't create a listen socket! Will try other options! (%s)", strerror(errno));
            continue;
        }

        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *) &one, sizeof (one)) == -1) {
            loginfo("Failed to setsockopt(SO_REUSEADDR) the listen socket! Will try other options! (%s)", strerror(errno));
            close(fd);
            continue;
        } else if (bind(fd, i->ai_addr, i->ai_addrlen) == -1) {
            loginfo("Failed to bind() the listen socket! Will try other options! (%s)", strerror(errno));
            close(fd);
            continue;
        } else if (listen(fd, backlog) == -1) {
            loginfo("Failed to listen() on the listen socket! Will try other options! (%s)", strerror(errno));
            close(fd);
            continue;
        }

        freeaddrinfo(ainfo);
        return fd;
    }

    loginfo("Failed to create a listen socket on any reasonable interface.");
    freeaddrinfo(ainfo);
    return -1;
}

static void loadInitialStory(const char *fname)
{
    uint8 *story;
    FILE *io;
    long len;

    if (!fname) {
        panic("USAGE: multizorkd <story_file>");
    } else if ((io = fopen(fname, "rb")) == NULL) {
        panic("Failed to open '%s'", fname);
    } else if ((fseek(io, 0, SEEK_END) == -1) || ((len = ftell(io)) == -1)) {
        panic("Failed to determine size of '%s'", fname);
    } else if ((story = (uint8 *) malloc(len)) == NULL) {
        panic("Out of memory");
    } else if ((fseek(io, 0, SEEK_SET) == -1) || (fread(story, len, 1, io) != 1)) {
        panic("Failed to read '%s'", fname);
    }

    fclose(io);

    GOriginalStoryName = fname;
    GOriginalStory = story;
    GOriginalStoryLen = (uint32) len;
}


static int GStopServer = 0;
static void signal_handler_shutdown(int sig)
{
    if (GStopServer == 0) {
        loginfo("PROCESS RECEIVED SIGNAL %d, SHUTTING DOWN!", sig);
        GStopServer = 1;
    }
}

static void drop_privileges(const gid_t egid, const uid_t euid)
{
    // this is a list I took from another daemon. Dunno if it's a good list.
    static const char *deleteenvs[] = { "PATH", "IFS", "CDPATH", "ENV", "BASH_ENV" };
    for (int i = 0; i < ARRAYSIZE(deleteenvs); i++) {
        unsetenv(deleteenvs[i]);
    }

    if (geteuid() == 0) {  /* if root... */
        if (!egid && !euid) {
            loginfo("");
            loginfo("WARNING: YOU ARE RUNNING AS ROOT BUT NOT DROPPING PRIVILEGES!");
            loginfo("WARNING: RESTART THIS PROCESS WITH THE --gid and --uid OPTIONS.");
            loginfo("");
        }

        if (egid) {
            if (setegid(egid) == -1) {
                panic("Couldn't set effective GID to %d: %s", (int) egid, strerror(errno));
            }
            loginfo("Set effective group id to %d", (int) egid);
        }

        if (euid) {
            if (seteuid(euid) == -1) {
                panic("Couldn't set effective UID to %d: %s", (int) euid, strerror(errno));
            }
            loginfo("Set effective user id to %d", (int) euid);
        }
    }
}

int main(int argc, char **argv)
{
    const char *storyfname = NULL;
    int port = MULTIZORKD_DEFAULT_PORT;
    int backlog = MULTIZORKD_DEFAULT_BACKLOG;
    gid_t egid = MULTIZORKD_DEFAULT_EGID;
    uid_t euid = MULTIZORKD_DEFAULT_EUID;

    setvbuf(stdout, NULL, _IOLBF, 0);  // make sure output is line-buffered.
    setvbuf(stderr, NULL, _IOLBF, 0);  // make sure output is line-buffered.

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--gid") == 0) {
            i++;
            egid = (gid_t) (argv[i] ? atoi(argv[i]) : 0);
        } else if (strcmp(arg, "--uid") == 0) {
            i++;
            euid = (uid_t) (argv[i] ? atoi(argv[i]) : 0);
        } else if (strcmp(arg, "--port") == 0) {
            i++;
            port = argv[i] ? atoi(argv[i]) : 0;
        } else if (strcmp(arg, "--backlog") == 0) {
            i++;
            backlog = argv[i] ? atoi(argv[i]) : 0;
        } else {
            if (storyfname != NULL) {
                panic("Tried to choose two story files! '%s' and '%s'", storyfname, arg);
            }
            storyfname = arg;
        }
    }

    if (!storyfname) {
        storyfname = "zork1.dat";
    }

    GNow = time(NULL);
    srandom((unsigned long) GNow);

    loginfo("multizork daemon " MULTIZORKD_VERSION " (built " __DATE__ " " __TIME__ ") starting up...");

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, signal_handler_shutdown);
    signal(SIGTERM, signal_handler_shutdown);
    signal(SIGQUIT, signal_handler_shutdown);

    loadInitialStory(storyfname);

    db_init();

    struct pollfd *pollfds = NULL;

    pollfds = (struct pollfd *) calloc(1, sizeof (struct pollfd));
    if (!pollfds) {
        panic("Out of memory creating pollfd array!");
    }

    const int listensock = prep_listen_socket(port, backlog);
    if (listensock == -1) {
        panic("Can't go on without a listen socket!");
    }

    drop_privileges(egid, euid);

    loginfo("Running with story '%s'", storyfname);
    loginfo("Now accepting connections on port %d (socket %d).", port, listensock);

    while (GStopServer < 3) {
        pollfds[0].fd = listensock;
        pollfds[0].events = POLLIN | POLLOUT;
        for (size_t i = 0; i < num_connections; i++) {
            const Connection *conn = connections[i];
            pollfds[i+1].fd = conn->sock;
            pollfds[i+1].events = (conn->outputbuf_used > 0) ? (POLLIN | POLLOUT) : POLLIN;
            pollfds[i+1].revents = 0;
        }

        int pollrc;
        if (GStopServer && !num_connections) {
            pollrc = 0;
        } else if (GStopServer) {
            pollrc = poll(pollfds + 1, num_connections, -1);
        } else {
            pollrc = poll(pollfds, num_connections + 1, -1);
        }

        if (pollrc == -1) {
            if (errno != EINTR) {  /* ignore EINTR and just run the loop. */
                panic("poll() reported an error! (%s). Giving up.", strerror(errno));
            }
        }

        GNow = time(NULL);

        for (size_t i = 0; i <= num_connections; i++) {
            const short revents = pollfds[i].revents;
            if (revents == 0) { continue; }  // nothing happening here.
            if (pollfds[i].fd < 0) { continue; }   // not a socket in use.

            //loginfo("New activity on socket %d", pollfds[i].fd);

            if (i == 0) {  // new connection.
                assert(pollfds[0].fd == listensock);
                if (revents & POLLERR) {
                    panic("Listen socket had an error! Giving up!");
                }
                assert(revents & POLLIN);
                const int sock = accept_new_connection(listensock);
                if (sock != -1) {
                    void *ptr = realloc(pollfds, sizeof (struct pollfd) * (num_connections + 1));
                    if (ptr == NULL) {
                        close(sock);  // just drop them, oh well.
                        num_connections--;
                        loginfo("Uhoh, out of memory reallocating pollfds!");
                    } else {
                        pollfds = (struct pollfd *) ptr;
                        pollfds[num_connections].fd = sock;
                        pollfds[num_connections].events = POLLIN;
                        pollfds[num_connections].revents = 0;
                    }
                }
            } else {
                Connection *conn = connections[i-1];
                if (revents & POLLIN) {
                    recv_from_connection(conn);
                }
                if (revents & POLLOUT) {
                    send_to_connection(conn);
                }
            }
        }

        // cleanup any done sockets.
        for (size_t i = 0; i < num_connections; i++) {
            Connection *conn = connections[i];

            #if 0  // !!! FIXME: maybe add this?
            if ((conn->state == CONNSTATE_READY) && ((GNow - conn->last_activity) > IDLE_KICK_TIMEOUT))
                write_to_connection(conn, "Dropping you because you seem to be AFK.");
                drop_connection(conn);
            }
            #endif

            if (conn->state == CONNSTATE_CLOSING) {
                const int rc = (conn->sock < 0) ? 0 : close(conn->sock);
                // closed, or failed for a reason other than still trying to flush final writes, dump it.
                if ((rc == 0) || ((errno != EAGAIN) && (errno != EWOULDBLOCK))) {
                    loginfo("Closed socket %d, removing connection object. %d current connections.", conn->sock, num_connections-1);
                    free(conn->outputbuf);
                    if (i != (num_connections-1)) {
                        memmove(connections + i, connections + i + 1, sizeof (*connections) * ((num_connections - i) - 1));
                    }
                    i--;
                    num_connections--;
                }
            }
        }

        if (GStopServer == 1) {
            GStopServer = 2;
            for (size_t i = 0; i < num_connections; i++) {
                Connection *conn = connections[i];
                Instance *inst = conn->instance;
                write_to_connection(conn, "\n\n\nThis server is shutting down!\n\n");
                if (inst && inst->started) {
                    const Player *player = find_connection_player(conn, NULL);
                    if (player) {
                        write_to_connection(conn, "When the server comes back up, you can rejoin this game with this code:\n");
                        write_to_connection(conn, "    ");
                        write_to_connection(conn, player->hash);
                        write_to_connection(conn, "\n\n");
                        write_to_connection(conn, "And view transcripts from this game here:\n");
                        write_to_connection(conn, "    ");
                        write_to_connection(conn, MULTIZORK_TRANSCRIPT_BASEURL);
                        write_to_connection(conn, "/game/");
                        write_to_connection(conn, inst->hash);
                        write_to_connection(conn, "\n\n");
                    }
                }
            }

            for (size_t i = 0; i < num_connections; i++) {
                Connection *conn = connections[i];
                if (conn->instance) {
                    free_instance(conn->instance);  // might drop several people.
                } else {
                    drop_connection(conn);
                }
            }
        } else if (GStopServer == 2) {
            if (num_connections == 0) {  // !!! FIXME or too much time has passed.
                GStopServer = 3;
            }
        }
    }

    // shutdown!

    loginfo("Final shutdown happening...");

    close(listensock);

    for (size_t i = 0; i < num_connections; i++) {
        if (connections[i]->sock >= 0) {
            close(connections[i]->sock);
        }
        free(connections[i]->outputbuf);
        free(connections[i]);
    }

    free(connections);
    free(pollfds);
    free(GOriginalStory);

    db_quit();

    loginfo("Your score is 350 (total of 350 points), in 371 moves.");
    loginfo("This gives you the rank of Master Adventurer.");

    return 0;
}

// end of multizorkd.c ...

