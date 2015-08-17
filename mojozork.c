// The Z-Machine specifications 1.1:
//     http://inform-fiction.org/zmachine/standards/z1point1/index.html

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdint.h>

#define NORETURN __attribute__((noreturn))

static inline void dbg(const char *fmt, ...)
{
#if 1
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
#endif
} // dbg

#define FIXME(what) { \
    static int seen = 0; \
    if (!seen) { \
        seen = 1; \
        dbg("FIXME: %s\n", what); \
    } \
}

// the "_t" drives me nuts.  :/
typedef uint8_t uint8;
typedef int8_t sint8;
typedef uint16_t uint16;
typedef int16_t sint16;
typedef uint32_t uint32;
typedef int32_t sint32;
typedef uint_fast8_t uint8fast;
typedef int_fast8_t sint8fast;
typedef uint_fast16_t uint16fast;
typedef int_fast16_t sint16fast;
typedef uint_fast32_t uint32fast;
typedef int_fast32_t sint32fast;

typedef size_t uintptr;

typedef struct ZHeader
{
    uint8fast version;
    uint8fast flags1;
    uint16fast release;
    uint16fast himem_addr;
    uint16fast pc_start;  // in ver6, packed address of main()
    uint16fast dict_addr;
    uint16fast objtab_addr;
    uint16fast globals_addr;
    uint16fast staticmem_addr;  // offset of static memory, also: size of dynamic mem.
    uint16fast flags2;
    char serial_code[7];  // six ASCII chars in ver2. In ver3+: ASCII of completion date: YYMMDD
    uint16fast abbrtab_addr;  // abbreviations table
    uint16fast story_len;
    uint16fast story_checksum;
    // !!! FIXME: more fields here, all of which are ver4+
} ZHeader;

static uint8 *GStory = NULL;
static uintptr GStoryLen = 0;
static ZHeader GHeader;
static uint8 *GPC = 0;
static int GQuit = 0;

static void die(const char *fmt, ...) NORETURN;
static void die(const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "\nERROR: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    fflush(stderr);
    fflush(stdout);

    _exit(1);
} // die

static void loadStory(const char *fname)
{
    FILE *io;
    off_t len;

    if (!fname)
        die("USAGE: mojozork <story_file>");
    else if ((io = fopen(fname, "rb")) == NULL)
        die("Failed to open '%s'", fname);
    else if ((fseeko(io, 0, SEEK_END) == -1) || ((len = ftello(io)) == -1))
        die("Failed to determine size of '%s'", fname);
    else if ((GStory = malloc(len)) == NULL)
        die("Out of memory");
    else if ((fseeko(io, 0, SEEK_SET) == -1) || (fread(GStory, len, 1, io) != 1))
        die("Failed to read '%s'", fname);

    memset(&GHeader, '\0', sizeof (GHeader));
    const uint8 *ptr = GStory;

    #define READUI8() *(ptr++)
    #define READUI16() (((uint16fast) ptr[0]) << 8) | ((uint16fast) ptr[1]); ptr += sizeof (uint16)
    GHeader.version = READUI8();
    GHeader.flags1 = READUI8();
    GHeader.release = READUI16();
    GHeader.himem_addr = READUI16();
    GHeader.pc_start = READUI16();
    GHeader.dict_addr = READUI16();
    GHeader.objtab_addr = READUI16();
    GHeader.globals_addr = READUI16();
    GHeader.staticmem_addr = READUI16();
    GHeader.flags2 = READUI16();
    GHeader.serial_code[0] = READUI8();
    GHeader.serial_code[1] = READUI8();
    GHeader.serial_code[2] = READUI8();
    GHeader.serial_code[3] = READUI8();
    GHeader.serial_code[4] = READUI8();
    GHeader.serial_code[5] = READUI8();
    GHeader.abbrtab_addr = READUI16();
    GHeader.story_len = READUI16();
    GHeader.story_checksum = READUI16();
    #undef READUI8
    #undef READUI16

    fclose(io);
    GStoryLen = (uintptr) len;
} // loadStory

typedef void (*OpcodeFn)(void);


