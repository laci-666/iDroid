// dn_hello.c — Step 2 fixture (docs/13): a trivial arm64 ELF shared
// library with ZERO imported symbols. It proves the "hello nativ"
// chain end to end: ELF -> Mach-O conversion on the Mac, signing
// with the user's team identity, then dlopen() + direct calls from
// inside iDroid on stock iOS (no JIT, no debugger).
//
// Keep it dependency-free forever: no libc calls, no global
// constructors, no TLS — so the converted library loads with the
// plain system dyld and every failure is attributable to exactly
// one link of the chain.
static const char dn_hello_msg[] =
    "hello from translated native code (dn_hello, docs/13 step 2)";

__attribute__((visibility("default")))
int dn_hello_answer(void)
{
    return 42;
}

__attribute__((visibility("default")))
int dn_hello_add(int a, int b)
{
    return a + b;
}

__attribute__((visibility("default")))
long dn_hello_fib(int n)
{
    long a = 0, b = 1;
    for (int i = 0; i < n; i++) {
        long t = a + b;
        a = b;
        b = t;
    }
    return a;
}

__attribute__((visibility("default")))
const char *dn_hello_text(void)
{
    return dn_hello_msg;
}
