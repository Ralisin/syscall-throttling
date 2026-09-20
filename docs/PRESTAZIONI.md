# Prestazioni

Lo scopo del modulo e' rallentare le system call quando viene superato un
limite. C'e' pero' una domanda altrettanto importante: quanto costa il modulo
quando il limite non viene raggiunto e il processo puo' proseguire subito?

Per rispondere abbiamo misurato `getpid` e `read` in quattro situazioni, dalla
baseline senza modulo fino al percorso completo attraverso hook e monitor. In
questo modo possiamo separare il costo dell'intercettazione dal ritardo voluto
del throttling.

## Come abbiamo svolto la prova

Il programma `tests/perf_probe.c` invoca direttamente le due syscall; nel caso
di `read` legge un byte alla volta da `/dev/zero`. Prima delle misure esegue
10000 chiamate di riscaldamento, poi raccoglie 100000 campioni mantenendo il
processo sulla stessa CPU.

Abbiamo ripetuto ogni scenario cinque volte:

| Scenario | Che cosa succede |
| --- | --- |
| Modulo assente | E' la misura di riferimento. |
| Modulo caricato e disabilitato | La kprobe esegue il filtro, ma il monitor e' spento. |
| Attivo, nessuna corrispondenza | Il monitor e' acceso, ma il path dell'eseguibile non rientra nei filtri. |
| Selezionata, senza throttling | La chiamata attraversa hook, wrapper e monitor, ma `MAX` e' abbastanza alto da non farla dormire. |

La prova e' stata eseguita sulla stessa VM usata per la verifica del progetto:

- Ubuntu 24.04.4 LTS x86-64;
- kernel `7.0.0-31-generic`;
- VMware su AMD Ryzen 9 9900X 12-Core Processor;
- CPU virtuale 0;
- GCC 13.3.0 e compilazione con `make W=1`.

## Risultati

Il valore piu' utile per il confronto e' la mediana, perche' risente meno delle
interruzioni occasionali della VM. Nella tabella riportiamo la mediana delle
cinque ripetizioni. Delta e percentuale sono calcolati rispetto al modulo
assente.

| Scenario | `getpid` mediana [ns] | Delta [ns] | Overhead | `read` mediana [ns] | Delta [ns] | Overhead |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Modulo assente | 17600 | - | riferimento | 18000 | - | riferimento |
| Modulo caricato e disabilitato | 18600 | +1000 | +5.68% | 19300 | +1300 | +7.22% |
| Attivo, nessuna corrispondenza | 19000 | +1400 | +7.95% | 19101 | +1101 | +6.12% |
| Selezionata, senza throttling | 29600 | +12000 | +68.18% | 29700 | +11700 | +65.00% |

Il primo risultato interessante e' che tenere il modulo caricato costa circa
1.0-1.3 microsecondi per chiamata nella configurazione provata, tra il 5.68% e
il 7.22%. Accendere il monitor senza selezionare il processo mantiene un costo
dello stesso ordine: il percorso si ferma ancora al filtro.

Quando la chiamata viene selezionata, invece, entrano in gioco il wrapper, il
mutex del monitor e la gestione della finestra temporale. Il costo aggiuntivo
sale a circa 11.7-12.0 microsecondi, cioe' tra il 65.00% e il 68.18%. In
questa prova nessun task viene sospeso: stiamo misurando il costo del
controllo, non il ritardo imposto da `MAX`.

## Da dove arriva l'overhead

Con il modulo caricato, la kprobe viene eseguita all'ingresso del dispatcher e
consulta sempre lo stato di configurazione. I registri hanno dimensione massima
fissa e la ricerca e' lineare: il costo del filtro e' quindi limitato, ma cresce
con il numero di syscall, path ed EUID registrati.

Una chiamata selezionata esegue inoltre il wrapper, aggiorna contatori atomici,
acquisisce il mutex del monitor, elimina i timestamp scaduti e inserisce il
nuovo timestamp nel buffer circolare. Sotto concorrenza, il mutex condiviso e'
il principale punto di serializzazione.

Il buffer contiene fino a `ST_MAX_LIMIT` timestamp da 64 bit. Con il limite
attuale di 1000000 elementi richiede 8000000 byte, circa 7.63 MiB, allocati al
caricamento del modulo indipendentemente dal valore configurato di `MAX`.

Quando `MAX` viene davvero raggiunto, il tempo di attesa non e' overhead
accidentale ma il comportamento richiesto. Il task dorme sulla wait queue fino
alla prima scadenza utile, senza consumare CPU in busy waiting.

## Ripetere il benchmark

Il modulo deve essere inizialmente assente. Dopo avere compilato il progetto,
la stessa prova si esegue con:

```sh
make
sudo env SAMPLES=100000 REPETITIONS=5 \
    ./scripts/benchmark-performance.sh > performance.csv
```

Lo script sceglie la prima CPU disponibile; si puo' indicarne una con
`BENCHMARK_CPU`. Nel CSV salva anche kernel, modello della CPU e parametri della
prova.
