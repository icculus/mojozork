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

#define MULTIZORK 1
#include "mojozork.c"

#define MULTIZORKD_VERSION "0.0.1"
#define MULTIZORKD_PORT 23  /* telnet! */
#define MULTIZORKD_BACKLOG 64

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

static void generate_unique_hash(char *hash)  // `hash` points to up to 8 bytes of space.
{
    // this is kinda cheesy, but it's good enough.
    static const char chartable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    for (size_t i = 0; i < 6; i++) {
        hash[i] = chartable[((size_t) random()) % (sizeof (chartable)-1)];
    }
    hash[6] = '\0';
    // !!! FIXME: make sure this isn't a duplicate.
}

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
    char username[16];
    char hash[8];
    uint32 next_logical_pc;  // next step_instance() should run this player from this z-machine program counter.
    uint32 next_logical_sp;  // next step_instance() should run this player from this z-machine stack pointer.
    uint16 next_logical_bp;  // next step_instance() should run this player from this z-machine base pointer.
    uint16 stack[2048];      // Copy of the stack to restore for next step_instance()  // !!! FIXME: make this dynamic?
    uint8 *next_inputbuf;   // where to write the next input for this player.
    uint8 next_inputbuflen;
    uint16 next_operands[2];  // to save off the READ operands for later.
    uint8 object_table_data[9];
    uint8 property_table_data[MULTIPLAYER_PROP_DATALEN];
    // ZORK 1 SPECIFIC MAGIC: these are player-specific globals we need to manage.
    uint16 gvar_location;
    uint16 gvar_coffin_held;
    uint16 gvar_dead;
    uint16 gvar_deaths;
    // !!! FIXME: several more
} Player;

typedef struct Instance
{
    ZMachineState zmachine_state;
    int started;
    char hash[8];
    Player players[4];
    int num_players;
    int step_completed;
    int current_player;   //  the player we're currently running the z-machine for.
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
};

static Connection **connections = NULL;
static size_t num_connections = 0;

// This queues a string for sending over the connection's socket when possible.
static void write_to_connection(Connection *conn, const char *str)
{
    if (!conn || (conn->state != CONNSTATE_READY)) {
        return;
    }

    const size_t slen = strlen(str);
    const size_t avail = conn->outputbuf_len - conn->outputbuf_used;
    if (avail < slen) {
        void *ptr = realloc(conn->outputbuf, conn->outputbuf_len + slen);
        if (!ptr) {
            panic("Uhoh, out of memory in write_to_connection");  // !!! FIXME: we could handle this more gracefully.
        }
        conn->outputbuf = (char *) ptr;
        conn->outputbuf_len += slen;
    }
    memcpy(conn->outputbuf + conn->outputbuf_used, str, slen);
    conn->outputbuf_used += slen;
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
            write_to_connection(inst->players[i].connection, str);
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
            ptr += (*ptr * 2) + 1;  // skip object name to start of properties.
        }

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

static void writechar_multizork(const int ch)
{
    Instance *inst = (Instance *) GState;  // this works because zmachine_state is the first field in Instance.
    char str[2] = { (char) ch, '\0' };
    write_to_connection(get_current_player(inst)->connection, str);
    // !!! FIXME: log to database?
}

