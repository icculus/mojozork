/**
 * MojoZork; a simple, just-for-fun implementation of Infocom's Z-Machine.
 *
 * Please see the file LICENSE.txt in the source's root directory.
 *
 *  This file written by Ryan C. Gordon.
 */

// The Z-Machine specifications 1.1:
//     http://inform-fiction.org/zmachine/standards/z1point1/index.html

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdint.h>
#include <time.h>

#define NORETURN __attribute__((noreturn))

static inline void dbg(const char *fmt, ...)
{
#if 0
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
#endif
} // dbg

#if 0
#define FIXME(what)
#else
#define FIXME(what) { \
    static int seen = 0; \
    if (!seen) { \
        seen = 1; \
        dbg("FIXME: %s\n", what); \
    } \
}
#endif

// the "_t" drives me nuts.  :/
typedef uint8_t uint8;
typedef int8_t sint8;
typedef uint16_t uint16;
typedef int16_t sint16;
typedef uint32_t uint32;
typedef int32_t sint32;

typedef size_t uintptr;

// !!! FIXME: maybe kill these.
#define READUI8(ptr) *(ptr++)
#define READUI16(ptr) ((((uint16) ptr[0]) << 8) | ((uint16) ptr[1])); ptr += sizeof (uint16)
#define WRITEUI16(dst, src) { *(dst++) = (src >> 8) & 0xFF; *(dst++) = (src >> 0) & 0xFF; }

typedef struct ZHeader
{
    uint8 version;
    uint8 flags1;
    uint16 release;
    uint16 himem_addr;
    uint16 pc_start;  // in ver6, packed address of main()
    uint16 dict_addr;
    uint16 objtab_addr;
    uint16 globals_addr;
    uint16 staticmem_addr;  // offset of static memory, also: size of dynamic mem.
    uint16 flags2;
    char serial_code[7];  // six ASCII chars in ver2. In ver3+: ASCII of completion date: YYMMDD
    uint16 abbrtab_addr;  // abbreviations table
    uint16 story_len;
    uint16 story_checksum;
    // !!! FIXME: more fields here, all of which are ver4+
} ZHeader;

static uint32 GInstructionsRun = 0;
static uint8 *GStory = NULL;
static uintptr GStoryLen = 0;
static ZHeader GHeader;
static const uint8 *GPC = 0;
static uint32 GLogicalPC = 0;
static int GQuit = 0;
static uint16 GStack[2048];  // !!! FIXME: make this dynamic?
static uint16 GOperands[8];
static uint8 GOperandCount = 0;
static uint16 *GSP = NULL;  // stack pointer
static uint16 GBP = 0;  // base pointer
static char GAlphabetTable[78];
static const char *GStartupScript = NULL;
static char *GStoryFname = NULL;

static void die(const char *fmt, ...) NORETURN;
static void die(const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "\nERROR: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, " (pc=%X)\n", (unsigned int) GLogicalPC);
    fprintf(stderr, " %u instructions run\n", (unsigned int) GInstructionsRun);
    fprintf(stderr, "\n");
    fflush(stderr);
    fflush(stdout);

    _exit(1);
} // die


typedef void (*OpcodeFn)(void);

// The Z-Machine can't directly address 32-bits, but this needs to expand past 16 bits when we multiply by 2, 4, or 8, etc.
static uint8 *unpackAddress(const uint32 addr)
{
    if (GHeader.version <= 3)
        return (GStory + (addr * 2));
    else if (GHeader.version <= 5)
        return (GStory + (addr * 4));
    else if (GHeader.version <= 6)
        die("write me");  //   4P + 8R_O    Versions 6 and 7, for routine calls ... or 4P + 8S_O    Versions 6 and 7, for print_paddr
    else if (GHeader.version <= 8)
        return (GStory + (addr * 8));

    die("FIXME Unsupported version for packed addressing");
    return NULL;
} // unpackAddress

static uint8 *varAddress(const uint8 var, const int writing)
{
    if (var == 0) // top of stack
    {
        if (writing)
        {
            if ((GSP-GStack) >= (sizeof (GStack) / sizeof (GStack[0])))
                die("Stack overflow");
            dbg("push stack\n");
            return (uint8 *) GSP++;
        } // if
        else
        {
            if (GSP == GStack)
                die("Stack underflow");  // nothing on the stack at all?

            const uint16 numlocals = GBP ? GStack[GBP-1] : 0;
            if ((GBP + numlocals) >= (GSP-GStack))
                die("Stack underflow");  // no stack data left in this frame.

            dbg("pop stack\n");
            return (uint8 *) --GSP;
        } // else
    } // if

    else if ((var >= 0x1) && (var <= 0xF))  // local var.
    {
        if (GStack[GBP-1] <= (var-1))
            die("referenced unallocated local var #%u (%u available)", (unsigned int) (var-1), (unsigned int) GStack[GBP-1]);
        return (uint8 *) &GStack[GBP + (var-1)];
    } // else if

    // else, global var
    FIXME("check for overflow, etc");
    return (GStory + GHeader.globals_addr) + ((var-0x10) * sizeof (uint16));
} // varAddress

static void opcode_call(void)
{
    uint8 args = GOperandCount;
    const uint16 *operands = GOperands;
    const uint8 storeid = *(GPC++);
    // no idea if args==0 should be the same as calling addr 0...
    if ((args == 0) || (operands[0] == 0))  // legal no-op; store 0 to return value and bounce.
    {
        uint8 *store = varAddress(storeid, 1);
        WRITEUI16(store, 0);
    } // if
    else
    {
        const uint8 *routine = unpackAddress(operands[0]);
        GLogicalPC = (uint32) (routine - GStory);
        const uint8 numlocals = *(routine++);
        if (numlocals > 15)
            die("Routine has too many local variables (%u)", numlocals);

        FIXME("check for stack overflow here");

        *(GSP++) = (uint16) storeid;  // save where we should store the call's result.

        // next instruction to run upon return.
        const uint32 pcoffset = (uint32) (GPC - GStory);
        *(GSP++) = (pcoffset & 0xFFFF);
        *(GSP++) = ((pcoffset >> 16) & 0xFFFF);

        *(GSP++) = GBP;  // current base pointer before the call.
        *(GSP++) = numlocals;  // number of locals we're allocating.

        GBP = (uint16) (GSP-GStack);

        sint8 i;
        if (GHeader.version <= 4)
        {
            for (i = 0; i < numlocals; i++, routine += sizeof (uint16))
                *(GSP++) = *((uint16 *) routine);  // leave it byteswapped when moving to the stack.
        } // if
        else
        {
            for (i = 0; i < numlocals; i++)
                *(GSP++) = 0;
        } // else

        args--;  // remove the return address from the count.
        if (args > numlocals)  // it's legal to have more args than locals, throw away the extras.
            args = numlocals;

        const uint16 *src = operands + 1;
        uint8 *dst = (uint8 *) (GStack + GBP);
        for (i = 0; i < args; i++)
        {
            WRITEUI16(dst, src[i]);
        } // for

        GPC = routine;
        // next call to runInstruction() will execute new routine.
    } // else
} // opcode_call

