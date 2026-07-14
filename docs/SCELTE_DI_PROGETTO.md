# Scelte di progetto

Questo file raccoglie i dubbi incontrati durante lo sviluppo e le motivazioni
delle soluzioni adottate.

## Questioni iniziali

- Capire se `MAX` deve essere globale oppure separato per ogni programma.
- Stabilire che cosa si intende esattamente per nome del programma.
- Scegliere un meccanismo di intercettazione compatibile con il kernel della VM.
- Evitare qualsiasi attesa attiva nel percorso delle syscall.

Le decisioni verranno aggiunte dopo aver verificato le alternative sulla VM.

