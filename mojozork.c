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


#define OPCODE(name) opname = #name; op = opcode_##name; break
#define OPCODE_WRITEME(name) opname = #name; break

// extended opcodes in ver5+ ...
static void opcode_extended(void)
{
    VERIFY_OPCODE("extended", 5);

    FIXME("lots of missing instructions here.  :)");
    FIXME("verify PC is sane");
    const uint8 opcode = *(GPC++);
    const OpcodeFn op = NULL;
    const char *opname = NULL;

    switch (opcode)
    {
        case 0: OPCODE_WRITEME(save_ext);
        case 1: OPCODE_WRITEME(restore);
        case 2: OPCODE_WRITEME(log_shift);
        case 3: OPCODE_WRITEME(art_shift);
        case 4: OPCODE_WRITEME(set_font);
        case 5: OPCODE_WRITEME(draw_picture);
        case 6: OPCODE_WRITEME(picture_data);
        case 7: OPCODE_WRITEME(erase_picture);
        case 8: OPCODE_WRITEME(set_margins);
        case 9: OPCODE_WRITEME(save_undo);
        case 10: OPCODE_WRITEME(restore_undo);
        case 11: OPCODE_WRITEME(print_unicode);
        case 12: OPCODE_WRITEME(check_unicode);
        case 13: OPCODE_WRITEME(set_true_colour);
        // 14 and 15 are unused.
        case 16: OPCODE_WRITEME(move_window);
        case 17: OPCODE_WRITEME(window_size);
        case 18: OPCODE_WRITEME(window_style);
        case 19: OPCODE_WRITEME(get_wind_prop);
        case 20: OPCODE_WRITEME(scroll_window);
        case 21: OPCODE_WRITEME(pop_stack);
        case 22: OPCODE_WRITEME(read_mouse);
        case 23: OPCODE_WRITEME(mouse_window);
        case 24: OPCODE_WRITEME(push_stack);
        case 25: OPCODE_WRITEME(put_wind_prop);
        case 26: OPCODE_WRITEME(print_form);
        case 27: OPCODE_WRITEME(make_menu);
        case 28: OPCODE_WRITEME(picture_table);
        case 29: OPCODE_WRITEME(buffer_screen);
        default:
            die("Unsupported or unknown extended opcode #%u", (unsigned int) opcode);
    } // switch

    if (!op)
        die("Unimplemented extended opcode #%d ('%s')", (unsigned int) opcode, opname);

    dbg("Run extended opcode %u ('%s') ...\n", opcode, opname);
    op();
} // opcode_extended