static void doReturn(const uint16 val)
{
    FIXME("newer versions start in a real routine, but still aren't allowed to return from it.");
    if (GBP == 0)
        die("Stack underflow in return operation");

    dbg("popping stack for return\n");
    dbg("returning: initial pc=%X, bp=%u, sp=%u\n", (unsigned int) (GPC-GStory), (unsigned int) GBP, (unsigned int) (GSP-GStack));

    GSP = GStack + GBP;  // this dumps all the locals and data pushed on the stack during the routine.
    GSP--;  // dump our copy of numlocals
    GBP = *(--GSP);  // restore previous frame's base pointer, dump it from the stack.

    GSP -= 2;  // point to start of our saved program counter.
    const uint32 pcoffset = ((uint32) GSP[0]) | (((uint32) GSP[1]) << 16);

    GPC = GStory + pcoffset;  // next instruction is one following our original call.

    const uint8 storeid = (uint8) *(--GSP);  // pop the result storage location.

    dbg("returning: new pc=%X, bp=%u, sp=%u\n", (unsigned int) (GPC-GStory), (unsigned int) GBP, (unsigned int) (GSP-GStack));
    uint8 *store = varAddress(storeid, 1);  // and store the routine result.
    WRITEUI16(store, val);
} // doReturn

static void opcode_ret(void)
{
    doReturn(GOperands[0]);
} // opcode_ret

static void opcode_rtrue(void)
{
    doReturn(1);
} // opcode_rtrue

static void opcode_rfalse(void)
{
    doReturn(0);
} // opcode_rfalse

static void opcode_ret_popped(void)
{
    uint8 *ptr = varAddress(0, 0);   // top of stack.
    const uint16 result = READUI16(ptr);
    doReturn(result);
} // opcode_ret_popped

static void opcode_push(void)
{
    uint8 *store = varAddress(0, 1);   // top of stack.
    WRITEUI16(store, GOperands[0]);
} // opcode_push

static void opcode_pull(void)
{
    uint8 *ptr = varAddress(0, 0);   // top of stack.
    const uint16 val = READUI16(ptr);
    uint8 *store = varAddress(GOperands[0], 1);
    WRITEUI16(store, val);
} // opcode_pull

static void opcode_pop(void)
{
    varAddress(0, 0);   // this causes a pop.
} // opcode_pop

static void opcode_add(void)
{
    uint8 *store = varAddress(*(GPC++), 1);
    const sint16 result = ((sint16) GOperands[0]) + ((sint16) GOperands[1]);
    WRITEUI16(store, result);
} // opcode_add

static void opcode_sub(void)
{
    uint8 *store = varAddress(*(GPC++), 1);
    const sint16 result = ((sint16) GOperands[0]) - ((sint16) GOperands[1]);
    WRITEUI16(store, result);
} // opcode_sub

static void doBranch(int truth)
{
    const uint8 branch = *(GPC++);
    const int farjump = (branch & (1<<6)) == 0;
    const int onTruth = (branch & (1<<7)) ? 1 : 0;

    const uint8 byte2 = farjump ? *(GPC++) : 0;

    if (truth == onTruth)  // take the branch?
    {
        sint16 offset = (sint16) (branch & 0x3F);
        if (farjump)
        {
            if (offset & (1 << 5))
                offset |= 0xC0;   // extend out sign bit.
            offset = (offset << 8) | ((sint16) byte2);
        } // else

        if (offset == 0)  // return false from current routine.
            doReturn(0);
        else if (offset == 1)  // return true from current routine.
            doReturn(1);
        else
            GPC = (GPC + offset) - 2;  // branch.
    } // if
} // doBranch

static void opcode_je(void)
{
    const uint16 a = GOperands[0];
    sint8 i;
    for (i = 1; i < GOperandCount; i++)
    {
        if (a == GOperands[i])
        {
            doBranch(1);
            return;
        } // if
    } // for

    doBranch(0);
} // opcode_je

static void opcode_jz(void)
{
    doBranch((GOperands[0] == 0) ? 1 : 0);
} // opcode_jz

static void opcode_jl(void)
{
    doBranch((((sint16) GOperands[0]) < ((sint16) GOperands[1])) ? 1 : 0);
} // opcode_jl

static void opcode_jg(void)
{
    doBranch((((sint16) GOperands[0]) > ((sint16) GOperands[1])) ? 1 : 0);
} // opcode_jg

static void opcode_test(void)
{
    doBranch((GOperands[0] & GOperands[1]) == GOperands[1]);
} // opcode_test

static void opcode_jump(void)
{
    // this opcode is not a branch instruction, and doesn't follow those rules.
    FIXME("make sure GPC is valid");
    GPC = (GPC + ((sint16) GOperands[0])) - 2;
} // opcode_jump

static void opcode_div(void)
{
    uint8 *store = varAddress(*(GPC++), 1);
    if (GOperands[1] == 0)
        die("Division by zero");
    const uint16 result = (uint16) (((sint16) GOperands[0]) / ((sint16) GOperands[1]));
    WRITEUI16(store, result);
} // opcode_div

static void opcode_mod(void)
{
    uint8 *store = varAddress(*(GPC++), 1);
    if (GOperands[1] == 0)
        die("Division by zero");
    const uint16 result = (uint16) (((sint16) GOperands[0]) % ((sint16) GOperands[1]));
    WRITEUI16(store, result);
} // opcode_div

static void opcode_mul(void)
{
    uint8 *store = varAddress(*(GPC++), 1);
    const uint16 result = (uint16) (((sint16) GOperands[0]) * ((sint16) GOperands[1]));
    WRITEUI16(store, result);
} // opcode_mul

static void opcode_or(void)
{
    uint8 *store = varAddress(*(GPC++), 1);
    const uint16 result = (GOperands[0] | GOperands[1]);
    WRITEUI16(store, result);
} // opcode_or

static void opcode_and(void)
{
    uint8 *store = varAddress(*(GPC++), 1);
    const uint16 result = (GOperands[0] & GOperands[1]);
    WRITEUI16(store, result);
} // opcode_and

static void opcode_not(void)
{
    uint8 *store = varAddress(*(GPC++), 1);
    const uint16 result = ~GOperands[0];
    WRITEUI16(store, result);
} // opcode_not

static void opcode_inc_chk(void)
{
    uint8 *store = varAddress((uint8) GOperands[0], 1);
    sint16 val = READUI16(store);
    store -= sizeof (uint16);
    val++;
    WRITEUI16(store, (uint16) val);
    doBranch( (((sint16) val) > ((sint16) GOperands[1])) ? 1 : 0 );
} // opcode_inc_chk