//#define VERIFY_OPCODE(name, minver)
static inline void VERIFY_OPCODE(const char *name, const uint16fast minver)
{
    if (GHeader.version < minver)
    {
        const uint8 *op = GPC-1;
        die("Opcode #%u ('%s') not available in version %u at pc %u",
            (unsigned int) *op, name, (unsigned int) GHeader.version,
            (unsigned int) (GStory-op));
    } // if
} // VERIFY_OPCODE

static void opcode_call(void)
{
    VERIFY_OPCODE("call", 1);
    die("write me");
} // opcode_call


typedef struct
{
    const char *name;
    OpcodeFn fn;
} Opcode;

// this is kinda wasteful (there are 120 opcodes scattered around these),
//  but it simplifies some things to just have a big linear array.
static Opcode GOpcodes[256];
static Opcode GExtendedOpcodes[30];

static void runInstruction(void)
{
    FIXME("lots of missing instructions here.  :)");
    FIXME("verify PC is sane");
    uint8 opcode = *(GPC++);

    const int extended = (opcode == 190) ? 1 : 0;
    if (extended)
    {
        opcode = *(GPC++);
        if (opcode >= (sizeof (GExtendedOpcodes) / sizeof (GExtendedOpcodes[0])))
            die("Unsupported or unknown extended opcode #%u", (unsigned int) opcode);
    } // if

    const Opcode *op = extended ? &GExtendedOpcodes[opcode] : &GOpcodes[opcode];

    if (!op->name)
        die("Unsupported or unknown %sopcode #%u", extended ? "extended " : "", (unsigned int) opcode);
    else if (!op->fn)
        die("Unimplemented %sopcode #%d ('%s')", extended ? "extended " : "", (unsigned int) opcode, op->name);
    else
    {
        dbg("pc=%u %sopcode=%u ('%s')\n", (((unsigned int) (GStory-GPC))-1) - extended, extended ? "ext " : "", opcode, op->name);
        op->fn();
    } // else
} // runInstruction

