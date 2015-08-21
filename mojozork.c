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
#if 1
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
typedef uint_fast8_t uint8fast;
typedef int_fast8_t sint8fast;
typedef uint_fast16_t uint16fast;
typedef int_fast16_t sint16fast;
typedef uint_fast32_t uint32fast;
typedef int_fast32_t sint32fast;

typedef size_t uintptr;

// !!! FIXME: maybe kill these.
#define READUI8(ptr) *(ptr++)
#define READUI16(ptr) ((((uint16fast) ptr[0]) << 8) | ((uint16fast) ptr[1])); ptr += sizeof (uint16)
#define WRITEUI16(dst, src) { *(dst++) = (src >> 8) & 0xFF; *(dst++) = (src >> 0) & 0xFF; }

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

static uint32fast GInstructionsRun = 0;
static uint8 *GStory = NULL;
static uintptr GStoryLen = 0;
static ZHeader GHeader;
static const uint8 *GPC = 0;
static uint16fast GLogicalPC = 0;
static int GQuit = 0;
static uint16 GStack[2048];  // !!! FIXME: make this dynamic?
static uint16 GOperands[8];
static uint8fast GOperandCount = 0;
static uint16 *GSP = NULL;  // stack pointer
static uint16fast GBP = 0;  // base pointer
static char GAlphabetTable[78];

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
static uint8 *unpackAddress(const uint32fast addr)
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

static uint8 *varAddress(const uint8fast var, const int writing)
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

            const uint16fast numlocals = GBP ? GStack[GBP-1] : 0;
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
    uint8fast args = GOperandCount;
    const uint16 *operands = GOperands;
    const uint8fast storeid = *(GPC++);
    // no idea if args==0 should be the same as calling addr 0...
    if ((args == 0) || (operands[0] == 0))  // legal no-op; store 0 to return value and bounce.
    {
        uint8 *store = varAddress(storeid, 1);
        WRITEUI16(store, 0);
    } // if
    else
    {
        const uint8 *routine = unpackAddress(operands[0]);
        GLogicalPC = (uint16fast) (routine - GStory);
        const uint8fast numlocals = *(routine++);
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

        GBP = (uint16fast) (GSP-GStack);

        sint8fast i;
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

static void doReturn(const uint16fast val)
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

    const uint8fast storeid = (uint8fast) *(--GSP);  // pop the result storage location.

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
    const uint16fast result = READUI16(ptr);
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
    const uint16fast val = READUI16(ptr);
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
    const int farjump = (branch & (1<<6)) != 0;
    const uint8 byte2 = farjump ? 0 : *(GPC++);
    const int onTruth = (branch & (1<<7)) ? 1 : 0;
    if (truth == onTruth)  // take the branch?
    {
        sint16fast offset = (sint16fast) (branch & 0x3F);
        if (farjump)
            offset = (byte2 << 8) | ((uint16fast) offset);

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
    const uint16fast a = GOperands[0];
    sint8fast i;
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
    doBranch((((sint16fast) GOperands[0]) < ((sint16fast) GOperands[1])) ? 1 : 0);
} // opcode_jl

static void opcode_jg(void)
{
    doBranch((((sint16fast) GOperands[0]) > ((sint16fast) GOperands[1])) ? 1 : 0);
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
    const uint16fast result = (GOperands[0] | GOperands[1]);
    WRITEUI16(store, result);
} // opcode_or

static void opcode_and(void)
{
    uint8 *store = varAddress(*(GPC++), 1);
    const uint16fast result = (GOperands[0] & GOperands[1]);
    WRITEUI16(store, result);
} // opcode_and

static void opcode_not(void)
{
    uint8 *store = varAddress(*(GPC++), 1);
    const uint16fast result = ~GOperands[0];
    WRITEUI16(store, result);
} // opcode_not

static void opcode_inc_chk(void)
{
    uint8 *store = varAddress((uint8fast) GOperands[0], 1);
    sint16 val = READUI16(store);
    store -= sizeof (uint16);
    val++;
    WRITEUI16(store, (uint16) val);
    doBranch( (((sint16fast) val) > ((sint16fast) GOperands[1])) ? 1 : 0 );
} // opcode_inc_chk

static void opcode_inc(void)
{
    uint8 *store = varAddress((uint8fast) GOperands[0], 1);
    sint16 val = (sint16) READUI16(store);
    store -= sizeof (uint16);
    val++;
    WRITEUI16(store, (uint16) val);
} // opcode_inc

static void opcode_dec_chk(void)
{
    uint8 *store = varAddress((uint8fast) GOperands[0], 1);
    sint16 val = (sint16) READUI16(store);
    store -= sizeof (uint16);
    val--;
    WRITEUI16(store, (uint16) val);
    doBranch( (((sint16fast) val) < ((sint16fast) GOperands[1])) ? 1 : 0 );
} // opcode_dec_chk

static void opcode_dec(void)
{
    uint8 *store = varAddress((uint8fast) GOperands[0], 1);
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
    uint8 *src = GStory + (GOperands[0] + (GOperands[1]));
    *store = *src;  // copy from 1 byte: no byteswap.
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
    uint8 *store = varAddress((uint8fast) (GOperands[0] & 0xFF), 1);
    const uint16 src = GOperands[1];
    WRITEUI16(store, src);
} // opcode_store


static uint8 *getObjectPtr(const uint16fast objid)
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
    const uint16fast objid = GOperands[0];
    const uint16fast attrid = GOperands[1];
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
    const uint16fast objid = GOperands[0];
    const uint16fast attrid = GOperands[1];
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
    const uint16fast objid = GOperands[0];
    const uint16fast attrid = GOperands[1];
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
        const uint16fast parent = *objptr;
        return parent ? getObjectPtr(parent) : NULL;
    }
    else
    {
        die("write me");
    } // else
} // getGetObjectPtrParent

