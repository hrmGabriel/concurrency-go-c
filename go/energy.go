package main

import (
	"os"
	"strconv"
	"strings"
)

// raplPath é o contador de energia da CPU exposto pelo kernel (Linux, Intel e AMD).
const raplPath = "/sys/class/powercap/intel-rapl:0/energy_uj"
const raplRangePath = "/sys/class/powercap/intel-rapl:0/max_energy_range_uj"

// PowerModel estima a energia quando o RAPL não está disponível (ex.: Windows
// ou sem permissão de leitura): parte fixa (CPU ociosa) + parte por CPU ativa.
type PowerModel struct {
	IdleWatts float64 // CPU ligada e ociosa
	MaxWatts  float64 // todos os núcleos em carga (TDP)
}

// raplJoules lê o contador RAPL em joules; ok é false se estiver indisponível.
func raplJoules() (float64, bool) {
	return readJoules(raplPath)
}

func readJoules(path string) (float64, bool) {
	data, err := os.ReadFile(path)
	if err != nil {
		return 0, false
	}
	uj, err := strconv.ParseUint(strings.TrimSpace(string(data)), 10, 64)
	if err != nil {
		return 0, false
	}
	return float64(uj) / 1e6, true
}

// Trata uma volta do contador, sem converter falhas de leitura em energia.
func raplDelta(j0, j1, counterRange float64) (float64, bool) {
	if j0 < 0 || j1 < 0 {
		return 0, false
	}
	if j1 >= j0 {
		return j1 - j0, true
	}
	if counterRange <= j0 || counterRange <= j1 {
		return 0, false
	}
	return j1 - j0 + counterRange, true
}

// raplAvailable informa se o contador RAPL pode ser lido.
func raplAvailable() bool {
	_, ok := raplJoules()
	return ok
}

// joules estima a energia gasta em seconds com workers ativos.
func (m PowerModel) joules(seconds float64, workers, numCPU int) float64 {
	active := min(workers, numCPU)
	wattsPerCPU := (m.MaxWatts - m.IdleWatts) / float64(numCPU)
	return (m.IdleWatts + wattsPerCPU*float64(active)) * seconds
}

// greenup calcula G = E_seq / E_p.
func greenup(seqJoules, parJoules float64) float64 {
	if parJoules <= 0 {
		return 0
	}
	return seqJoules / parJoules
}

// edp calcula o energy-delay product (J*s).
func edp(joules, seconds float64) float64 {
	return joules * seconds
}