static void initOpcodeTable(const uint8fast version)
{
    memset(GOpcodes, '\0', sizeof (GOpcodes));
    memset(GExtendedOpcodes, '\0', sizeof (GExtendedOpcodes));

    Opcode *opcodes = GOpcodes;

    #define OPCODE(num, opname) opcodes[num].name = #opname; opcodes[num].fn = opcode_##opname
    #define OPCODE_WRITEME(num, opname) opcodes[num].name = #opname

    // this is the basic ver1 opcode table, then we can patch it after.
    // most early Infocom games are version 3, but apparently ver1 is in the wild...

    // 2-operand instructions...
    OPCODE_WRITEME(1, je);
    OPCODE_WRITEME(2, jl);
    OPCODE_WRITEME(3, jg);
    OPCODE_WRITEME(4, dec_chk);
    OPCODE_WRITEME(5, inc_chk);
    OPCODE_WRITEME(6, jin);
    OPCODE_WRITEME(7, test);
    OPCODE_WRITEME(8, or);
    OPCODE_WRITEME(9, and);
    OPCODE_WRITEME(10, test_attr);
    OPCODE_WRITEME(11, set_attr);
    OPCODE_WRITEME(12, clear_attr);
    OPCODE_WRITEME(13, store);
    OPCODE_WRITEME(14, insert_obj);
    OPCODE_WRITEME(15, loadw);
    OPCODE_WRITEME(16, loadb);
    OPCODE_WRITEME(17, get_prop);
    OPCODE_WRITEME(18, get_prop_addr);
    OPCODE_WRITEME(19, get_next_prop);
    OPCODE_WRITEME(20, add);
    OPCODE_WRITEME(21, sub);
    OPCODE_WRITEME(22, mul);
    OPCODE_WRITEME(23, div);
    OPCODE_WRITEME(24, mod);

    // 1-operand instructions...
    OPCODE_WRITEME(128, jz);
    OPCODE_WRITEME(129, get_sibling);
    OPCODE_WRITEME(130, get_child);
    OPCODE_WRITEME(131, get_parent);
    OPCODE_WRITEME(132, get_prop_len);
    OPCODE_WRITEME(133, inc);
    OPCODE_WRITEME(134, dec);
    OPCODE_WRITEME(135, print_addr);
    OPCODE_WRITEME(137, remove_obj);
    OPCODE_WRITEME(138, print_obj);
    OPCODE_WRITEME(139, ret);
    OPCODE_WRITEME(140, jump);
    OPCODE_WRITEME(141, print_paddr);
    OPCODE_WRITEME(142, load);
    OPCODE_WRITEME(143, not);

    // 0-operand instructions...
    OPCODE_WRITEME(176, rtrue);
    OPCODE_WRITEME(177, rfalse);
    OPCODE_WRITEME(178, print);
    OPCODE_WRITEME(179, print_ret);
    OPCODE_WRITEME(180, nop);
    OPCODE_WRITEME(181, save);
    OPCODE_WRITEME(182, restore);
    OPCODE_WRITEME(183, restart);
    OPCODE_WRITEME(184, ret_popped);
    OPCODE_WRITEME(185, pop);
    OPCODE_WRITEME(186, quit);
    OPCODE_WRITEME(187, new_line);

    // variable operand instructions...
    OPCODE(224, call);
    OPCODE_WRITEME(225, storew);
    OPCODE_WRITEME(226, storeb);
    OPCODE_WRITEME(227, put_prop);
    OPCODE_WRITEME(228, sread);
    OPCODE_WRITEME(229, print_char);
    OPCODE_WRITEME(230, print_num);
    OPCODE_WRITEME(231, random);
    OPCODE_WRITEME(232, push);
    OPCODE_WRITEME(233, pull);

    if (version < 3)  // most early Infocom games are version 3.
        return;  // we're done.

    OPCODE_WRITEME(188, show_status);
    OPCODE_WRITEME(189, verify);
    OPCODE_WRITEME(234, split_window);
    OPCODE_WRITEME(235, set_window);
    OPCODE_WRITEME(243, output_stream);
    OPCODE_WRITEME(244, input_stream);
    OPCODE_WRITEME(245, sound_effect);

    if (version < 4)
        return;  // we're done.

    OPCODE_WRITEME(25, call_2s);
    OPCODE_WRITEME(180, save_ver4);
    OPCODE_WRITEME(224, call_vs);;
    OPCODE_WRITEME(228, sread_ver4);
    OPCODE_WRITEME(236, call_vs2);
    OPCODE_WRITEME(237, erase_window);
    OPCODE_WRITEME(238, erase_line);
    OPCODE_WRITEME(239, set_cursor);
    OPCODE_WRITEME(240, get_cursor);
    OPCODE_WRITEME(241, set_text_style);
    OPCODE_WRITEME(242, buffer_mode);
    OPCODE_WRITEME(246, read_char);
    OPCODE_WRITEME(247, scan_table);

    // this is the "show_status" opcode in ver3; illegal in ver4+.
    opcodes[188].name = NULL;
    opcodes[188].fn = NULL;

    if (version < 5)
        return;  // we're done.

    OPCODE_WRITEME(26, call_2n);
    OPCODE_WRITEME(27, set_colour);
    OPCODE_WRITEME(28, throw);
    OPCODE_WRITEME(136, call_1s);
    OPCODE_WRITEME(143, call_1n);
    OPCODE_WRITEME(185, catch);
    OPCODE_WRITEME(191, piracy);
    OPCODE_WRITEME(228, aread);
    OPCODE_WRITEME(243, output_stream_ver5);
    OPCODE_WRITEME(245, sound_effect_ver5);
    OPCODE_WRITEME(248, not_ver5);
    OPCODE_WRITEME(249, call_vn);
    OPCODE_WRITEME(250, call_vn2);
    OPCODE_WRITEME(251, tokenise);
    OPCODE_WRITEME(252, encode_text);
    OPCODE_WRITEME(253, copy_table);
    OPCODE_WRITEME(254, print_table);
    OPCODE_WRITEME(255, check_arg_count);

    // this is the "save" and "restore" opcodes in ver1-4; illegal in ver5+.
    //  in ver5+, they use extended opcode 0 and 1 for these.
    opcodes[180].name = opcodes[181].name = NULL;
    opcodes[180].name = opcodes[181].name = NULL;

    // We special-case this later, so no function pointer supplied.
    opcodes[190].name = "extended";

    // extended opcodes in ver5+ ...
    opcodes = GExtendedOpcodes;
    OPCODE_WRITEME(0, save_ext);
    OPCODE_WRITEME(1, restore_ext);
    OPCODE_WRITEME(2, log_shift);
    OPCODE_WRITEME(3, art_shift);
    OPCODE_WRITEME(4, set_font);
    OPCODE_WRITEME(9, save_undo);
    OPCODE_WRITEME(10, restore_undo);
    OPCODE_WRITEME(11, print_unicode);
    OPCODE_WRITEME(12, check_unicode);
    OPCODE_WRITEME(13, set_true_colour);
    opcodes = GOpcodes;

    if (version < 6)
        return;  // we're done.

    OPCODE_WRITEME(27, set_colour_ver6);
    OPCODE_WRITEME(27, throw_ver6);
    OPCODE_WRITEME(185, catch_ver6);
    OPCODE_WRITEME(233, pull_ver6);
    OPCODE_WRITEME(238, erase_line_ver6);
    OPCODE_WRITEME(239, set_cursor_ver6);
    OPCODE_WRITEME(243, output_stream_ver6);
    OPCODE_WRITEME(248, not_ver6);

    opcodes = GExtendedOpcodes;
    OPCODE_WRITEME(4, set_font_ver6);
    OPCODE_WRITEME(5, draw_picture);
    OPCODE_WRITEME(6, picture_data);
    OPCODE_WRITEME(7, erase_picture);
    OPCODE_WRITEME(8, set_margins);
    OPCODE_WRITEME(13, set_true_colour_ver6);
    // 14 and 15 are unused.
    OPCODE_WRITEME(16, move_window);
    OPCODE_WRITEME(17, window_size);
    OPCODE_WRITEME(18, window_style);
    OPCODE_WRITEME(19, get_wind_prop);
    OPCODE_WRITEME(20, scroll_window);
    OPCODE_WRITEME(21, pop_stack);
    OPCODE_WRITEME(22, read_mouse);
    OPCODE_WRITEME(23, mouse_window);
    OPCODE_WRITEME(24, push_stack);
    OPCODE_WRITEME(25, put_wind_prop);
    OPCODE_WRITEME(26, print_form);
    OPCODE_WRITEME(27, make_menu);
    OPCODE_WRITEME(28, picture_table);
    OPCODE_WRITEME(29, buffer_screen);
    opcodes = GOpcodes;

    #undef OPCODE
    #undef OPCODE_WRITEME
} // initOpcodeTable