static void opcode_insert_obj(void)
{
    const uint16fast objid = GOperands[0];
    const uint16fast dstid = GOperands[1];

    uint8 *objptr = getObjectPtr(objid);
    uint8 *dstptr = getObjectPtr(dstid);
    uint8 *parentptr = getObjectPtrParent(objptr);

    if (GHeader.version <= 3)
    {
        // this child/sibling thing is messy. There's a lot of tapdancing to
        //  move stuff around.

        // Let's take the object out of its original tree first.
        if (parentptr != NULL)  // if NULL, no need to remove it.
        {   //p s c
            uint8 *ptr = objptr + 6;  // 4 to skip attrs, 2 to skip to child.
            if (*ptr != objid) // objid is _not_ direct child.
            {
                do  // ugh, have to look through sibling list...
                {
                    ptr = getObjectPtr(*ptr) + 5;  // get actual direct child's sibling field.
                } while (*ptr != objid);
            } // if
            *ptr = *(objptr + 5);  // obj sibling takes obj's place.
        } // if

        // now insert in the right place.
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
    const uint16fast objid = GOperands[0];

    uint8 *objptr = getObjectPtr(objid);
    uint8 *parentptr = getObjectPtrParent(objptr);

    if (GHeader.version <= 3)
    {
        // this child/sibling thing is messy. There's a lot of tapdancing to
        //  move stuff around.

        // Let's take the object out of its original tree.
        if (parentptr != NULL)  // if NULL, no need to remove it.
        {
            uint8 *ptr = objptr + 6;  // 4 to skip attrs, 2 to skip to child.
            if (*ptr != objid) // objid is _not_ direct child.
            {
                do  // ugh, have to look through sibling list...
                {
                    ptr = getObjectPtr(*ptr) + 5;  // get actual direct child's sibling field.
                } while (*ptr != objid);
            } // if
            *ptr = *(objptr + 5);  // obj sibling takes obj's place.
        } // if

        // now clear out object's relationships...
        *(objptr + 4) = 0;  // parent field: zero.
        *(objptr + 5) = 0;  // sibling field: zero.
    } // if
    else
    {
        die("write me");  // fields are different in ver4+.
    } // else
} // opcode_remove_obj

static void opcode_put_prop(void)
{
    const uint16fast objid = GOperands[0];
    const uint16fast propid = GOperands[1];
    const uint16fast value = GOperands[2];

    uint8 *ptr = getObjectPtr(objid);

    if (GHeader.version <= 3)
    {
        ptr += 7;  // skip to properties address field.
        const uint16fast addr = READUI16(ptr);
        ptr = GStory + addr;
        ptr += (*ptr * 2) + 1;  // skip object name to start of properties.
        while (1)
        {
            const uint8fast info = *(ptr++);
            const uint16fast num = (info & 0x1F);  // 5 bits for the prop id.
            const uint8fast size = ((info >> 5) & 0x7) + 1; // 3 bits for prop size.
            // these go in descending numeric order, and should fail
            //  the interpreter if missing.
            if (num < propid)
                die("put_prop on missing object property (obj=%X, prop=%X, val=%x)", (unsigned int) objid, (unsigned int) propid, (unsigned int) value);
            else if (num == propid)  // found it?
            {
                if (size == 1)
                    *ptr = (value & 0xFF);
                else
                {
                    WRITEUI16(ptr, value);
                } // else
                return;
            } // else if

            ptr += size;  // try the next property.
        } // while
    } // if
    else
    {
        die("write me");
    } // else
} // opcode_put_prop

static uint16fast getDefaultObjectProperty(const uint16fast propid)
{
    if ( ((GHeader.version <= 3) && (propid > 31)) ||
         ((GHeader.version >= 4) && (propid > 63)) )
    {
        FIXME("Should we die here?");
        return 0;
    } // if

    const uint16 *values = (const uint16 *) (GStory + GHeader.objtab_addr);
    return values[propid];
} // getDefaultObjectProperty

static void opcode_get_prop(void)
{
    uint8 *store = varAddress(*(GPC++), 1);
    const uint16fast objid = GOperands[0];
    const uint16fast propid = GOperands[1];
    uint16fast result = 0;

    uint8 *ptr = getObjectPtr(objid);

    if (GHeader.version <= 3)
    {
        ptr += 7;  // skip to properties address field.
        const uint16fast addr = READUI16(ptr);
        ptr = GStory + addr;
        ptr += (*ptr * 2) + 1;  // skip object name to start of properties.
        while (1)
        {
            const uint8fast info = *(ptr++);
            const uint16fast num = (info & 0x1F);  // 5 bits for the prop id.
            const uint8fast size = ((info >> 5) & 0x7) + 1; // 3 bits for prop size.
            // these go in descending numeric order
            if (num < propid)  // missing for this object? Use the default.
            {
                result = getDefaultObjectProperty(propid);
                break;
            } // if
            else if (num == propid)  // found it?
            {
                if (size == 1)
                    result = *ptr;
                else
                {
                    result = READUI16(ptr);
                } // else
                break;
            } // else if

            ptr += size;  // try the next property.
        } // while
    } // if
    else
    {
        die("write me");
    } // else

    WRITEUI16(store, result);
} // opcode_get_prop

static void opcode_get_prop_addr(void)
{
    FIXME("So much code dupe");
    uint8 *store = varAddress(*(GPC++), 1);
    const uint16fast objid = GOperands[0];
    const uint16fast propid = GOperands[1];
    uint16fast result = 0;

    uint8 *ptr = getObjectPtr(objid);

    if (GHeader.version <= 3)
    {
        ptr += 7;  // skip to properties address field.
        const uint16fast addr = READUI16(ptr);
        ptr = GStory + addr;
        ptr += (*ptr * 2) + 1;  // skip object name to start of properties.
        while (1)
        {
            const uint8fast info = *(ptr++);
            const uint16fast num = (info & 0x1F);  // 5 bits for the prop id.
            const uint8fast size = ((info >> 5) & 0x7) + 1; // 3 bits for prop size.
            // these go in descending numeric order
            if (num < propid)  // missing for this object?
                break;
            else if (num == propid)  // found it?
            {
                result = (uint16fast) (ptr - GStory);
                break;
            } // else if

            ptr += size;  // try the next property.
        } // while
    } // if
    else
    {
        die("write me");
    } // else

    WRITEUI16(store, result);
} // opcode_get_prop_addr

static void opcode_get_prop_len(void)
{
    uint8 *store = varAddress(*(GPC++), 1);
    const uint8 *ptr = GStory + GOperands[0];
    uint16fast result = 0;

    if (GOperands[0] == 0)
        result = 0;  // this must return 0, to avoid a bug in older Infocom games.
    else if (GHeader.version <= 3)
    {
        ptr--;  // go back to size field.
        const uint8fast info = *ptr;
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
    FIXME("So much code dupe");
    uint8 *store = varAddress(*(GPC++), 1);
    const uint16fast objid = GOperands[0];
    const uint16fast propid = GOperands[1];
    int thisone = (propid == 0);  // zero == "first property in the list"
    uint16fast result = 0;

    uint8 *ptr = getObjectPtr(objid);

    if (GHeader.version <= 3)
    {
        ptr += 7;  // skip to properties address field.
        const uint16fast addr = READUI16(ptr);
        ptr = GStory + addr;
        ptr += (*ptr * 2) + 1;  // skip object name to start of properties.
        while (1)
        {
            const uint8fast info = *(ptr++);
            const uint16fast num = (info & 0x1F);  // 5 bits for the prop id.
            const uint8fast size = ((info >> 5) & 0x7) + 1; // 3 bits for prop size.
            // these go in descending numeric order
            if (thisone)
            {
                result = num;
                break;
            } // if

            else if (num < propid)  // missing for this object?
                die("get_next_prop on missing propery");

            else if (num == propid)  // found it?
                thisone = 1;

            ptr += size;  // try the next property.
        } // while
    } // if
    else
    {
        die("write me");
    } // else

    WRITEUI16(store, result);
} // opcode_get_next_prop

static void opcode_jin(void)
{
    const uint16fast objid = GOperands[0];
    const uint16fast parentid = GOperands[1];
    const uint8 *objptr = getObjectPtr(objid);

    if (GHeader.version <= 3)
        doBranch((((uint16fast) objptr[4]) == parentid) ? 1 : 0);
    else
    {
        die("write me");  // fields are different in ver4+.
    } // else
} // opcode_jin

static uint16fast getObjectRelationship(const uint16fast objid, const uint8fast relationship)
{
    const uint8 *objptr = getObjectPtr(objid);

    if (GHeader.version <= 3)
        return objptr[relationship];
    else
    {
        die("write me");  // fields are different in ver4+.
    } // else
} // getObjectRelationship

static void opcode_get_parent(void)
{
    uint8 *store = varAddress(*(GPC++), 1);
    const uint16fast result = getObjectRelationship(GOperands[0], 4);
    WRITEUI16(store, result);
} // opcode_get_parent

static void opcode_get_sibling(void)
{
    uint8 *store = varAddress(*(GPC++), 1);
    const uint16fast result = getObjectRelationship(GOperands[0], 5);
    WRITEUI16(store, result);
    doBranch((result != 0) ? 1: 0);
} // opcode_get_sibling

static void opcode_get_child(void)
{
    uint8 *store = varAddress(*(GPC++), 1);
    const uint16fast result = getObjectRelationship(GOperands[0], 6);
    WRITEUI16(store, result);
    doBranch((result != 0) ? 1: 0);
} // opcode_get_child

static void opcode_new_line(void)
{
    putchar('\n');
    fflush(stdout);
} // opcode_new_line

static void print_zscii_char(const uint16fast val)
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
    uint16fast code = 0;
    uint8fast alphabet = 0;
    uint8fast useAbbrTable = 0;
    uint8fast zscii_collector = 0;
    uint16fast zscii_code = 0;

    do
    {
        code = READUI16(str);

        // characters are 5 bits each, packed three to a 16-bit word.
        sint8fast i;
        for (i = 10; i >= 0; i -= 5)
        {
            int newshift = 0;
            char printVal = 0;
            const uint8fast ch = ((code >> i) & 0x1F);

            if (zscii_collector)
            {
                if (zscii_collector == 2)
                    zscii_code |= ((uint16fast) ch) << 5;
                else
                    zscii_code |= ((uint16fast) ch);

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
                const uint16fast abbraddr = READUI16(ptr);
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
} // opcode_print_ret

static void opcode_print_obj(void)
{
    uint8 *ptr = getObjectPtr(GOperands[0]);

    if (GHeader.version <= 3)
    {
        ptr += 7;  // skip to properties field.
        const uint16fast addr = READUI16(ptr);  // dereference to get to property table.
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

static void opcode_random(void)
{
    uint8 *store = varAddress(*(GPC++), 1);
    const sint16fast range = (sint16fast) GOperands[0];
    uint16fast result = 0;

    if (range == 0)  // reseed in "most random way"
        srandom((unsigned long) time(NULL));
    else if (range < 0)  // reseed with specific value
        srandom(-range);
    else
    {
        FIXME("this is sucky");
        long r = random();
        r = (r >> (sizeof (r) / 2) * 8) ^ r;
        result = (uint16fast) ((((float) (r & 0xFFFF)) / 65535.0f) * ((float) range));
    } // else

    WRITEUI16(store, result);
} // opcode_random

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


static int parseOperand(const uint8fast optype, uint16 *operand)
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

static uint8fast parseVarOperands(uint16 *operands)
{
    const uint8fast operandTypes = *(GPC++);
    uint8fast shifter = 6;
    uint8fast i;

    for (i = 0; i < 4; i++)
    {
        const uint8fast optype = (operandTypes >> shifter) & 0x3;
        shifter -= 2;
        if (!parseOperand(optype, operands + i))
            break;
    } // for

    return i;
} // parseVarOperands


static void runInstruction(void)
{
    FIXME("verify PC is sane");

    GLogicalPC = (uint16fast) (GPC - GStory);
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
            const uint8fast optype = (opcode >> 4) & 0x3;
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
            uint8fast i;
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
    uint8fast i;

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
    OPCODE_WRITEME(181, save);
    OPCODE_WRITEME(182, restore);
    OPCODE_WRITEME(183, restart);
    OPCODE(184, ret_popped);
    OPCODE(185, pop);
    OPCODE(186, quit);
    OPCODE(187, new_line);

    // variable operand instructions...
    OPCODE(224, call);
    OPCODE(225, storew);
    OPCODE(226, storeb);
    OPCODE(227, put_prop);
    OPCODE_WRITEME(228, read);
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
    uint8fast i;
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

    fclose(io);
    GStoryLen = (uintptr) len;
} // loadStory


int main(int argc, char **argv)
{
    const char *fname = argv[1] ? argv[1] : "zork1.dat";
    loadStory(fname);

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

    srandom((unsigned long) time(NULL));

    initAlphabetTable();
    initOpcodeTable();
    finalizeOpcodeTable();

    FIXME("in ver6+, this is the address of a main() routine, not a raw instruction address.");
    GPC = GStory + GHeader.pc_start;
    GBP = 0;
    GSP = GStack;

    while (!GQuit)
        runInstruction();

    dbg("ok.\n");

    free(GStory);
    return 0;
} // main

// end of mojozork.c ...

