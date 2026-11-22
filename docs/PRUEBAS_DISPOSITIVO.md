# Pruebas en el teléfono (Dimensity 8350 / Mali‑G615)

Lo que solo se puede comprobar en el dispositivo, en orden. Anota cada resultado (✔ / ✘ y una línea).

## 0. Instalación
- APK: `app-release.apk` (no depurable, recomendado) o `app-debug.apk`. Mismo paquete `com.lurveta.co` y misma firma entre sí.
- Si Android dice que la firma no coincide con la versión instalada, la instalada se compiló con otra clave: hay que desinstalarla (se pierden los contenedores) o probar en otro perfil de usuario.
- Los contenedores y ajustes existentes se conservan al actualizar con la misma firma.

## 1. Arranque y ajustes nuevos (5 min)
1. Abrir un contenedor › Gráficos › Vortek › Configuración. Deben aparecer:
   - **Subidas de texturas BC**: *En línea (recomendado)* por defecto;
   - **Caché de pipelines persistente**: activada.
2. Arrancar un juego DX9/DX11 conocido. En el log (`adb logcat -s System.out` o el visor de logs) debe verse:
   - `vortek: texture decoder active (no native BC): upload mode inline`
   - `vortek: persistent pipeline cache .../pipeline-13b5-....bin (0 KB loaded)` la primera vez, y en la segunda partida un tamaño mayor que 0.

## 2. Corrección de texturas (GTA IV y un juego DX11)
Mismo punto del juego, captura de pantalla en cada modo (Vortek › Subidas de texturas BC):
| Modo | Texturas correctas | Parpadeos / negro | Congelamientos |
|---|---|---|---|
| En línea | | | |
| Por lotes + barreras | | | |
| Por lotes (LURVETA 1.0) | | | |
| Síncrono (Winlator 11.2) | | | |

Lo esperado según las pruebas de host: *En línea* correcto siempre; los otros tres pueden fallar cuando el juego reutiliza command buffers.

## 3. Rendimiento (A/B con Winlator 11.2)
Seguir `docs/BENCHMARKING.md`: 3 pasadas de 120 s por escena y app, capturadas con `tools/bench/sf_capture.py` (mismo método para las dos apps), y `tools/bench/analyze.py --a <Winlator> --b <LURVETA>`.
Comparar también, solo en LURVETA, *En línea* contra *Síncrono* en la misma escena (aísla el efecto de las subidas BC).

## 4. Caché de pipelines
1. Borrar `/data/data/com.lurveta.co/cache/vortek/pipeline-*.bin` (o desactivar y volver a activar la opción).
2. Partida 1: recorrer una zona nueva 2 min con el benchmark grabando.
3. Partida 2: mismo recorrido.
Esperado: menos tirones (`hitches`/`stalls` en `summary.txt`) en la partida 2. Si un juego empieza a fallar al compilar shaders, desactivar la opción y anotarlo.

## 5. Congelamientos (DEVICE_LOST)
Si un juego se cuelga:
- Antes: Wine se quedaba esperando para siempre.
- Ahora: el juego debe recibir un error y cerrarse o mostrarlo, y `files/logs/vortek-errors.log` debe tener una línea con la hora y la llamada (`vkQueueSubmit`, `vkWaitForFences`…).
- Con el benchmark grabando: `events.csv` (freeze_start / vortek_fatal) y `time_to_failure_s` en `summary.txt`.

## 6. BC6H / BC7
Juegos DX11 que los usan (cielos HDR, texturas de alta calidad). Antes: la creación de la imagen fallaba. Ahora deben verse bien. Anotar el juego y la escena.

## 7. Opcionales
- `release` frente a `debug` en la misma escena (efecto de no ser depurable en las partes Java, como el servidor X).
- Preset personalizado de Box64 con `BOX64_DYNACACHE=1`: la segunda partida debería cargar más rápido. Si algo falla, volver a 0.
- Afinidad de CPU del contenedor sin los núcleos A510 (núcleos 0–3 en la mayoría de los Dimensity 8350): comparar 1 % low.

## Qué enviar
Las carpetas `Download/LURVETA/benchmarks/*`, `files/logs/vortek-errors.log` si existe, capturas del punto 2 y la tabla de resultados.