// see comments on getObjectProperty
static void opcode_get_prop_addr_multizork(void)
{
    Instance *inst = (Instance *) GState;  // this works because zmachine_state is the first field in Instance.
    uint8 *store = varAddress(*(GState->pc++), 1);
    const uint16 objid = remap_objectid(GState->operands[0]);
    const uint16 propid = GState->operands[1];
    const uint16 external_mem_objects_base = ZORK1_EXTERN_MEM_OBJS_BASE;  // ZORK 1 SPECIFIC MAGIC
    uint8 *ptr = getObjectProperty(objid, propid, NULL);
    uint16 result;

    if (objid >= external_mem_objects_base) {  // looking for a multiplayer character
        const uint16 fake_prop_base_addr = (uint16) (0x10000 - (MULTIPLAYER_PROP_DATALEN * 5));
        const int requested_player = (int) (objid - external_mem_objects_base);
        if (requested_player >= inst->num_players) {
            GState->die("Invalid multiplayer object id referenced");
        }
        result = fake_prop_base_addr + (MULTIPLAYER_PROP_DATALEN * requested_player);  // we give each player FAKE bytes at the end of the address space.
    } else {
        result = ptr ? ((uint16) (ptr-GState->story)) : 0;
    }
    WRITEUI16(store, result);
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
    inst->step_completed = 1;  // time to break out of the Z-Machine simulation loop.
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

static void opcode_quit_multizork(void)
{
    Instance *inst = (Instance *) GState;  // this works because zmachine_state is the first field in Instance.
    // !!! FIXME: decide if this was an expected endgame and die() as an error if not?
    GState->quit = 1;  // note that this should terminate the Z-Machine.
    inst->step_completed = 1;  // time to break out of the Z-Machine simulation loop.
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

    // !!! FIXME: log this crash to the database for later examination.

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
        generate_unique_hash(inst->hash);
        uint8 *story = (uint8 *) malloc(GOriginalStoryLen);
        if (!story) {
            free(inst);
            return NULL;
        }
        GState = &inst->zmachine_state;
        memcpy(story, GOriginalStory, GOriginalStoryLen);
        initStory(GOriginalStoryName, story, GOriginalStoryLen);
        for (size_t i = 0; i < ARRAYSIZE(inst->players); i++) {
            inst->players[i].next_logical_pc = GState->logical_pc;  // set all players to game entry point.
        }

        // override some Z-Machine opcode handlers we need...
        GState->opcodes[18].fn = opcode_get_prop_addr_multizork;
        GState->opcodes[181].fn = opcode_save_multizork;
        GState->opcodes[182].fn = opcode_restore_multizork;
        GState->opcodes[183].fn = opcode_restart_multizork;
        GState->opcodes[186].fn = opcode_quit_multizork;
        GState->opcodes[228].fn = opcode_read_multizork;

        for (uint8 i = 32; i <= 127; i++)  // 2OP opcodes repeating with different operand forms.
            GState->opcodes[i] = GState->opcodes[i % 32];
        for (uint8 i = 144; i <= 175; i++)  // 1OP opcodes repeating with different operand forms.
            GState->opcodes[i] = GState->opcodes[128 + (i % 16)];
        for (uint8 i = 192; i <= 223; i++)  // 2OP opcodes repeating with VAR operand forms.
            GState->opcodes[i] = GState->opcodes[i % 32];

        GState->writechar = writechar_multizork;
        GState->die = die_multizork;

        loginfo("Created instance '%s'", inst->hash);
    }
    return inst;
}