static void opcode_inc(void)
{
    uint8 *store = varAddress((uint8) GOperands[0], 1);
    sint16 val = (sint16) READUI16(store);
    store -= sizeof (uint16);
    val++;
    WRITEUI16(store, (uint16) val);
} // opcode_inc

static void opcode_dec_chk(void)
{
    uint8 *store = varAddress((uint8) GOperands[0], 1);
    sint16 val = (sint16) READUI16(store);
    store -= sizeof (uint16);
    val--;
    WRITEUI16(store, (uint16) val);
    doBranch( (((sint16) val) < ((sint16) GOperands[1])) ? 1 : 0 );
} // opcode_dec_chk

static void opcode_dec(void)
{
    uint8 *store = varAddress((uint8) GOperands[0], 1);
    sint16 val = (sint16) READUI16(store);
    store -= sizeof (uint16);
    val--;
    WRITEUI16(store, (uint16) val);
} // opcode_dec

static void opcode_load(void)
{
    uint8 *store = varAddress(*(GPC++), 1);
    WRITEUI16(store, GOperands[0]);
} // opcode_load

static void opcode_loadw(void)
{
    uint16 *store = (uint16 *) varAddress(*(GPC++), 1);
    FIXME("can only read from dynamic or static memory (not highmem).");
    FIXME("how does overflow work here? Do these wrap around?");
    uint16 *src = (uint16 *) (GStory + (GOperands[0] + (GOperands[1] * 2)));
    *store = *src;  // copy from bigendian to bigendian: no byteswap.
} // opcode_loadw

static void opcode_loadb(void)
{
    uint8 *store = varAddress(*(GPC++), 1);
    FIXME("can only read from dynamic or static memory (not highmem).");
    FIXME("how does overflow work here? Do these wrap around?");
    const uint8 *src = GStory + (GOperands[0] + (GOperands[1]));
    const uint16 value = *src;  // expand out to 16-bit before storing.
    WRITEUI16(store, value);
} // opcode_loadb

static void opcode_storew(void)
{
    FIXME("can only write to dynamic memory.");
    FIXME("how does overflow work here? Do these wrap around?");
    uint8 *dst = (GStory + (GOperands[0] + (GOperands[1] * 2)));
    const uint16 src = GOperands[2];
    WRITEUI16(dst, src);
} // opcode_storew

static void opcode_storeb(void)
{
    FIXME("can only write to dynamic memory.");
    FIXME("how does overflow work here? Do these wrap around?");
    uint8 *dst = (GStory + GOperands[0] + GOperands[1]);
    const uint8 src = (uint8) GOperands[2];
    *dst = src;
} // opcode_storeb

static void opcode_store(void)
{
    uint8 *store = varAddress((uint8) (GOperands[0] & 0xFF), 1);
    const uint16 src = GOperands[1];
    WRITEUI16(store, src);
} // opcode_store

static uint8 *getObjectPtr(const uint16 objid)
{
    if (objid == 0)
        die("Object id #0 referenced");

    if ((GHeader.version <= 3) && (objid > 255))
        die("Invalid object id referenced");

    uint8 *ptr = GStory + GHeader.objtab_addr;
    ptr += 31 * sizeof (uint16);  // skip properties defaults table
    ptr += 9 * (objid-1);  // find object in object table
    return ptr;
} // getObjectPtr

static void opcode_test_attr(void)
{
    const uint16 objid = GOperands[0];
    const uint16 attrid = GOperands[1];
    uint8 *ptr = getObjectPtr(objid);

    if (GHeader.version <= 3)
    {
        ptr += (attrid / 8);
        doBranch((*ptr & (0x80 >> (attrid & 7))) ? 1 : 0);
    } // if
    else
    {
        die("write me");
    } // else
} // opcode_test_attr

static void opcode_set_attr(void)
{
    const uint16 objid = GOperands[0];
    const uint16 attrid = GOperands[1];
    uint8 *ptr = getObjectPtr(objid);

    if (GHeader.version <= 3)
    {
        ptr += (attrid / 8);
        *ptr |= 0x80 >> (attrid & 7);
    } // if
    else
    {
        die("write me");
    } // else
} // opcode_set_attr

static void opcode_clear_attr(void)
{
    const uint16 objid = GOperands[0];
    const uint16 attrid = GOperands[1];
    uint8 *ptr = getObjectPtr(objid);

    if (GHeader.version <= 3)
    {
        ptr += (attrid / 8);
        *ptr &= ~(0x80 >> (attrid & 7));
    } // if
    else
    {
        die("write me");
    } // else
} // opcode_clear_attr

static uint8 *getObjectPtrParent(uint8 *objptr)
{
    if (GHeader.version <= 3)
    {
        objptr += 4;  // skip object attributes.
        const uint16 parent = *objptr;
        return parent ? getObjectPtr(parent) : NULL;
    }
    else
    {
        die("write me");
    } // else
} // getGetObjectPtrParent

static void unparentObject(const uint16 objid)
{
    uint8 *objptr = getObjectPtr(objid);
    uint8 *parentptr = getObjectPtrParent(objptr);
    if (parentptr != NULL)  // if NULL, no need to remove it.
    {
        uint8 *ptr = parentptr + 6;  // 4 to skip attrs, 2 to skip to child.
        while (*ptr != objid) // if not direct child, look through sibling list...
            ptr = getObjectPtr(*ptr) + 5;  // get sibling field.
        *ptr = *(objptr + 5);  // obj sibling takes obj's place.
    } // if
} // unparentObject

static void opcode_insert_obj(void)
{
    const uint16 objid = GOperands[0];
    const uint16 dstid = GOperands[1];

    uint8 *objptr = getObjectPtr(objid);
    uint8 *dstptr = getObjectPtr(dstid);

    if (GHeader.version <= 3)
    {
        unparentObject(objid);  // take object out of its original tree first.

        // now reinsert in the right place.
        *(objptr + 4) = (uint8) dstid;  // parent field: new destination
        *(objptr + 5) = *(dstptr + 6);  // sibling field: new dest's old child.
        *(dstptr + 6) = objid;  // dest's child field: object being moved.
    } // if
    else
    {
        die("write me");  // fields are different in ver4+.
    } // else
} // opcode_insert_obj

static void opcode_remove_obj(void)
{
    const uint16 objid = GOperands[0];
    uint8 *objptr = getObjectPtr(objid);

    if (GHeader.version > 3)
        die("write me");  // fields are different in ver4+.
    else
    {
        unparentObject(objid);  // take object out of its original tree first.

        // now clear out object's relationships...
        *(objptr + 4) = 0;  // parent field: zero.
        *(objptr + 5) = 0;  // sibling field: zero.
    } // else
} // opcode_remove_obj

