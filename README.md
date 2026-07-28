# Syscall throttling

Progetto per il corso di Sistemi Operativi Avanzati.

L'obiettivo e' realizzare un modulo Linux che intercetti alcune system call e
ne limiti il numero di esecuzioni al secondo. La configurazione dovra' essere
modificabile da user space tramite un device driver.

Al momento sono presenti il device, la configurazione minima di `MAX` e stato
del monitor, l'utility user space e un primo test del device. Mancano ancora i
registri e l'intercettazione vera e propria.

La prima prova di intercettazione usa una kprobe sul dispatcher
`x64_sys_call`. Per ora il wrapper richiama sempre la funzione originale e non
applica ancora nessun limite.

## Ambiente

Lo sviluppo viene fatto su una VM Ubuntu 24.04 x86-64. Prima di compilare il
modulo bisogna controllare che gli header corrispondano al kernel restituito da
`uname -r`.

## Compilazione

```sh
./scripts/check-environment.sh
make
```

## Prima prova

```sh
sudo insmod kernel/syscall_throttle.ko
./user/throttle_ctl status
sudo ./user/throttle_ctl set-max 10
sudo ./user/throttle_ctl enable
sudo ./user/throttle_ctl disable
sudo rmmod syscall_throttle
```