static int step_instance(Instance *inst, const int playernum, const char *input)
{
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
    globals[83] = player->gvar_coffin_held;
    globals[96] = player->gvar_dead;
    globals[99] = player->gvar_deaths;

    // If user had hit a READ instruction. Write the user's
    //  input to Z-Machine memory, and tokenize it.
    if (player->next_inputbuf) {
        snprintf((char *) player->next_inputbuf, player->next_inputbuflen-1, "%s", input ? input : "");
        // !!! FIXME: this is a hack. Blank out invalid characters.
        for (char *ptr =  (char *) player->next_inputbuf; *ptr; ptr++) {
            const char ch = *ptr;
            if ((ch >= 'A') && (ch <= 'Z')) {
                *ptr = 'a' + (ch - 'A');  // lowercase it.
            } else if ((ch >= 'a') && (ch <= 'z')) {
                /* cool */ ;
            } else if ((ch != ',') && (ch != '.') && (ch != ' ') && (ch != '\"')) {
                *ptr = ' ';  /* oh well. */
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
    // !!! FIXME

    // Now run the Z-Machine!
    if (setjmp(inst->jmpbuf) == 0) {  // !!! FIXME: can we dump die() so we don't need this?
        inst->step_completed = 0;  // opcode_quit or opcode_read, etc.
        while (!inst->step_completed) {
            runInstruction();
        }

        // ZORK 1 SPECIFIC MAGIC: mark the current player as visible so other players see him.
        // !!! FIXME

        // save off Z-Machine state for next time.
        player->next_logical_pc = GState->logical_pc;
        player->next_logical_sp = (uint32) (GState->sp - GState->stack);
        player->next_logical_bp = GState->bp;
        assert(player->next_logical_sp < ARRAYSIZE(player->stack));
        memcpy(player->stack, GState->stack, player->next_logical_sp * 2);

        // ZORK 1 SPECIFIC MAGIC:
        // some "globals" are player-specific, so we swap them out after running.
        player->gvar_location = globals[0];
        player->gvar_coffin_held = globals[83];
        player->gvar_dead = globals[96];
        player->gvar_deaths = globals[99];

        if (inst->zmachine_state.quit) {
            // we've hit a QUIT opcode, which means, at least for Zork 1, that the user
            //  has either beaten the game or died in an unrecoverable way.
            // Drop this player's connection. Let the others continue to play.
            // If player comes back, they'll just hit the QUIT opcode immediately and get dropped again.
            inst->zmachine_state.quit = 0;  // reset for next player.
            drop_connection(player->connection);
        }
    } else {
        // uhoh, the Z-machine called die(). Kill this instance.
        broadcast_to_instance(inst, "\n\n*** Oh no, this game instance had a fatal error, so we're jumping ship! ***\n\n\n");
        free_instance(inst);
        retval = 0;
    }

    GState = NULL;
    return retval;
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

    // !!! FIXME: write the instance and all players to the database.

    // !!! FIXME: split this out to a separate function.
    const uint8 *playerptr = GState->story + GState->header.objtab_addr;
    playerptr += 31 * sizeof (uint16);  // skip properties defaults table
    playerptr += 9 * (ZORK1_PLAYER_OBJID-1);  // find object in object table  // ZORK 1 SPECIFIC MAGIC

    const uint8 *propptr = playerptr + 7;  // skip to properties address field.
    const uint16 propaddr = READUI16(propptr);
    propptr = GState->story + propaddr;
    propptr += (*propptr * 2) + 1;  // skip object name to start of properties.
    uint16 propsize;
    for (propsize = 0; propptr[propsize]; propsize += ((propptr[propsize] >> 5) & 0x7) + 1) { /* spin */ }
    assert(propsize < sizeof (inst->players[0].property_table_data));

    for (size_t i = 0; i < num_players; i++) {
        Player *player = &inst->players[i];
        Connection *conn = player->connection;

        snprintf(player->username, sizeof (player->username), "%s", conn->username);

        // Copy the original player object for each player.
        memcpy(player->object_table_data, playerptr, sizeof (player->object_table_data));

        // Build a custom property table for each player.
        uint8 *propdst = player->property_table_data;
        propdst++ ;  // text-length (number of 2-byte words). Skip for now.

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

        generate_unique_hash(player->hash);  // assign a hash to the player while we're here so they can rejoin.
        snprintf(players->username, sizeof (player->username), "%s", conn->username);
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

    inst->started = 1;

    const uint16 external_mem_objects_base = ZORK1_EXTERN_MEM_OBJS_BASE;  // ZORK 1 SPECIFIC MAGIC
    GState = &inst->zmachine_state;
    uint8 *startroomptr = getObjectPtr(180);  // ZORK 1 SPECIFIC MAGIC: West of House room.
    GState = NULL;
    const uint8 orig_start_room_child = startroomptr[6];

    // run a step right now, so they get the intro text and their next input will be for the game.
    for (int i = 0; i < num_players; i++) {
        // just this once, reset the Z-Machine between each player, so that we end up with
        //  one definite state and things like intro text gets run...
        // !!! FIXME: this can just copy dynamic memory, not the whole thing.
        memcpy(inst->zmachine_state.story, GOriginalStory, GOriginalStoryLen);

        // ZORK 1 SPECIFIC MAGIC:
        // Insert all the players into the West of House room each time. The
        //  game will move the current player there on startup, but we're
        //  resetting dynamic memory between each step here, which wipes
        //  those moves.
        for (int j = 0; j < num_players; j++) {
            Player *player = &inst->players[j];
            uint8 *ptr = player->object_table_data;
            ptr[4] = 180;  // parent is West of House room.
            ptr[5] = (j < (num_players-1)) ? (external_mem_objects_base + j + 1) : orig_start_room_child;
            assert(ptr[6] == 0);  // assume player object has no initial children
        }
        startroomptr[6] = external_mem_objects_base;  // make players start of child list for start room.


        // Run until the READ instruction, then gameplay officially starts.
        if (!step_instance(inst, i, NULL)) {
            break;  // instance failed, don't access it further.
        }
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

    if (inst->started) {  // if game started, save the state. If not, just drop the resources.
        // !!! FIXME: write me
    }

    if (GState == &inst->zmachine_state) {
        GState = NULL;
    }

    free(inst->zmachine_state.story);
    free(inst->zmachine_state.story_filename);
    free(inst);
}

static void inpfn_confirm_quit(Connection *conn, const char *str)
{
    // !!! FIXME: can we just put a pointer to the Player in Connection?
    Instance *inst = conn->instance;
    Player *player = NULL;
    int playernum;
    for (playernum = 0; playernum < inst->num_players; playernum++) {
        if (inst->players[playernum].connection == conn) {
            player = &inst->players[playernum];
            break;
        }
    }

    if (strcasecmp(str, "y") == 0) {
        if (player != NULL) {
            write_to_connection(conn, "\nOkay, you can come back to this game in progress with this code:\n");
            write_to_connection(conn, "    ");
            write_to_connection(conn, player->hash);
            write_to_connection(conn, "\n");
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
    // !!! FIXME: can we just put a pointer to the Player in Connection?
    Instance *inst = conn->instance;
    Player *player = NULL;
    int playernum;
    for (playernum = 0; playernum < inst->num_players; playernum++) {
        if (inst->players[playernum].connection == conn) {
            player = &inst->players[playernum];
            break;
        }
    }

    if (!player) {
        loginfo("Um, socket %d is trying to talk to instance '%s', which it is not a player on.", conn->sock, inst->hash);
        write_to_connection(conn, "\n\n*** The server appears to be confused. This is a bug on our end. Sorry, dropping you now. ***\n\n\n");
        drop_connection(conn);
        return;
    }

    if ((strcasecmp(str, "q") == 0) || (strncasecmp(str, "quit", 4) == 0)) {
        write_to_connection(conn, "Do you wish to leave the game? (Y is affirmative):");
        conn->inputfn = inpfn_confirm_quit;
    } else if (strncasecmp(str, "save", 4) == 0) {
        write_to_connection(conn, "Requests to save the game are ignored, sorry.\n>");
    } else if (strncasecmp(str, "restore", 7) == 0) {
        write_to_connection(conn, "Requests to restory the game are ignored, sorry.\n>");
    } else if (str[0] == '!') {
        // !!! FIXME: filter to basic ASCII so they can't send terminal escape codes, etc.
        if (str[1] == '!') { // broadcast to whole instance
            broadcast_to_instance(inst, "\n*** ");
            broadcast_to_instance(inst, player->username);
            broadcast_to_instance(inst, " says to the whole dungeon, \"");
            broadcast_to_instance(inst, str + 2);
            broadcast_to_instance(inst, "\" ***\n>");
        } else {
            broadcast_to_room(inst, player->gvar_location, "\n*** ");
            broadcast_to_room(inst, player->gvar_location, player->username);
            broadcast_to_room(inst, player->gvar_location, " says to the room, \"");
            broadcast_to_room(inst, player->gvar_location, str + 1);
            broadcast_to_room(inst, player->gvar_location, "\" ***\n>");
        }
    } else {
        const uint16 loc = player->gvar_location;
        player->gvar_location = 0;  // so we don't broadcast to ourselves.
        broadcast_to_room(inst, loc, "\n*** ");
        broadcast_to_room(inst, loc, player->username);
        broadcast_to_room(inst, loc, " decides to \"");
        broadcast_to_room(inst, loc, str);
        broadcast_to_room(inst, loc, "\" ***\n>");
        player->gvar_location = loc;
        step_instance(conn->instance, playernum, str);  // run the Z-machine with new input.
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
        }
    }
}

static void inpfn_new_game_or_join(Connection *conn, const char *str)
{
    if (strcmp(str, "1") == 0) {  // new game
        assert(!conn->instance);
        conn->instance = create_instance();
        if (!conn->instance) {
            write_to_connection(conn, "Uhoh, we appear to be out of memory. Try again later?");
            drop_connection(conn);
            return;
        }
        conn->instance->players[0].connection = conn;
        write_to_connection(conn, "Okay! Tell your friends to telnet here, too, and join game '");
        write_to_connection(conn, conn->instance->hash);
        write_to_connection(conn, "'.\n\n");
        write_to_connection(conn, "We'll wait for them now.\n");
        write_to_connection(conn, "You can type 'go' to begin when enough have arrived.\n");
        write_to_connection(conn, "There's still room for three more people.\n");
        write_to_connection(conn, "Once you type 'go' no more will be admitted.\n");
        write_to_connection(conn, "Type 'quit' to drop this game and anyone connected.\n");
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

// First prompt after connecting.
static void inpfn_hello_sailor(Connection *conn, const char *str)
{
    if (*str == '\0') {  // just hit enter without a specific code?
        write_to_connection(conn, "Okay, let's get you set up.\n\n");
        write_to_connection(conn, "What's your name? Keep it simple or I'll simplify it for you.\n");
        write_to_connection(conn, "(sorry if you're name isn't one word made up of english letters.\n");
        write_to_connection(conn, " This is American tech from 1980, after all.)");
        conn->inputfn = inpfn_enter_name;
    } else {
        // look up player code.
        write_to_connection(conn, "!!! FIXME: write this part. Bye for now.");
        drop_connection(conn);
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

static void process_connection_command(Connection *conn)
{
    conn->inputbuf[conn->inputbuf_used] = '\0';  // null-terminate the input.
    trim(conn->inputbuf);
    loginfo("New input from socket %d: '%s'", conn->sock, conn->inputbuf);
    // !!! FIXME: write to database if in an instance
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
                if (((unsigned char) buf[i]) >= 250) {
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
            avail = sizeof (conn->inputbuf);
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

    loginfo("New connection from %s (socket %d)", conn->address, sock);

    write_to_connection(conn, "\n\nHello sailor!\n\nIf you are returning, go ahead and type in your access code.\nOtherwise, just press enter.\n\n>");

    return sock;
}

static int prep_listen_socket(const int port, const int backlog)
{
    char service[32];
    const int one = 1;
    struct addrinfo hints;
    struct addrinfo *ainfo = NULL;

    memset(&hints, '\0', sizeof (hints));
    hints.ai_family = AF_UNSPEC;    // Allow IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV | AI_PASSIVE;    // AI_PASSIVE for the "any" address.
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

// !!! FIXME: command line handling and less hardcoding.
int main(int argc, char **argv)
{
    const char *storyfname = (argc >= 2) ? argv[1] : "zork1.dat";
    GNow = time(NULL);
    srandom((unsigned long) GNow);

    loginfo("multizork daemon %s starting up...", MULTIZORKD_VERSION);

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, signal_handler_shutdown);
    signal(SIGTERM, signal_handler_shutdown);
    signal(SIGQUIT, signal_handler_shutdown);

    loadInitialStory(storyfname);

    struct pollfd *pollfds = NULL;

    pollfds = (struct pollfd *) calloc(1, sizeof (struct pollfd));
    if (!pollfds) {
        panic("Out of memory creating pollfd array!");
    }

    const int listensock = prep_listen_socket(MULTIZORKD_PORT, MULTIZORKD_BACKLOG);
    if (listensock == -1) {
        panic("Can't go on without a listen socket!");
    }

    //drop_privileges();

    //connect_to_database();

    loginfo("Now accepting connections on port %d (socket %d).", MULTIZORKD_PORT, listensock);

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
            pollrc = poll(pollfds + 1, num_connections, -1);  // !!! FIXME: timeout.
        } else {
            pollrc = poll(pollfds, num_connections + 1, -1);  // !!! FIXME: timeout.
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
                    loginfo("Closed socket %d, removing connection object.", conn->sock);
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
                    // !!! FIXME: a conn->player field would remove this search.
                    for (size_t j = 0; j < ARRAYSIZE(inst->players); j++) {
                        Player *player = &inst->players[j];
                        if (player->connection == conn) {
                            write_to_connection(conn, "When the server comes back up, you can rejoin this game with this code:\n");
                            write_to_connection(conn, "    ");
                            write_to_connection(conn, player->hash);
                            write_to_connection(conn, "\n\n");
                            break;
                        }
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

    loginfo("Your score is 350 (total of 350 points), in 371 moves.");
    loginfo("This gives you the rank of Master Adventurer.");

    return 0;
}

// end of multizorkd.c ...