static uint8 *getObjectProperty(const uint16 objid, const uint32 propid, uint8 *_size)
{
    uint8 *ptr = getObjectPtr(objid);

    if (GHeader.version <= 3)
    {
        ptr += 7;  // skip to properties address field.
        const uint16 addr = READUI16(ptr);
        ptr = GStory + addr;
        ptr += (*ptr * 2) + 1;  // skip object name to start of properties.
        while (1)
        {
            const uint8 info = *(ptr++);
            const uint16 num = (info & 0x1F);  // 5 bits for the prop id.
            const uint8 size = ((info >> 5) & 0x7) + 1; // 3 bits for prop size.
            // these go in descending numeric order, and should fail
            //  the interpreter if missing. We use 0xFFFFFFFF internally to mean "first property".
            if ((num == propid) || (propid == 0xFFFFFFFF))  // found it?
            {
                if (_size)
                    *_size = size;
                return ptr;
            } // if

            else if (num < propid)  // we're past it.
                break;

            ptr += size;  // try the next property.
        } // while
    } // if
    else
    {
        die("write me");
    } // else

    return NULL;
} // getObjectProperty

static void opcode_put_prop(void)
{
    const uint16 objid = GOperands[0];
    const uint16 propid = GOperands[1];
    const uint16 value = GOperands[2];
    uint8 size = 0;
    uint8 *ptr = getObjectProperty(objid, propid, &size);

    if (!ptr)
        die("Lookup on missing object property (obj=%X, prop=%X)", (unsigned int) objid, (unsigned int) propid);
    else if (size == 1)
        *ptr = (value & 0xFF);
    else
    {
        WRITEUI16(ptr, value);
    } // else
} // opcode_put_prop

static uint16 getDefaultObjectProperty(const uint16 propid)
{
    if ( ((GHeader.version <= 3) && (propid > 31)) ||
         ((GHeader.version >= 4) && (propid > 63)) )
    {
        FIXME("Should we die here?");
        return 0;
    } // if

    const uint8 *values = (GStory + GHeader.objtab_addr);
    values += (propid-1) * sizeof (uint16);
    const uint16 result = READUI16(values);
    return result;
} // getDefaultObjectProperty

static void opcode_get_prop(void)
{
    uint8 *store = varAddress(*(GPC++), 1);
    const uint16 objid = GOperands[0];
    const uint16 propid = GOperands[1];
    uint16 result = 0;
    uint8 size = 0;
    uint8 *ptr = getObjectProperty(objid, propid, &size);

    if (!ptr)
        result = getDefaultObjectProperty(propid);
    else if (size == 1)
        result = *ptr;
    else
    {
        result = READUI16(ptr);
    } // else

    WRITEUI16(store, result);
} // opcode_get_prop

static void opcode_get_prop_addr(void)
{
    uint8 *store = varAddress(*(GPC++), 1);
    const uint16 objid = GOperands[0];
    const uint16 propid = GOperands[1];
    uint8 *ptr = getObjectProperty(objid, propid, NULL);
    const uint16 result = ptr ? ((uint16) (ptr-GStory)) : 0;
    WRITEUI16(store, result);
} // opcode_get_prop_addr

static void opcode_get_prop_len(void)
{
    uint8 *store = varAddress(*(GPC++), 1);
    uint16 result;

    if (GOperands[0] == 0)
        result = 0;  // this must return 0, to avoid a bug in older Infocom games.
    else if (GHeader.version <= 3)
    {
        const uint8 *ptr = GStory + GOperands[0];
        const uint8 info = ptr[-1];  // the size field.
        result = ((info >> 5) & 0x7) + 1; // 3 bits for prop size.
    } // if
    else
    {
        die("write me");
    } // else

    WRITEUI16(store, result);
} // opcode_get_prop_size

static void opcode_get_next_prop(void)
{
    uint8 *store = varAddress(*(GPC++), 1);
    const uint16 objid = GOperands[0];
    const int firstProp = (GOperands[1] == 0);
    uint16 result = 0;
    uint8 size = 0;
    uint8 *ptr = getObjectProperty(objid, firstProp ? 0xFFFFFFFF : GOperands[1], &size);

    if (!ptr)
        die("get_next_prop on missing property obj=%X, prop=%X", (unsigned int) objid, (unsigned int) GOperands[1]);
    else if (GHeader.version <= 3)
        result = ptr[firstProp ? -1 : ((sint8) size)] & 0x1F;  // 5 bits for the prop id.
    else
        die("write me");

    WRITEUI16(store, result);
} // opcode_get_next_prop

static void opcode_jin(void)
{
    const uint16 objid = GOperands[0];
    const uint16 parentid = GOperands[1];
    const uint8 *objptr = getObjectPtr(objid);

    if (GHeader.version <= 3)
        doBranch((((uint16) objptr[4]) == parentid) ? 1 : 0);
    else
        die("write me");  // fields are different in ver4+.
} // opcode_jin

static uint16 getObjectRelationship(const uint16 objid, const uint8 relationship)
{
    const uint8 *objptr = getObjectPtr(objid);

    if (GHeader.version <= 3)
        return objptr[relationship];
    else
        die("write me");  // fields are different in ver4+.
} // getObjectRelationship

static void opcode_get_parent(void)
{
    uint8 *store = varAddress(*(GPC++), 1);
    const uint16 result = getObjectRelationship(GOperands[0], 4);
    WRITEUI16(store, result);
} // opcode_get_parent

static void opcode_get_sibling(void)
{
    uint8 *store = varAddress(*(GPC++), 1);
    const uint16 result = getObjectRelationship(GOperands[0], 5);
    WRITEUI16(store, result);
    doBranch((result != 0) ? 1: 0);
} // opcode_get_sibling

static void opcode_get_child(void)
{
    uint8 *store = varAddress(*(GPC++), 1);
    const uint16 result = getObjectRelationship(GOperands[0], 6);
    WRITEUI16(store, result);
    doBranch((result != 0) ? 1: 0);
} // opcode_get_child

static void opcode_new_line(void)
{
    putchar('\n');
    fflush(stdout);
} // opcode_new_line

static void print_zscii_char(const uint16 val)
{
    char ch = 0;

    FIXME("ver6+ has a few more valid codes");

    // only a few values are valid ZSCII codes for output.
    if ((val >= 32) && (val <= 126))
        ch = (char) val;  // FIXME: we assume you have an ASCII terminal for now.
    else if (val == 13)  // newline
        ch = '\n';
    else if (val == 0)
        /* val==0 is "valid" but produces no output. */ ;
    else if ((val >= 155) && (val <= 251))
        { FIXME("write me: extended ZSCII characters"); ch = '?'; }
    else
        ch = '?';  // this is illegal, but we'll be nice.

    if (ch)
        putchar(ch);
} // print_zscii_char

