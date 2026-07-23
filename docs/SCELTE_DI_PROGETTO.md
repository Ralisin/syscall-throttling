# Scelte di progetto

Questo file raccoglie i dubbi incontrati durante lo sviluppo e le motivazioni
delle soluzioni adottate.

## Questioni iniziali

- Capire se `MAX` deve essere globale oppure separato per ogni programma.
- Stabilire che cosa si intende esattamente per nome del programma.
- Scegliere un meccanismo di intercettazione compatibile con il kernel della VM.
- Evitare qualsiasi attesa attiva nel percorso delle syscall.

Le decisioni verranno aggiunte dopo aver verificato le alternative sulla VM.

## Intercettazione delle syscall

La prima idea era usare ftrace sulle funzioni `__x64_sys_*`, perche' avrebbe
permesso di registrare soltanto le syscall richieste. Sulla VM, pero', non tutte
le funzioni interessanti compaiono in `available_filter_functions`: in
particolare `__x64_sys_getpid` e' visibile nei simboli ma non e' agganciabile in
quel modo.

Il simbolo `x64_sys_call` e' invece disponibile per una kprobe. Provero' quindi
un solo hook sul dispatcher e faro' il filtro nel pre-handler. Il limite di
questa soluzione e' che dipende dall'implementazione x86-64 del kernel usato
nella VM.

Lo script `experiments/check-interception.sh` raccoglie i controlli fatti prima
di scegliere il meccanismo.
