# Metodología de benchmarks

> **Resultados disponibles:** todavía no hay mediciones en el dispositivo. LURVETA no ha podido ejecutarse en un Dimensity 8350 desde el entorno de desarrollo, así que **no se declara ninguna mejora de FPS** hasta tener datos de las pasadas descritas aquí. Las verificaciones hechas (corrección y número de submits/esperas del decodificador de texturas) son pruebas en host y se describen en `CHANGELOG.md`.

## Comparación A/B
- **A** = Winlator 11.2 oficial (`com.winlator`).
- **B** = LURVETA Performance (`com.lurveta.co`).

Las dos se instalan a la vez.

Condiciones idénticas:
- mismo dispositivo y versión de Android;
- mismo juego y versión;
- misma resolución y ajustes gráficos del juego;
- mismo DXVK, preset de Box64 y driver (Vortek).

Partida de inicio térmico controlado:
- pantalla apagada hasta que la temperatura de batería baje de unos 32 °C;
- brillo fijo, modo avión, sin cargador;
- sin apps en segundo plano.

## Medición
- **Mismo método para A y B**, en el PC: `tools/bench/sf_capture.py` lee los tiempos de presentación de SurfaceFlinger por adb.
  ```bash
  python3 tools/bench/sf_capture.py --list
  python3 tools/bench/sf_capture.py --layer "<capa del juego>" --seconds 120 --out A_trafico_1.csv
  ```
- **Detalle extra en B:** menú del juego › *Benchmark (start/stop)*. Genera:
  - `frames.csv`: un fotograma por fila;
  - `sensors.csv`, una vez por segundo: temperaturas, estado y margen térmico, RAM del sistema, CPU (`proc_cpu_pct`, 100 = un núcleo) y memoria (`proc_rss_mb`) de todos los procesos de la app (app + Wine + Box64), proceso que más CPU usa, carga y frecuencia de GPU si Android deja leerlas (si no, vacías) y frecuencia de cada núcleo;
  - `events.csv`: paradas de más de 1 s, congelamientos (5 s sin fotogramas, con su duración) y errores fatales de Vortek (`files/logs/vortek-errors.log`);
  - `summary.txt`: estadísticas, tirones (> 100 ms), **tiempo hasta el fallo** (≥ 10 s sin fotogramas o error fatal de Vortek), CPU media y memoria máxima de los procesos y temperaturas máximas.
  - Un congelamiento también se registra si el juego está en pausa o en segundo plano durante la grabación.
- **Análisis:**
  ```bash
  python3 tools/bench/analyze.py --a A_*.csv --b B_*.csv
  ```

## Escenas (GTA V u otro AAA equivalente)
1. Calle con tráfico (conducción fija de 120 s).
2. Centro urbano (a pie, mismo recorrido).
3. Vegetación (Paleto Bay / Monte Chiliad).
4. Campo abierto (autopista).
5. Interior.
6. Zona con muchos NPC.

Por escena: **3 pasadas de 120 s** con A y otras 3 con B, alternando el orden, más **una pasada sostenida de 20 minutos** por app para ver el throttling.

## Definiciones (iguales en la app y en `analyze.py`)
- **FPS medio** = fotogramas / tiempo total.
- **1 % low / 0,1 % low** = 1000 / media del 1 % / 0,1 % de frametimes más lentos ("low average").
- **Percentiles** p50/p95/p99 por rango más cercano; desviación típica del frametime.
- **Estabilidad:** tirones por minuto (fotogramas > 100 ms), paradas (> 1 s) y tiempo congelado (huecos ≥ 5 s). `analyze.py` los calcula también para las capturas de SurfaceFlinger, así que valen para Winlator 11.2.
- **Criterio:** una diferencia solo es significativa si |media_B − media_A| supera la suma de las desviaciones entre pasadas. Un pico aislado no cuenta.

## Plantilla de resultados (rellenar con datos reales)
| Escena | App | FPS medio (± entre pasadas) | 1 % low | 0,1 % low | p99 ms | T. máx batería | Estado térmico máx |
|---|---|---|---|---|---|---|---|
| Tráfico | A | | | | | | |
| Tráfico | B | | | | | | |
