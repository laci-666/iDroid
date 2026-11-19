// dn_base.c — dependency half of the docs/13 step-2.5 fixture.
//
// This is deliberately a *real* shared-library dependency, not a
// static convenience object: libdnlink.so carries DT_NEEDED for
// libdnbase.so and imports both a function and a data object from it.
// The DN mini-linker must link this image first, run its constructor,
// and only then bind the main library.

__attribute__((visibility("default")))
int dn_base_value = 17;

static volatile int dn_base_ctor_count_value;
static volatile int dn_base_finalized_value;
static const char dn_base_msg[] = "dn-base";
static const char *volatile dn_base_msg_ptr = dn_base_msg;

__attribute__((constructor, used))
static void dn_base_init(void)
{
    dn_base_ctor_count_value++;
    dn_base_value = 19;
}

__attribute__((destructor, used))
static void dn_base_fini(void)
{
    dn_base_finalized_value = 1;
}

__attribute__((visibility("default")))
int dn_base_add(int a, int b)
{
    return a + b + dn_base_value;
}

__attribute__((visibility("default")))
int dn_base_mul(int a, int b)
{
    return a * b;
}

__attribute__((visibility("default")))
const char *dn_base_text(void)
{
    return dn_base_msg_ptr;
}

__attribute__((visibility("default")))
int dn_base_ctor_calls(void)
{
    return dn_base_ctor_count_value;
}

__attribute__((visibility("default")))
int dn_base_finalized(void)
{
    return dn_base_finalized_value;
}
