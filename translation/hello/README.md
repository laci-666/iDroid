# translation/hello — fixture „hello nativ” (docs/13, Pas 2)

`dn_hello.c` e cea mai mică bibliotecă ELF arm64 utilă pentru
poarta Pasului 2: fără importuri (zero simboluri nedefinite,
verificat cu `--no-undefined` la link), fără constructori, fără
libc — doar trei funcții de calcul (`dn_hello_answer/add/fib`) și
un text static (`dn_hello_text`).

Lanțul testat (vezi `scripts/make-hello-native.sh`):

1. Mac: compilare ELF (NDK / LLVM / zig, oricare e prezent).
2. Mac: conversie ELF→Mach-O + semnare cu identitatea utilizatorului
   prin `henge-convert` (unealtă de cercetare third-party, clonată
   și construită local de script; NU face parte din proiect).
3. iPhone: import în iDroid → container → `dlopen` + apel direct,
   din tabul Capabilities, secțiunea „Native translation test”.
   Rezultatele apar ca linii `[native-hello]` în consolă/export.

Întrebarea la care răspunde poarta: acceptă iOS 27 stoc, fără
debugger, cod semnat cu același team încărcat din container?
(Upstream, henge M1 a dovedit asta pe iPhone; noi o re-dovedim în
iDroid, cu identitatea noastră.) Dacă da, urmează semnarea și
conversia proprii, pe dispozitiv (identitatea din Keychain,
docs/14), apoi Pasul 3 (NativeActivity real).

## Stare (26.10.2026)

Utilizatorul a raportat PASS pe iPhone pentru poarta Pasului 2
(dlopen din container + apeluri directe cu valorile așteptate).
Fixture-ul rămâne înghețat ca regresie minimală a lanțului de
semnare/încărcare. Pentru arhiva formală perfectă mai rămâne de colectat
un export curat din lansare de pe iconiță, dar poarta funcțională nu mai
blochează dezvoltarea. Lucrarea a continuat la Pasul 2.5, driverul Vulkan 0.3.0 și
runtime-ul nativ complet 0.4.0 — vezi docs/13 și docs/15.