int main(int argc, char **argv)
{
    const char *fname = argv[1] ? argv[1] : "zork1.dat";
    loadStory(fname);

    dbg("Story '%s' header:\n", fname);
    dbg(" - version %u\n", (unsigned int) GHeader.version);
    dbg(" - flags 0x%X\n", (unsigned int) GHeader.flags1);
    dbg(" - release %u\n", (unsigned int) GHeader.release);
    dbg(" - high memory addr %u\n", (unsigned int) GHeader.himem_addr);
    dbg(" - program counter start %u\n", (unsigned int) GHeader.pc_start);
    dbg(" - dictionary address %u\n", (unsigned int) GHeader.dict_addr);
    dbg(" - object table address %u\n", (unsigned int) GHeader.objtab_addr);
    dbg(" - globals address %u\n", (unsigned int) GHeader.globals_addr);
    dbg(" - static memory address %u\n", (unsigned int) GHeader.staticmem_addr);
    dbg(" - flags2 0x%X\n", (unsigned int) GHeader.flags2);
    dbg(" - serial '%s'\n", GHeader.serial_code);
    dbg(" - abbreviations table address %u\n", (unsigned int) GHeader.abbrtab_addr);
    dbg(" - story length %u\n", (unsigned int) GHeader.story_len);
    dbg(" - story checksum 0x%X\n", (unsigned int) GHeader.story_checksum);

    if (GHeader.version != 3)
        die("FIXME: only version 3 is supported right now, this is %d", (int) GHeader.version);

    FIXME("in ver6+, this is the address of a main() routine, not a raw instruction address.");
    GPC = GStory + GHeader.pc_start;
    initOpcodeTable(GHeader.version);

    while (!GQuit)
        runInstruction();

    dbg("ok.\n");

    free(GStory);
    return 0;
} // main

// end of mojozork.c ...

