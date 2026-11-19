// dn_link.c — main half of the docs/13 step-2.5 fixture.
//
// Unlike the step-2 hello fixture, this library is not dependency
// free. It exercises the pieces a real Android .so needs immediately:
// DT_NEEDED, imported functions/data, PLT/GOT binding, function and
// data pointer relocations, a constructor that may only run after
// imports are bound, and a destructor run by the linker finalizer.
//
// Host imports are intentionally fixed-argument functions. Variadic
// Android->Darwin calls need a separate ABI bridge and are not faked
// here.

extern int dn_base_value;
extern int dn_base_add(int a, int b);
extern const char *dn_base_text(void);

/* Provided by DroidNative's own DNNativeShims.c. */
extern int dn_native_selftest_add(int a, int b);

/* Provided by the host C library; compiled with -fno-builtin so the
 * constructor really calls through the PLT instead of folding it. */
extern unsigned long strlen(const char *s);

typedef int (*dn_link_op_fn)(int);

static int dn_link_inc(int x)
{
    return x + 1;
}

static int dn_link_triple(int x)
{
    return x * 3;
}

/* A relocated function-pointer table (.data.rel.ro on ELF).  It is
 * exported with hidden visibility and indexed through a volatile
 * selector so an optimizing compiler cannot fold the table reads
 * away; the device test must observe real dynamic relocations. */
static volatile int dn_link_op_index = 1;
__attribute__((visibility("hidden"), used))
dn_link_op_fn dn_link_ops[] = {
    dn_link_inc,
    dn_link_triple,
};

static const char dn_link_msg[] = "dn-link-native";
static const char *volatile dn_link_msg_ptr = dn_link_msg;

static volatile int dn_link_ctor_count_value;
static volatile int dn_link_state_value;
static volatile int dn_link_finalized_value;

__attribute__((constructor, used))
static void dn_link_init(void)
{
    dn_link_ctor_count_value++;
    /* Dependencies and PLT entries must already be bound here:
     * dn_base_value is 19 after the base constructor and the string
     * is 14 bytes long, so a correctly linked image observes 33. */
    dn_link_state_value = dn_base_value + (int)strlen(dn_link_msg_ptr);
}

__attribute__((destructor, used))
static void dn_link_fini(void)
{
    dn_link_finalized_value = 1;
}

__attribute__((visibility("default")))
int dn_link_compute(int x)
{
    return dn_link_ops[dn_link_op_index](x) + dn_base_add(2, 3) +
           dn_native_selftest_add(4, 5);
}

__attribute__((visibility("default")))
int dn_link_state(void)
{
    return dn_link_state_value;
}

__attribute__((visibility("default")))
int dn_link_ctor_calls(void)
{
    return dn_link_ctor_count_value;
}

__attribute__((visibility("default")))
int dn_link_pointer_check(void)
{
    const char *base_text = dn_base_text();
    if (dn_link_ops[dn_link_op_index - 1](41) != 42)
        return 1;
    if (dn_link_msg_ptr[0] != 'd' || dn_link_msg_ptr[3] != 'l')
        return 2;
    if (base_text[0] != 'd' || base_text[3] != 'b')
        return 3;
    return 42;
}

__attribute__((visibility("default")))
const char *dn_link_text(void)
{
    return dn_link_msg_ptr;
}

__attribute__((visibility("default")))
int dn_link_finalized(void)
{
    return dn_link_finalized_value;
}