static void runInstruction(void)
{
    FIXME("lots of missing instructions here.  :)");
    FIXME("verify PC is sane");
    const uint8 opcode = *(GPC++);
    OpcodeFn op = NULL;
    const char *opname = NULL;

    switch (opcode)
    {
        // 2-operand instructions...
        case 1: OPCODE_WRITEME(je);
        case 2: OPCODE_WRITEME(jl);
        case 3: OPCODE_WRITEME(jg);
        case 4: OPCODE_WRITEME(dec_chk);
        case 5: OPCODE_WRITEME(inc_chk);
        case 6: OPCODE_WRITEME(jin);
        case 7: OPCODE_WRITEME(test);
        case 8: OPCODE_WRITEME(or);
        case 9: OPCODE_WRITEME(and);
        case 10: OPCODE_WRITEME(test_attr);
        case 11: OPCODE_WRITEME(set_attr);
        case 12: OPCODE_WRITEME(clear_attr);
        case 13: OPCODE_WRITEME(store);
        case 14: OPCODE_WRITEME(insert_obj);
        case 15: OPCODE_WRITEME(loadw);
        case 16: OPCODE_WRITEME(loadb);
        case 17: OPCODE_WRITEME(get_prop);
        case 18: OPCODE_WRITEME(get_prop_addr);
        case 19: OPCODE_WRITEME(get_next_prop);
        case 20: OPCODE_WRITEME(add);
        case 21: OPCODE_WRITEME(sub);
        case 22: OPCODE_WRITEME(mul);
        case 23: OPCODE_WRITEME(div);
        case 24: OPCODE_WRITEME(mod);
        case 25: OPCODE_WRITEME(call_2s);
        case 26: OPCODE_WRITEME(call_2n);
        case 27: OPCODE_WRITEME(set_colour);
        case 28: OPCODE_WRITEME(throw);

        // 1-operand instructions...
        case 128: OPCODE_WRITEME(jz);
        case 129: OPCODE_WRITEME(get_sibling);
        case 130: OPCODE_WRITEME(get_child);
        case 131: OPCODE_WRITEME(get_parent);
        case 132: OPCODE_WRITEME(get_prop_len);
        case 133: OPCODE_WRITEME(inc);
        case 134: OPCODE_WRITEME(dec);
        case 135: OPCODE_WRITEME(print_addr);
        case 136: OPCODE_WRITEME(call_1s);
        case 137: OPCODE_WRITEME(remove_obj);
        case 138: OPCODE_WRITEME(print_obj);
        case 139: OPCODE_WRITEME(ret);
        case 140: OPCODE_WRITEME(jump);
        case 141: OPCODE_WRITEME(print_paddr);
        case 142: OPCODE_WRITEME(load);
        case 143: OPCODE_WRITEME(not);  // call_1n in ver5+

        // 0-operand instructions...
        case 176: OPCODE_WRITEME(rtrue);
        case 177: OPCODE_WRITEME(rfalse);
        case 178: OPCODE_WRITEME(print);
        case 179: OPCODE_WRITEME(print_ret);
        case 180: OPCODE_WRITEME(nop);
        case 181: OPCODE_WRITEME(save);
        case 182: OPCODE_WRITEME(restore);
        case 183: OPCODE_WRITEME(restart);
        case 184: OPCODE_WRITEME(ret_popped);
        case 185: OPCODE_WRITEME(pop);
        case 186: OPCODE_WRITEME(quit);
        case 187: OPCODE_WRITEME(new_line);
        case 188: OPCODE_WRITEME(show_status);
        case 189: OPCODE_WRITEME(verify);
        case 190: OPCODE(extended);
        case 191: OPCODE_WRITEME(piracy);

        // variable operand instructions...
        case 224: OPCODE(call);
        case 225: OPCODE_WRITEME(storew);
        case 226: OPCODE_WRITEME(storeb);
        case 227: OPCODE_WRITEME(put_prop);
        case 228: OPCODE_WRITEME(sread);
        case 229: OPCODE_WRITEME(print_char);
        case 230: OPCODE_WRITEME(print_num);
        case 231: OPCODE_WRITEME(random);
        case 232: OPCODE_WRITEME(push);
        case 233: OPCODE_WRITEME(pull);
        case 234: OPCODE_WRITEME(split_window);
        case 235: OPCODE_WRITEME(set_window);
        case 236: OPCODE_WRITEME(call_vs2);
        case 237: OPCODE_WRITEME(erase_window);
        case 238: OPCODE_WRITEME(erase_line);
        case 239: OPCODE_WRITEME(set_cursor);
        case 240: OPCODE_WRITEME(get_cursor);
        case 241: OPCODE_WRITEME(set_text_style);
        case 242: OPCODE_WRITEME(buffer_mode);
        case 243: OPCODE_WRITEME(output_stream);
        case 244: OPCODE_WRITEME(input_stream);
        case 245: OPCODE_WRITEME(sound_effect);
        case 246: OPCODE_WRITEME(read_char);
        case 247: OPCODE_WRITEME(scan_table);
        case 248: OPCODE_WRITEME(not_v5);
        case 249: OPCODE_WRITEME(call_vn);
        case 250: OPCODE_WRITEME(call_vn2);
        case 251: OPCODE_WRITEME(tokenise);
        case 252: OPCODE_WRITEME(encode_text);
        case 253: OPCODE_WRITEME(copy_table);
        case 254: OPCODE_WRITEME(print_table);
        case 255: OPCODE_WRITEME(check_arg_count);

        default:
            die("Unsupported or unknown opcode #%u", (unsigned int) opcode);
    } // switch

    if (!op)
        die("Unimplemented opcode #%d ('%s')", (unsigned int) opcode, opname);

    dbg("Run opcode %u ('%s') ...\n", opcode, opname);
    op();
} // runInstruction

#undef OPCODE
#undef OPCODE_WRITEME

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
    while (!GQuit)
        runInstruction();

    dbg("ok.\n");

    free(GStory);
    return 0;
} // main

// end of mojozork.c ...

