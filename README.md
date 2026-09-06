# cpufreq-watch ⚡

Monitor de frecuencias de escalado de CPU en Linux en tiempo real.

Lee datos directamente de `/sys/devices/system/cpu/cpu*/cpufreq/` para mostrar
la frecuencia actual, base, máxima, gobernador y estado de turbo boost por cada núcleo.

## Features

- **Frecuencia por núcleo**: muestra MHz actual vs base vs máximo
- **Turbo boost detection**: identifica núcleos con turbo activo
- **Gobernador**: muestra el governor activo (performance, powersave, schedutil...)
- **Utilización promedio**: porcentaje agregado con barra visual
- **Watch mode**: refresco periódico con pantalla limpia
- **JSON output**: para integración con pipelines
- **Demo mode**: datos simulados para entornos sin sysfs
- **Selftest**: verificaciones automáticas internas

## Build

```bash
g++ -std=c++23 -O2 -Wall -Wextra src/main.cpp -o cpufreq-watch
```

## Usage

```bash
# Snapshot único
./cpufreq-watch

# Modo watch (refresco cada 3s)
./cpufreq-watch --watch 3

# Salida JSON
./cpufreq-watch --json

# Demo (sin necesidad de sysfs)
./cpufreq-watch --demo

# Sin colores
./cpufreq-watch --no-color

# Auto-tests
./cpufreq-watch --selftest
```

## Data Source

- `/sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq` — frecuencia actual (kHz)
- `/sys/devices/system/cpu/cpu*/cpufreq/scaling_min_freq` — frecuencia mínima
- `/sys/devices/system/cpu/cpu*/cpufreq/scaling_max_freq` — frecuencia máxima
- `/sys/devices/system/cpu/cpu*/cpufreq/cpuinfo_max_freq` — frecuencia base/fábrica
- `/sys/devices/system/cpu/cpu*/cpufreq/scaling_governor` — gobernador activo
- `/sys/devices/system/cpu/cpu*/online` — estado online/offline

## Example Output

```
╔══════════════════════════════════════════════════════════╗
║        ⚡ CPU Frequency Scaling Monitor                 ║
╚══════════════════════════════════════════════════════════╝

  ── Aggregate ──
  CPUs online:  8 of 8
  Total freq:   22.10 GHz / 28.80 GHz
  Utilization:   76.7% ██████████████......
  Turbo active: 4 cores

  ── Per-Core ──
  CPU     Current      Base        Max  Governor          Load
  ──────────────────────────────────────────────────────────────
  cpu0     3800 MHz   4700 MHz   5000 MHz  performance  ██████████....
  cpu1     2400 MHz   4700 MHz   5000 MHz  powersave    █████.........
  ...
```

## License

MIT
