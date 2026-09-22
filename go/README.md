# Contagem paralela de números primos em Go e C

Trabalho de **Programação Concorrente e Distribuída (PCD)** sobre a contagem de números primos em um intervalo. A implementação original usa Go e goroutines; a próxima implementação será feita em C com OpenMP, mantendo o mesmo algoritmo para permitir uma comparação justa.

## Objetivo

Avaliar o impacto da concorrência no desempenho da contagem de primos em Go, variando o número de goroutines.

### Pergunta de pesquisa

Como a variação do número de goroutines influencia o tempo de execução, o speedup e a eficiência da contagem de números primos em Go?

### Hipótese

Espera-se que o aumento do número de goroutines reduza o tempo de execução e aumente o speedup até determinado ponto. Após ultrapassar a capacidade de processamento paralelo do hardware, os ganhos diminuem devido ao overhead de gerenciamento e à disputa pelos recursos computacionais.

## Decisões de projeto

| Decisão | Escolha | Motivo |
|---------|---------|--------|
| Teste de primalidade | Divisão trial otimizada (até √n, pulando pares) | CPU-bound, simples e fácil de paralelizar — bom para medir impacto de goroutines |
| Partição do trabalho | Estática: intervalo `[1, N]` dividido em `W` faixas contíguas | Baixo overhead, sem fila dinâmica; equilíbrio entre eficiência e simplicidade |
| Sincronização | `sync.WaitGroup` + slice de contagens (um índice por goroutine) | Sem contenção de mutex/channel no caminho crítico; soma final na main |
| Interface | CLI com flags | Facilita experimentos reprodutíveis (`seq`, `par`, `bench`) |

Não foi usado o Crivo de Eratóstenes: embora seja assintoticamente mais rápido, a paralelização é mais complexa e o foco do trabalho é o impacto das goroutines, não o algoritmo de primalidade em si.

## Estrutura do projeto

```text
T1/
  README.md          # documentação geral do trabalho
  go/                # implementação existente com goroutines
    go.mod
    main.go          # CLI e orquestração do experimento
    metrics.go       # timing, speedup, eficiência e CSV
    primes.go        # isPrime, countPrimesSeq e countPrimesPar
    results.csv      # resultados do benchmark em Go
  c/                 # futura implementação em C com OpenMP
    README.md
```

### Fluxo da versão concorrente

1. O intervalo `[1, N]` é particionado em até `W` segmentos de tamanho aproximadamente igual.
2. Cada goroutine conta os primos na sua faixa e grava o resultado em `counts[idx]`.
3. A goroutine principal aguarda com `WaitGroup` e soma os resultados.

## Como compilar e executar

