# primos em c e go

contagem de primos com openmp e goroutines.
tem versão sequencial, paralela e crivo segmentado.

pra compilar no linux, precisa de gcc com openmp e go:

```bash
make
```

- `c/` tem a implementação em c
- `go/` tem a implementação em go
- `results/` tem os resultados dos experimentos

a medição de energia usa rapl e precisa de permissão de leitura.
