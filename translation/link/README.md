# translation/link — fixture pentru mini-linkerul propriu (docs/13, Pas 2.5)

> **Stare (26.10.2026):** implementarea Pasului 2.5 a fost livrată în
> pachetul reparat 0.2.46 și trece testele host. Un export separat de pe
> dispozitiv cu verdictul explicit `[native-link]` nu este încă arhivat
> formal; valorile de mai jos sunt criteriul de acceptanță, nu o dovadă
> nouă inventată retrospectiv.

`dn_base.c` și `dn_link.c` formează prima pereche de biblioteci care
nu mai poate fi rulată de rezolvitorul minimal din Pasul 2:

- `libdnlink.so` are `DT_NEEDED` pe `libdnbase.so`;
- importă funcție + variabilă din dependență;
- importă `strlen` din gazdă și `dn_native_selftest_add` din
  shim-urile proprii DN;
- conține pointeri la funcții/date care cer relocări;
- constructorul folosește importurile, deci e valid doar după
  legarea completă;
- destructorul este observat prin finalizatoarele linkerului.

Lanțul de test:

1. Mac: `scripts/make-link-native.sh` compilează ELF-urile, le
   convertește în Mach-O și le semnează exact ca fixture-ul Pas 2.
2. iPhone: Capabilities → „Native runtime linker (docs/13 step 2.5)”,
   importă ambele dylib-uri și apasă „Link and call (own mini-linker)”.
3. Verdictul apare în UI și ca linii `[native-link]` în exportul de
   consolă.

Valori așteptate:

- `dn_base_add(20, 3) = 42` (după constructor, `dn_base_value = 19`);
- `dn_link_state() = 33` (`19 + strlen("dn-link-native")`);
- `dn_link_compute(10) = 63`;
- fiecare constructor rulează o singură dată;
- după finalizatoare, `dn_link_finalized() = 1`.

Acesta rămâne fixture-ul de regresie al fundației de încărcare/legare.
După el, 0.3.0 a adăugat Vulkan-on-Metal, iar 0.4.0 a implementat
patch-urile `svc`/`tpidr_el0`, TLS/APS2, bionic, libandroid, input,
assets, AAudio clocked, JNI local și host NativeActivity în
`translation/full`. Conversia/semnarea proprii pe dispozitiv și
compatibilitatea cu titluri Android reale rămân etape separate.
