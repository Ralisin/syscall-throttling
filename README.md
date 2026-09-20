# Syscall Throttling LKM

Questo progetto implementa un modulo kernel Linux per limitare la frequenza di
alcune system call su x86-64. Le chiamate da controllare vengono scelte in base
al numero della syscall e al path dell'eseguibile oppure all'EUID del processo.
Quando il limite e' stato raggiunto, il processo viene messo in attesa senza
fare busy waiting.

Il kernel risolve il path in configurazione e registra la directory e il
basename risultanti. La sostituzione del file nella stessa posizione continua
quindi a essere intercettata, senza risolvere pathname nel pre-handler.

La configurazione passa attraverso il device `/dev/syscall_throttle`. Il
programma `throttle_ctl` permette di aggiungere o rimuovere path, UID e
syscall, attivare il monitor e leggere le statistiche.

## Ambiente usato

Il progetto e' stato compilato e provato nella seguente VM:

- Ubuntu 24.04.4 LTS x86-64;
- kernel `7.0.0-31-generic`;
- GCC 13.3.0;
- GNU Make 4.3;
- Secure Boot disabilitato.

L'intercettazione dipende dal dispatcher `x64_sys_call`, quindi non considero
il modulo portabile senza verifiche su versioni o architetture diverse.

## Come funziona

Il modulo registra una kprobe sul dispatcher comune delle syscall. Il
pre-handler controlla soltanto se la chiamata interessa il monitor e, se
necessario, sposta l'esecuzione su un wrapper del modulo. Il pre-handler non si
blocca e non alloca memoria.

Il wrapper esegue il controllo sul limite. Se c'e' posto nella finestra mobile
di un secondo, richiama subito il dispatcher originale. Altrimenti dorme su una
wait queue fino alla scadenza del timestamp piu' vecchio o fino a una modifica
della configurazione.

`MAX` e' un limite globale: viene condiviso da tutte le syscall e da tutte le
identita' registrate. Una chiamata viene selezionata quando la syscall e'
registrata e corrisponde almeno il path dell'eseguibile oppure l'EUID.

Durante una riconfigurazione viene incrementata una generazione. In questo
modo i processi gia' in attesa capiscono che devono rileggere lo stato invece di
continuare ad aspettare usando valori vecchi.

Per lo scaricamento del modulo vengono prima rimosse le nuove deviazioni, poi
vengono svegliati i processi in attesa e infine si aspetta che tutti i wrapper
gia' entrati siano terminati. Una syscall originale che si blocca naturalmente,
come `read`, deve comunque terminare prima che l'unload possa completarsi.

## Compilazione

Servono compilatore e header corrispondenti al kernel in esecuzione:

```sh
sudo apt update
sudo apt install build-essential linux-headers-$(uname -r)
```

Dalla directory principale:

```sh
./scripts/check-environment.sh
make
```

Caricamento del modulo:

```sh
sudo insmod kernel/syscall_throttle.ko
```

## Configurazione

Le modifiche alla configurazione richiedono EUID 0. La lettura dello stato e
delle statistiche e' invece disponibile anche agli utenti normali.

Esempio con `getpid`, limite globale di cinque chiamate al secondo e selezione
del path dell'eseguibile `test_throttle`:

```sh
sudo ./user/throttle_ctl configure --clear \
    --program "$(pwd)/tests/test_throttle" \
    --syscall getpid \
    --max 5 \
    --reset-stats \
    --enable
```

Per controllare configurazione e statistiche:

```sh
./user/throttle_ctl show
```

I comandi singoli (`add-program`, `add-uid`, `add-syscall`, `set-max`,
`enable` e `disable`) restano disponibili. L'elenco completo si ottiene con:

```sh
./user/throttle_ctl --help
```

Prima di rimuovere il modulo conviene disabilitare il monitor:

```sh
sudo ./user/throttle_ctl disable
sudo rmmod syscall_throttle
```

## Test

La demo piu' breve configura il modulo, esegue sei `getpid` con `MAX=5` e
mostra il ritardo e le statistiche:

```sh
sudo ./scripts/demo.sh
```

Dopo la compilazione si puo' eseguire l'intera suite con:

```sh
sudo ./scripts/test-all.sh
```

I test includono configurazioni non valide, permessi, registri concorrenti,
variazioni di `MAX` mentre il monitor e' attivo, segnali, statistiche, carico
concorrente e rimozione del modulo. In `tests/syscalls/` ci sono inoltre piccoli
programmi separati per provare syscall bloccanti e non bloccanti.

## Limiti e scelte fatte

- i path configurati devono essere assoluti, esistere e contenere al massimo
  255 caratteri; il registro contiene al massimo 64 eseguibili;
- la finestra temporale e' mobile e usa il clock monotono;
- non e' garantito un ordine FIFO tra i processi in attesa;
- un segnale puo' interrompere l'attesa;
- `exit`, `exit_group` e `rt_sigreturn` non vengono accettate, perche' non
  ritornano normalmente al wrapper;
- registri e limite hanno dimensione massima fissa per mantenere limitato il
  lavoro svolto dal pre-handler.

## Struttura

```text
kernel/   sorgenti del modulo
user/     utility throttle_ctl
include/  interfaccia ioctl condivisa
tests/    programmi usati dai test
scripts/  demo e test di integrazione
docs/     architettura, prestazioni e scelte di progetto
```

Per una panoramica dei componenti e dei flussi interni, vedere
[Architettura](docs/ARCHITETTURA.md). La metodologia per misurare l'overhead e'
descritta in [Prestazioni](docs/PRESTAZIONI.md).

Il codice e' distribuito secondo la licenza GPL-2.0-only.