static uintptr print_zscii(const uint8 *_str, const int abbr)
{
    // ZCSII encoding is so nasty.
    const uint8 *str = _str;
    uint16 code = 0;
    uint8 alphabet = 0;
    uint8 useAbbrTable = 0;
    uint8 zscii_collector = 0;
    uint16 zscii_code = 0;

    do
    {
        code = READUI16(str);

        // characters are 5 bits each, packed three to a 16-bit word.
        sint8 i;
        for (i = 10; i >= 0; i -= 5)
        {
            int newshift = 0;
            char printVal = 0;
            const uint8 ch = ((code >> i) & 0x1F);

            if (zscii_collector)
            {
                if (zscii_collector == 2)
                    zscii_code |= ((uint16) ch) << 5;
                else
                    zscii_code |= ((uint16) ch);

                zscii_collector--;
                if (!zscii_collector)
                {
                    print_zscii_char(zscii_code);
                    alphabet = useAbbrTable = zscii_code = 0;
                } // if
                continue;
            } // if

            else if (useAbbrTable)
            {
                if (abbr)
                    die("Abbreviation strings can't use abbreviations");
                //FIXME("Make sure offset is sane");
                const uintptr index = ((32 * (((uintptr) useAbbrTable) - 1)) + (uintptr) ch);
                const uint8 *ptr = (GStory + GHeader.abbrtab_addr) + (index * sizeof (uint16));
                const uint16 abbraddr = READUI16(ptr);
                print_zscii(GStory + (abbraddr * sizeof (uint16)), 1);
                useAbbrTable = 0;
                alphabet = 0;  // FIXME: no shift locking in ver3+, but ver1 needs it.
                continue;
            } // if

            switch (ch)
            {
                case 0:
                    printVal = ' ';
                    break;

                case 1:
                    if (GHeader.version == 1)
                        printVal = '\n';
                    else
                        useAbbrTable = 1;
                    break;

                case 2:
                case 3:
                    if (GHeader.version <= 2)
                        die("write me: handle ver1/2 alphabet shifting");
                    else
                        useAbbrTable = ch;
                    break;

                case 4:
                case 5:
                    if (GHeader.version <= 2)
                        die("write me: handle ver1/2 alphabet shift locking");
                    else
                    {
                        newshift = 1;
                        alphabet = ch - 3;
                    } // else
                    break;

                default:
                    if ((ch == 6) && (alphabet == 2))
                        zscii_collector = 2;
                    else
                        printVal = GAlphabetTable[(alphabet*26) + (ch-6)]; break;
                    break;
            } // switch

            if (printVal)
                putchar(printVal);

            if (alphabet && !newshift)
                alphabet = 0;
        } // for

        // there is no NULL terminator, you look for a word with the top bit set.
    } while ((code & (1<<15)) == 0);

    return str - _str;
} // print_zscii

static void opcode_print(void)
{
    GPC += print_zscii(GPC, 0);
} // opcode_print

static void opcode_print_num(void)
{
    printf("%d", (int) GOperands[0]);
} // opcode_print_num

static void opcode_print_char(void)
{
    print_zscii_char(GOperands[0]);
} // opcode_print_char

static void opcode_print_ret(void)
{
    GPC += print_zscii(GPC, 0);
    putchar('\n');
    fflush(stdout);
    doReturn(1);
} // opcode_print_ret

static void opcode_print_obj(void)
{
    uint8 *ptr = getObjectPtr(GOperands[0]);

    if (GHeader.version <= 3)
    {
        ptr += 7;  // skip to properties field.
        const uint16 addr = READUI16(ptr);  // dereference to get to property table.
        print_zscii(GStory + addr + 1, 0);
    } // if
    else
    {
        die("write me");
    } // else
} // opcode_print_obj

static void opcode_print_addr(void)
{
    print_zscii(GStory + GOperands[0], 0);
} // opcode_print_addr

static void opcode_print_paddr(void)
{
    print_zscii(unpackAddress(GOperands[0]), 0);
} // opcode_print_paddr

static uint16 doRandom(const sint16 range)
{
    uint16 result = 0;
    if (range == 0)  // reseed in "most random way"
        srandom((unsigned long) time(NULL));
    else if (range < 0)  // reseed with specific value
        srandom(-range);
    else
    {
        FIXME("this is sucky");
        long r = random();
        r = (r >> (sizeof (r) / 2) * 8) ^ r;
        result = (uint16) ((((float) (r & 0xFFFF)) / 65535.0f) * ((float) range));
        if (!result)
            result = 1;
    } // else
    return result;
} // doRandom

static void opcode_random(void)
{
    uint8 *store = varAddress(*(GPC++), 1);
    const sint16 range = (sint16) GOperands[0];
    const uint16 result = doRandom(range);
    WRITEUI16(store, result);
} // opcode_random

static uint16 toZscii(const uint8 ch)
{
    if ((ch >= 'a') && (ch <= 'z'))
        return (ch - 'a') + 6;
    else if ((ch >= 'A') && (ch <= 'Z'))
        return (ch - 'A') + 6;
    else
        die("write me");
    return 0;
} // toZscii

