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

## Identificazione del programma

Per il nome uso `current->comm`. E' disponibile direttamente nel task corrente
e il confronto nel pre-handler rimane breve, ma ci sono due conseguenze:

- il campo contiene al massimo 15 caratteri oltre al terminatore;
- identifica il task e puo' essere cambiato, quindi non equivale a un percorso
  completo dell'eseguibile.

Ho scelto registri con dimensione massima fissa. Si perde flessibilita', ma il
tempo del filtro resta limitato e non servono allocazioni dentro la probe.

Le letture indicizzate portano con se' la generazione della configurazione. Se
il registro cambia durante una lettura, l'utility riceve `EAGAIN` e ricomincia
la scansione invece di stampare una configurazione mista.

## Significato di MAX e finestra temporale

La traccia non specifica se il limite sia per processo o complessivo. Ho scelto
un solo limite globale condiviso da tutte le chiamate selezionate. E' la
lettura piu' semplice della frase "numero massimo di system call" e permette di
verificare facilmente il risultato con piu' processi concorrenti.

Per il secondo uso una finestra mobile, non intervalli allineati all'orologio.
Il monitor conserva i timestamp delle chiamate ammesse in un buffer circolare:
prima di ammettere una nuova chiamata elimina quelli vecchi di almeno un
secondo. Se il buffer contiene gia' `MAX` elementi, il processo dorme fino alla
prima scadenza utile.

Ogni modifica della configurazione incrementa `wake_generation` e sveglia la
wait queue. Il processo confronta la generazione salvata e rivaluta tutto lo
stato, perche' durante l'attesa potrebbero essere cambiati `MAX`, il monitor o
uno dei registri.

## Statistiche

La media dei thread bloccati non puo' essere calcolata facendo la media di
campioni presi a intervalli arbitrari. Mantengo invece l'integrale nel tempo:
ogni volta che il numero di waiter cambia aggiungo
`durata * thread_bloccati` al totale. Dividendo per il tempo trascorso ottengo
la media richiesta.

Il reset non porta a zero `current_blocked_threads`, perche' quei processi sono
ancora realmente in attesa. Azzera l'intervallo precedente e usa il numero
corrente come valore iniziale e come primo picco del nuovo intervallo.