Pré-requisito: [Go](https://go.dev/dl/) 1.21 ou superior instalado e no `PATH`.

```bash
# Entrar na implementação Go
cd go

# Compilar
go build -o primes .

# Sequencial
go run . -mode seq -n 5000000 -runs 3

# Paralelo com 8 goroutines
go run . -mode par -n 5000000 -workers 8 -runs 3

# Benchmark completo (validação + grade de workers + CSV)
go run . -mode bench -n 5000000 -runs 3 -csv results.csv
```

### Flags

| Flag | Padrão | Descrição |
|------|--------|-----------|
| `-n` | `5000000` | Limite superior do intervalo `[1, n]` |
| `-workers` | `NumCPU` | Número de goroutines (modo `par`) |
| `-mode` | `bench` | `seq`, `par` ou `bench` |
| `-runs` | `3` | Repetições para média de tempo |
| `-csv` | `results.csv` | Arquivo de saída do modo `bench` |

No modo `bench`, a grade de workers é `{1, 2, 4, 8, NumCPU, 2×NumCPU}` (valores duplicados são removidos). Antes das medições, a corretude é validada: o total sequencial deve coincidir com o paralelo para vários valores de `W`.

## Métricas

| Métrica | Fórmula / método |
|---------|------------------|
| Tempo de execução (`T`) | Média de `time.Since` sobre `-runs` execuções (sem I/O no caminho crítico) |
| Speedup (`S`) | `S = T₁ / Tₚ` — tempo sequencial dividido pelo tempo paralelo |
| Eficiência paralela (`E`) | `E = S / W` — speedup normalizado pelo número de workers |
| Escalabilidade | Comportamento de `S` e `E` em função de `W` (tabela/CSV) |
| Utilização de CPU | Observação no Gerenciador de Tarefas durante o `bench`, cruzada com `runtime.NumCPU()` e `GOMAXPROCS` |

## Ambiente experimental

| Item | Valor |
|------|-------|
| SO | Windows 11 (build 26200) |
| CPU | AMD Ryzen 5 7600X (6 núcleos físicos / 12 threads lógicos) |
| `runtime.NumCPU()` / `GOMAXPROCS` | 12 |
| Go | go1.27.0 windows/amd64 |
| Intervalo | `[1, 5_000_000]` |
| Repetições | 3 (média) |
| Primos encontrados | 348 513 |

## Resultados

Baseline sequencial médio: **454,04 ms**.

| Workers | Tempo médio | Speedup | Eficiência |
|---------|-------------|---------|------------|
| 1 | 450,81 ms | 1,01 | 1,01 |
| 2 | 334,69 ms | 1,36 | 0,68 |
| 4 | 192,30 ms | 2,36 | 0,59 |
| 8 | 114,87 ms | 3,95 | 0,49 |
| 12 | 92,82 ms | 4,89 | 0,41 |
| 24 | 88,69 ms | 5,12 | 0,21 |

Os mesmos dados estão em [`go/results.csv`](go/results.csv).

### Interpretação vs. hipótese

Os resultados **confirmam a hipótese**:

1. **Redução de tempo e aumento de speedup** conforme `W` cresce de 1 até 12 (número de threads lógicas): o tempo cai de ~451 ms para ~93 ms e o speedup chega a ~4,89.
2. **Ganhos marginais além da capacidade do hardware**: com 24 workers (2×`NumCPU`), o speedup sobe apenas para ~5,12 — melhoria pequena em relação a 12 workers — enquanto a eficiência cai para ~0,21.
3. **Eficiência decrescente**: típico de paralelismo com overhead de scheduling e desbalanceamento leve (números maiores na parte final do intervalo exigem mais trabalho por candidato na divisão trial).
4. **Utilização de CPU**: durante o `bench`, a carga sobe de forma consistente com o número de workers até saturar os 12 threads lógicos; workers extras passam a competir pelos mesmos núcleos, o que explica o platô no speedup.

O speedup máximo (~5×) ficou abaixo do número ideal teórico de 12, o que é esperado: partição estática desbalanceada, custo de criação/sincronização de goroutines e o fato de o Ryzen 7600X ter 6 núcleos físicos (SMT ajuda, mas não dobra o desempenho).

## Limitações

- **Desbalanceamento estático**: faixas de mesmo tamanho em elementos não têm o mesmo custo — verificar primalidade de números próximos de `N` é mais caro que no início do intervalo.
- **Overhead de scheduling**: muitas goroutines além de `GOMAXPROCS` aumentam contenção sem trabalho útil adicional.
- **Algoritmo de primalidade**: divisão trial não é o método mais rápido; serve ao objetivo pedagógico de carga CPU-bound paralelizável.
- **Medição de CPU**: qualitativa via Gerenciador de Tarefas; não há profiler integrado (pprof) neste escopo.

## Reimplementação em outra linguagem (ex.: C)

Para comparar Go com uma linguagem mais tradicionalmente ligada a paralelismo (C com **pthreads** ou **OpenMP**), o algoritmo deve ser o **mesmo**; só muda o mecanismo de paralelismo. Assim, diferenças de tempo/speedup refletem runtime e modelo de threads, não outra estratégia de contagem.

### O que manter idêntico

| Aspecto | Especificação (igual à versão Go) |
|---------|-----------------------------------|
| Problema | Contar primos no intervalo fechado `[1, N]` |
| `is_prime(n)` | `n < 2` → falso; `2` é primo; pares > 2 → falso; loop ímpar de `3` até `√n` |
| Sequencial | Um único fluxo percorrendo `[1, N]` |
| Paralelo | Partição **estática** em `W` faixas contíguas de tamanho ~igual (`base = total/W`, `extra = total%W`) |
| Agregação | Cada worker escreve só em `counts[i]`; a main soma no final (sem mutex no caminho crítico) |
| Validação | Total sequencial == total paralelo para vários `W` |
| Experimento | Mesmo `N`, mesma grade de workers `{1, 2, 4, 8, nCPU, 2×nCPU}`, mesmas fórmulas de speedup/eficiência |
| Medição | Cronometrar **apenas** a contagem (sem I/O); média de várias corridas |

### Mapeamento Go → C

| Conceito neste projeto (Go) | Equivalente típico em C |
|------------------------------|-------------------------|
| Goroutine (`go func(...)`) | Thread POSIX (`pthread_create`) ou região paralela OpenMP |
| `sync.WaitGroup` | `pthread_join` em todas as threads, ou barreira implícita no fim de `#pragma omp parallel` |
| Slice `counts[workers]` | Array `long counts[W]` (ou `malloc`); cada índice é exclusivo de um worker |
| `runtime.NumCPU()` | `sysconf(_SC_NPROCESSORS_ONLN)` (Linux/macOS) ou `GetSystemInfo` (Windows) |
| `time.Since` | `clock_gettime(CLOCK_MONOTONIC, ...)` ou `omp_get_wtime()` |

### Passos de implementação

1. **`is_prime`** — copiar a lógica de `primes.go` (divisão trial otimizada). Não trocar por crivo se o objetivo é comparar paralelismo.
2. **`count_primes_seq(start, end)`** — loop sequencial; baseline `T₁`.
3. **`count_primes_par(start, end, workers)`**:
   - Se `workers <= 1`, chamar a versão sequencial.
   - Calcular `total`, `base`, `extra` e os pares `(seg_start, seg_end)` como em `countPrimesPar`.
   - Lançar `W` workers; o worker `i` conta em `[seg_start, seg_end]` e grava em `counts[i]`.
   - Aguardar todos; somar `counts`.
4. **CLI / harness** — mesmos modos úteis (`seq`, `par`, `bench`), mesmas métricas e, de preferência, CSV no mesmo formato (`workers,tempo_ms,primos,speedup,eficiencia`).
5. **Compilação sugerida (C)**:
   - pthreads: `gcc -O2 -pthread -o primes_c primes.c -lm`
   - OpenMP: `gcc -O2 -fopenmp -o primes_c primes.c -lm`

### Esboço com pthreads (C)

```c
typedef struct {
    long start, end;
    long *slot; /* &counts[i] */
} Task;

void *worker(void *arg) {
    Task *t = arg;
    long c = 0;
    for (long n = t->start; n <= t->end; n++)
        if (is_prime(n)) c++;
    *(t->slot) = c;
    return NULL;
}

/* Para cada worker i: preencher tasks[i], pthread_create(...), depois pthread_join em todos. */
```

Com **OpenMP**, o mesmo padrão fica mais curto: calcular as faixas e, dentro de `#pragma omp parallel num_threads(W)`, cada thread (`omp_get_thread_num()`) processa a sua faixa e escreve em `counts[tid]`.

### Boas práticas para a comparação ser justa

- Usar o **mesmo `N`** e a **mesma máquina** (e, se possível, fechar outros programas pesados).
- Compilar C com otimização (`-O2` ou `-O3`); em Go, medir com binário `go build` (não só `go run` em demos informais — o harness já usa o código compilado no fluxo normal).
- Não misturar algoritmos (ex.: crivo em C e trial division em Go).
- Reportar também o modelo usado (pthreads vs OpenMP) e quantos núcleos físicos/lógicos o SO enxerga.
- Esperado na análise: C costuma ter menos overhead por thread que goroutines em cargas muito finas; com `N` grande (10⁶–10⁷), ambos devem escalar até ~número de cores, depois estabilizar — o interessante é **onde** o speedup satura e qual eficiência cada runtime entrega.

## Possíveis extensões

- Partição dinâmica (worker pool + canal de subtarefas) para reduzir desbalanceamento.
- Crivo paralelo segmentado para intervalos maiores.
- Coleta automática de métricas de CPU via pprof ou contadores do SO.

## Licença

Uso acadêmico — PCD / T1.