static void opcode_read(void)
{
    static char *script = NULL;

    dbg("read from input stream: text-buffer=%X parse-buffer=%X\n", (unsigned int) GOperands[0], (unsigned int) GOperands[1]);

    uint8 *input = GStory + GOperands[0];
    const uint8 inputlen = *(input++);
    dbg("max input: %u\n", (unsigned int) inputlen);
    if (inputlen < 3)
        die("text buffer is too small for reading");  // happens on buffer overflow.

    uint8 *parse = GStory + GOperands[1];
    const uint8 parselen = *(parse++);
    parse++;  // skip over where we will write the final token count.

    dbg("max parse: %u\n", (unsigned int) parselen);
    if (parselen < 4)
        die("parse buffer is too small for reading");  // happens on buffer overflow.

    if (GStartupScript != NULL)
    {
        snprintf((char *) input, inputlen-1, "#script %s\n", GStartupScript);
        input[inputlen-1] = '\0';
        GStartupScript = NULL;
        printf("%s", (const char *) input);
    } // if

    else if (script == NULL)
    {
        FIXME("fgets isn't really the right solution here.");
        if (!fgets((char *) input, inputlen, stdin))
            die("EOF or error on stdin during read");
    } // else if

    else
    {
        uint8 i;
        char *scriptptr = script;
        for (i = 0; i < inputlen; i++, scriptptr++)
        {
            const char ch = *scriptptr;
            if (ch == '\0')
                break;
            else if (ch == '\n')
            {
                scriptptr++;
                break;
            } // else if
            else if (ch == '\r')
            {
                i--;
                continue;
            } // else if
            else
            {
                input[i] = (uint8) ch;
            } // else
        } // for
        input[i] = '\0';

        printf("%s\n", input);

        memmove(script, scriptptr, strlen(scriptptr) + 1);
        if (script[0] == '\0')
        {
            printf("*** Done running script.\n");
            free(script);
            script = NULL;
        } // if
    } // else

    dbg("input string from user is '%s'\n", (const char *) input);
    {
        char *ptr;
        for (ptr = (char *) input; *ptr; ptr++)
        {
            if ((*ptr >= 'A') && (*ptr <= 'Z'))
                *ptr -= 'A' - 'a';  // make it lowercase.
            else if ((*ptr == '\n') || (*ptr == '\r'))
            {
                *ptr = '\0';
                break;
            } // if
        } // for
    }

    if (strncmp((const char *) input, "#script ", 8) == 0)
    {
        if (script != NULL)
            die("FIXME: Can't nest scripts at the moment");

        const char *fname = (const char *) (input + 8);
        off_t len = 0;
        FILE *io = NULL;
        if ((io = fopen(fname, "rb")) == NULL)
            die("Failed to open '%s'", fname);
        else if ((fseeko(io, 0, SEEK_END) == -1) || ((len = ftello(io)) == -1))
            die("Failed to determine size of '%s'", fname);
        else if ((script = malloc(len)) == NULL)
            die("Out of memory");
        else if ((fseeko(io, 0, SEEK_SET) == -1) || (fread(script, len, 1, io) != 1))
            die("Failed to read '%s'", fname);
        fclose(io);
        printf("*** Running script '%s'...\n", fname);
        opcode_read();  // start over.
        return;
    } // if

    else if (strncmp((const char *) input, "#random ", 8) == 0)
    {
        const uint16 val = doRandom((sint16) atoi((const char *) (input+8)));
        printf("*** random replied: %u\n", (unsigned int) val);
        return opcode_read();  // go again.
    } // else if

    const uint8 *seps = GStory + GHeader.dict_addr;
    const uint8 numseps = *(seps++);
    const uint8 *dict = seps + numseps;
    const uint8 entrylen = *(dict++);
    const uint16 numentries = READUI16(dict);
    uint8 numtoks = 0;

    uint8 *strstart = input;
    uint8 *ptr = (uint8 *) input;
    while (1)
    {
        int isSep = 0;
        const uint8 ch = *ptr;
        if ((ch == ' ') || (ch == '\0'))
            isSep = 1;
        else
        {
            uint8 i;
            for (i = 0; i < numseps; i++)
            {
                if (ch == seps[i])
                {
                    isSep = 1;
                    break;
                } // if
            } // for
        } // else

        if (isSep)
        {
            uint16 encoded[3] = { 0, 0, 0 };

            const uint8 toklen = (uint8) (ptr-strstart);

            uint8 pos = 0;
            encoded[0] |= ((pos < toklen) ? toZscii(strstart[pos]) : 5) << 10; pos++;
            encoded[0] |= ((pos < toklen) ? toZscii(strstart[pos]) : 5) << 5; pos++;
            encoded[0] |= ((pos < toklen) ? toZscii(strstart[pos]) : 5) << 0; pos++;
            encoded[1] |= ((pos < toklen) ? toZscii(strstart[pos]) : 5) << 10; pos++;
            encoded[1] |= ((pos < toklen) ? toZscii(strstart[pos]) : 5) << 5; pos++;
            encoded[1] |= ((pos < toklen) ? toZscii(strstart[pos]) : 5) << 0; pos++;

            FIXME("this can binary search, since we know how many equal-sized records there are.");
            const uint8 *dictptr = dict;
            uint16 i;
            if (GHeader.version <= 3)
            {
                encoded[1] |= 0x8000;

                FIXME("byteswap 'encoded' and just memcmp here.");
                for (i = 0; i < numentries; i++)
                {
                    const uint16 zscii1 = READUI16(dictptr);
                    const uint16 zscii2 = READUI16(dictptr);
                    if ((encoded[0] == zscii1) && (encoded[1] == zscii2))
                    {
                        dictptr -= sizeof (uint16) * 2;
                        break;
                    } // if
                    dictptr += (entrylen - 4);
                } // for
            } // if
            else
            {
                encoded[2] |= ((pos < toklen) ? toZscii(strstart[pos]) : 5) << 10; pos++;
                encoded[2] |= ((pos < toklen) ? toZscii(strstart[pos]) : 5) << 5; pos++;
                encoded[2] |= ((pos < toklen) ? toZscii(strstart[pos]) : 5) << 0; pos++;
                encoded[2] |= 0x8000;

                FIXME("byteswap 'encoded' and just memcmp here.");
                for (i = 0; i < numentries; i++)
                {
                    const uint16 zscii1 = READUI16(dictptr);
                    const uint16 zscii2 = READUI16(dictptr);
                    const uint16 zscii3 = READUI16(dictptr);
                    if ((encoded[0] == zscii1) && (encoded[1] == zscii2) && (encoded[2] == zscii3))
                    {
                        dictptr -= sizeof (uint16) * 3;
                        break;
                    } // if
                    dictptr += (entrylen - 6);
                } // for
            } // else

            if (i == numentries)
                dictptr = NULL;  // not found.
            const uint16 dictaddr = (unsigned int) (dictptr - GStory);

            //dbg("Tokenized dictindex=%X, tokenlen=%u, strpos=%u\n", (unsigned int) dictaddr, (unsigned int) toklen, (unsigned int) ((uint8) (strstart-input)));

            WRITEUI16(parse, dictaddr);
            *(parse++) = (uint8) toklen;
            *(parse++) = (uint8) ((strstart-input) + 1);
            numtoks++;

            if (numtoks >= parselen)
                break;  // ran out of space.
            else if (*ptr == '\0')
                break;  // ran out of string.

            strstart = ptr + 1;
        } // if

        ptr++;
    } // while

    dbg("Tokenized %u tokens\n", (unsigned int) numtoks);

    *(GStory + GOperands[1] + 1) = numtoks;
} // opcode_read

static void loadStory(const char *fname);

static void opcode_restart(void)
{
    loadStory(GStoryFname);
} // opcode_restart

static void opcode_save(void)
{
    FIXME("this should write Quetzal format; this is temporary.");
    const uint32 addr = (uint32) (GPC-GStory);
    const uint32 sp = (uint32) (GSP-GStack);
    FILE *io = fopen("save.dat", "wb");
    int okay = 1;
    okay &= io != NULL;
    okay &= fwrite(GStory, GHeader.staticmem_addr, 1, io) == 1;
    okay &= fwrite(&addr, sizeof (addr), 1, io) == 1;
    okay &= fwrite(&sp, sizeof (sp), 1, io) == 1;
    okay &= fwrite(GStack, sizeof (GStack), 1, io) == 1;
    okay &= fwrite(&GBP, sizeof (GBP), 1, io) == 1;
    if (io)
        fclose(io);
    doBranch(okay ? 1 : 0);
} // opcode_save

static void opcode_restore(void)
{
    FIXME("this should read Quetzal format; this is temporary.");
    FILE *io = fopen("save.dat", "rb");
    int okay = 1;
    uint32 x = 0;

    okay &= io != NULL;
    okay &= fread(GStory, GHeader.staticmem_addr, 1, io) == 1;
    okay &= fread(&x, sizeof (x), 1, io) == 1;
    GLogicalPC = x;
    GPC = GStory + x;
    okay &= fread(&x, sizeof (x), 1, io) == 1;
    GSP = GStack + x;
    okay &= fread(GStack, sizeof (GStack), 1, io) == 1;
    okay &= fread(&GBP, sizeof (GBP), 1, io) == 1;
    if (io)
        fclose(io);

    if (!okay)
        die("Failed to restore.");

    doBranch(okay ? 1 : 0);
} // opcode_restore

static void opcode_quit(void)
{
    GQuit = 1;
} // opcode_quit

static void opcode_nop(void)
{
    // that's all, folks.
} // opcode_nop

typedef struct
{
    const char *name;
    OpcodeFn fn;
} Opcode;

// this is kinda wasteful (we could pack the 89 opcodes in their various forms
//  into separate arrays and strip off the metadata bits) but it simplifies
//  some things to just have a big linear array.
static Opcode GOpcodes[256];

// The extended ones, however, only have one form, so we pack that tight.
static Opcode GExtendedOpcodes[30];


static int parseOperand(const uint8 optype, uint16 *operand)
{
    switch (optype)
    {
        case 0: *operand = (uint16) READUI16(GPC); return 1;  // large constant (uint16)
        case 1: *operand = *(GPC++); return 1;  // small constant (uint8)
        case 2: { // variable
            const uint8 *addr = varAddress(*(GPC++), 0);
            *operand = READUI16(addr);
            return 1;
        }
        case 3: break;  // omitted altogether, we're done.
    } // switch

    return 0;
} // parseOperand

static uint8 parseVarOperands(uint16 *operands)
{
    const uint8 operandTypes = *(GPC++);
    uint8 shifter = 6;
    uint8 i;

    for (i = 0; i < 4; i++)
    {
        const uint8 optype = (operandTypes >> shifter) & 0x3;
        shifter -= 2;
        if (!parseOperand(optype, operands + i))
            break;
    } // for

    return i;
} // parseVarOperands


static void runInstruction(void)
{
    FIXME("verify PC is sane");

    GLogicalPC = (uint32) (GPC - GStory);
    uint8 opcode = *(GPC++);

    const Opcode *op = NULL;

    const int extended = ((opcode == 190) && (GHeader.version >= 5)) ? 1 : 0;
    if (extended)
    {
        opcode = *(GPC++);
        if (opcode >= (sizeof (GExtendedOpcodes) / sizeof (GExtendedOpcodes[0])))
            die("Unsupported or unknown extended opcode #%u", (unsigned int) opcode);
        GOperandCount = parseVarOperands(GOperands);
        op = &GExtendedOpcodes[opcode];
    } // if
    else
    {
        if (opcode <= 127)  // 2OP
        {
            GOperandCount = 2;
            parseOperand(((opcode >> 6) & 0x1) ? 2 : 1, GOperands + 0);
            parseOperand(((opcode >> 5) & 0x1) ? 2 : 1, GOperands + 1);
        } // if

        else if (opcode <= 175)  // 1OP
        {
            GOperandCount = 1;
            const uint8 optype = (opcode >> 4) & 0x3;
            parseOperand(optype, GOperands);  // 1OP or 0OP
        } // else if

        else if (opcode <= 191)  // 0OP
            GOperandCount = 0;

        else if (opcode > 191)  // VAR
        {
            const int takes8 = ((opcode == 236) || (opcode == 250));  // call_vs2 and call_vn2 take up to EIGHT arguments!
            if (!takes8)
                GOperandCount = parseVarOperands(GOperands);
            else
            {
                GOperandCount = parseVarOperands(GOperands);
                if (GOperandCount == 4)
                    GOperandCount += parseVarOperands(GOperands + 4);
                else
                    GPC++;  // skip the next byte, since we don't have any more args.
            } // else
        } // else

        op = &GOpcodes[opcode];
    } // if

    if (!op->name)
        die("Unsupported or unknown %sopcode #%u", extended ? "extended " : "", (unsigned int) opcode);
    else if (!op->fn)
        die("Unimplemented %sopcode #%d ('%s')", extended ? "extended " : "", (unsigned int) opcode, op->name);
    else
    {
        dbg("pc=%X %sopcode=%u ('%s') [", (unsigned int) GLogicalPC, extended ? "ext " : "", opcode, op->name);
        if (GOperandCount)
        {
            uint8 i;
            for (i = 0; i < GOperandCount-1; i++)
                dbg("%X,", (unsigned int) GOperands[i]);
            dbg("%X", (unsigned int) GOperands[i]);
        } // if
        dbg("]\n");
        op->fn();
        GInstructionsRun++;
    } // else
} // runInstruction

static void initAlphabetTable(void)
{
    FIXME("ver5+ specifies alternate tables in the header");

    char *ptr = GAlphabetTable;
    uint8 i;

    // alphabet A0
    for (i = 0; i < 26; i++)
        *(ptr++) = 'a' + i;

    // alphabet A1
    for (i = 0; i < 26; i++)
        *(ptr++) = 'A' + i;

    // alphabet A2
    *(ptr++) = '\0';

    if (GHeader.version != 1)
        *(ptr++) = '\n';

    for (i = 0; i < 10; i++)
        *(ptr++) = '0' + i;
    *(ptr++) = '.';
    *(ptr++) = ',';
    *(ptr++) = '!';
    *(ptr++) = '?';
    *(ptr++) = '_';
    *(ptr++) = '#';
    *(ptr++) = '\'';
    *(ptr++) = '"';
    *(ptr++) = '/';
    *(ptr++) = '\\';

    if (GHeader.version == 1)
        *(ptr++) = '<';

    *(ptr++) = '-';
    *(ptr++) = ':';
    *(ptr++) = '(';
    *(ptr++) = ')';
} // initAlphabetTable

static void initOpcodeTable(void)
{
    FIXME("lots of missing instructions here.  :)");

    memset(GOpcodes, '\0', sizeof (GOpcodes));
    memset(GExtendedOpcodes, '\0', sizeof (GExtendedOpcodes));

    Opcode *opcodes = GOpcodes;

    #define OPCODE(num, opname) opcodes[num].name = #opname; opcodes[num].fn = opcode_##opname
    #define OPCODE_WRITEME(num, opname) opcodes[num].name = #opname

    // this is the basic ver1 opcode table, then we can patch it after.
    // most early Infocom games are version 3, but apparently ver1 is in the wild...

    // 2-operand instructions...
    OPCODE(1, je);
    OPCODE(2, jl);
    OPCODE(3, jg);
    OPCODE(4, dec_chk);
    OPCODE(5, inc_chk);
    OPCODE(6, jin);
    OPCODE(7, test);
    OPCODE(8, or);
    OPCODE(9, and);
    OPCODE(10, test_attr);
    OPCODE(11, set_attr);
    OPCODE(12, clear_attr);
    OPCODE(13, store);
    OPCODE(14, insert_obj);
    OPCODE(15, loadw);
    OPCODE(16, loadb);
    OPCODE(17, get_prop);
    OPCODE(18, get_prop_addr);
    OPCODE(19, get_next_prop);
    OPCODE(20, add);
    OPCODE(21, sub);
    OPCODE(22, mul);
    OPCODE(23, div);
    OPCODE(24, mod);

    // 1-operand instructions...
    OPCODE(128, jz);
    OPCODE(129, get_sibling);
    OPCODE(130, get_child);
    OPCODE(131, get_parent);
    OPCODE(132, get_prop_len);
    OPCODE(133, inc);
    OPCODE(134, dec);
    OPCODE(135, print_addr);
    OPCODE(137, remove_obj);
    OPCODE(138, print_obj);
    OPCODE(139, ret);
    OPCODE(140, jump);
    OPCODE(141, print_paddr);
    OPCODE(142, load);
    OPCODE(143, not);

    // 0-operand instructions...
    OPCODE(176, rtrue);
    OPCODE(177, rfalse);
    OPCODE(178, print);
    OPCODE(179, print_ret);
    OPCODE(180, nop);
    OPCODE(181, save);
    OPCODE(182, restore);
    OPCODE(183, restart);
    OPCODE(184, ret_popped);
    OPCODE(185, pop);
    OPCODE(186, quit);
    OPCODE(187, new_line);

    // variable operand instructions...
    OPCODE(224, call);
    OPCODE(225, storew);
    OPCODE(226, storeb);
    OPCODE(227, put_prop);
    OPCODE(228, read);
    OPCODE(229, print_char);
    OPCODE(230, print_num);
    OPCODE(231, random);
    OPCODE(232, push);
    OPCODE(233, pull);

    if (GHeader.version < 3)  // most early Infocom games are version 3.
        return;  // we're done.

    OPCODE_WRITEME(188, show_status);
    OPCODE_WRITEME(189, verify);
    OPCODE_WRITEME(234, split_window);
    OPCODE_WRITEME(235, set_window);
    OPCODE_WRITEME(243, output_stream);
    OPCODE_WRITEME(244, input_stream);
    OPCODE_WRITEME(245, sound_effect);

    if (GHeader.version < 4)
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

    if (GHeader.version < 5)
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
    opcodes[180].fn = opcodes[181].fn = NULL;

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

    if (GHeader.version < 6)
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

static void finalizeOpcodeTable(void)
{
    uint8 i;
    for (i = 32; i <= 127; i++)  // 2OP opcodes repeating with different operand forms.
        GOpcodes[i] = GOpcodes[i % 32];
    for (i = 144; i <= 175; i++)  // 1OP opcodes repeating with different operand forms.
        GOpcodes[i] = GOpcodes[128 + (i % 16)];
    for (i = 192; i <= 223; i++)  // 2OP opcodes repeating with VAR operand forms.
        GOpcodes[i] = GOpcodes[i % 32];
} // finalizeOpcodeTable


static void loadStory(const char *fname)
{
    FILE *io;
    off_t len;

    if (GStory)
    {
        free(GStory);
        GStory = NULL;
    } // if

    GInstructionsRun = 0;
    GStoryLen = 0;
    GPC = 0;
    GLogicalPC = 0;
    GQuit = 0;
    memset(GStack, '\0', sizeof (GStack));
    memset(GOperands, '\0', sizeof (GOperands));
    GOperandCount = 0;
    GSP = NULL;  // stack pointer
    GBP = 0;  // base pointer
    free(GStoryFname);
    GStoryFname = NULL;

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

    fclose(io);

    memset(&GHeader, '\0', sizeof (GHeader));
    const uint8 *ptr = GStory;

    GStory[1] |= 0x8;  // report that we don't (currently) support a status bar.
    
    GHeader.version = READUI8(ptr);
    GHeader.flags1 = READUI8(ptr);
    GHeader.release = READUI16(ptr);
    GHeader.himem_addr = READUI16(ptr);
    GHeader.pc_start = READUI16(ptr);
    GHeader.dict_addr = READUI16(ptr);
    GHeader.objtab_addr = READUI16(ptr);
    GHeader.globals_addr = READUI16(ptr);
    GHeader.staticmem_addr = READUI16(ptr);
    GHeader.flags2 = READUI16(ptr);
    GHeader.serial_code[0] = READUI8(ptr);
    GHeader.serial_code[1] = READUI8(ptr);
    GHeader.serial_code[2] = READUI8(ptr);
    GHeader.serial_code[3] = READUI8(ptr);
    GHeader.serial_code[4] = READUI8(ptr);
    GHeader.serial_code[5] = READUI8(ptr);
    GHeader.abbrtab_addr = READUI16(ptr);
    GHeader.story_len = READUI16(ptr);
    GHeader.story_checksum = READUI16(ptr);

    dbg("Story '%s' header:\n", fname);
    dbg(" - version %u\n", (unsigned int) GHeader.version);
    dbg(" - flags 0x%X\n", (unsigned int) GHeader.flags1);
    dbg(" - release %u\n", (unsigned int) GHeader.release);
    dbg(" - high memory addr %X\n", (unsigned int) GHeader.himem_addr);
    dbg(" - program counter start %X\n", (unsigned int) GHeader.pc_start);
    dbg(" - dictionary address %X\n", (unsigned int) GHeader.dict_addr);
    dbg(" - object table address %X\n", (unsigned int) GHeader.objtab_addr);
    dbg(" - globals address %X\n", (unsigned int) GHeader.globals_addr);
    dbg(" - static memory address %X\n", (unsigned int) GHeader.staticmem_addr);
    dbg(" - flags2 0x%X\n", (unsigned int) GHeader.flags2);
    dbg(" - serial '%s'\n", GHeader.serial_code);
    dbg(" - abbreviations table address %X\n", (unsigned int) GHeader.abbrtab_addr);
    dbg(" - story length %u\n", (unsigned int) GHeader.story_len);
    dbg(" - story checksum 0x%X\n", (unsigned int) GHeader.story_checksum);

    if (GHeader.version != 3)
        die("FIXME: only version 3 is supported right now, this is %d", (int) GHeader.version);

    GStoryLen = (uintptr) len;
    GStoryFname = strdup(fname);

    srandom((unsigned long) time(NULL));

    initAlphabetTable();
    initOpcodeTable();
    finalizeOpcodeTable();

    FIXME("in ver6+, this is the address of a main() routine, not a raw instruction address.");
    GPC = GStory + GHeader.pc_start;
    GBP = 0;
    GSP = GStack;
} // loadStory

int main(int argc, char **argv)
{
    const char *fname = (argc >= 2) ? argv[1] : "zork1.dat";
    GStartupScript = (argc >= 3) ? argv[2] : NULL;
    loadStory(fname);

    while (!GQuit)
        runInstruction();

    dbg("ok.\n");

    free(GStory);
    free(GStoryFname);

    return 0;
} // main

// end of mojozork.c ...
