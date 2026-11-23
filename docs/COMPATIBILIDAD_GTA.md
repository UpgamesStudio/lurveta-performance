# Compatibilidad: GTA IV y GTA V

> **Estado a fecha de esta rama (`1.1.1-dev`): ningún resultado en dispositivo.**
> Todo lo marcado «Probado» se probó en el PC de desarrollo (lavapipe, no Mali). Los juegos no se han podido ejecutar desde el entorno de desarrollo: no hay teléfono ni copia de los juegos. Nada de esta página debe leerse como «funciona».

## Estado por juego

| | GTA IV | GTA V |
|---|---|---|
| Arranque | Sin datos de esta rama. Informado por el usuario: arrancó en una versión anterior y dejó de arrancar en una posterior | Sin datos. Informado: no arrancó; luego «ambos dejaron de iniciar» |
| Carga hasta el juego | Sin datos | Sin datos |
| Ejecución | Informado: rendimiento muy bajo, vehículos y peatones que desaparecen, cámara/controles, congelamientos de imagen con audio | Sin datos |
| Problemas gráficos | Los de arriba (sin causa identificada) | Sin datos |
| Estabilidad | Sin datos | Sin datos |
| Rendimiento | Sin mediciones | Sin mediciones |

## GTA V, segundo informe (tras la corrección de fences): análisis

Comparación con el informe anterior (misma instalación «GTA V LITE V13.2 OptiJuegos», mismo contenedor):

| | Antes | Después |
|---|---|---|
| `Failed to sync fence: VK_ERROR_DEVICE_LOST` en el log de DXVK | sí | **no** |
| Llamadas / envíos / presentaciones de `GTA5.exe` | 389 / 2 / 0 | 389 / 2 / 0 |
| Última llamada Vulkan de `GTA5.exe` | `vkDestroyInstance` | `vkDestroyInstance` (~T+31 s) |
| `GTA5.exe` cuando terminó la sesión | (no medido) | **vivo**, 13–24 % de CPU, sin actividad Vulkan |
| Fin | el proceso principal de Wine (escritorio) sale con código 1 ~10 s después | lo mismo, ~12 s después; `wfm.exe` y `explorer.exe` terminan en el mismo instante |

**Hechos:**
- El error del fence ya no aparece, lo que es coherente con que la corrección se ejecutó, pero no lo demuestra.
- El código 1 es del proceso principal de Wine (`explorer /desktop` + gestor de archivos `wfm.exe`), **no de GTA V**, que seguía en ejecución.
- `GTA5.exe` inicializa DXVK (varias instancias y dispositivos), destruye la instancia sin crear ningún swapchain y se queda en segundo plano.

**Qué faltaba para ir más lejos:**
- La secuencia de llamadas Vulkan de GTA V con sus resultados.
- Las líneas de error de Wine, que estaban **ocultas** bajo miles de avisos `glVertex…NV not found` generados por `BOX64_LOG=1` (activado por la propia captura).
- Un identificador inequívoco de la compilación.

**Añadido en 1.1.1‑dev6:**
- Diario de llamadas Vulkan por cliente (`vortek-journal-<pid>.txt`): cada llamada con su resultado real, en orden y con tiempo; el primer error real queda marcado (`FIRST ERROR`).
- Extracción de **todas** las líneas de error de la salida completa de Box64/Wine, con el ruido filtrado y contado.
- Procesos del juego vivos al terminar la sesión.
- Commit y hora de compilación en cada informe.

**Hipótesis abiertas (sin confirmar):**
1. GTA V descarta el dispositivo tras comprobar capacidades. Por ejemplo, el feature level 11_0 está bien, pero puede que falte algo que este repack exige; el diario lo dirá.
2. El cierre de la sesión lo provoca `wfm.exe` o el lanzador `PlayGTAV.exe`, no GTA V.
3. Algo específico del repack (lanzador, archivos modificados).

Los mensajes de GStreamer/GTK y PolygonOffset (este último viene de wined3d en el escritorio, no del juego, que usa DXVK) no tienen relación demostrada con el cierre.

## GTA V en el Dimensity 8350: falso «device lost» al esperar un fence (corrección en 1.1.1‑dev5, sin validar en el teléfono)

**Informe:** «GTA V LITE V13.2 OptiJuegos». `PlayGTAV.exe` → `GTA5.exe`; DXVK 1.10.3 crea instancia y dispositivo en la Mali‑G615. Tras **solo 2 envíos a la GPU** (`submits=2 images=3 presents=0`), DXVK registra `DxvkSubmissionQueue: Failed to sync fence: VK_ERROR_DEVICE_LOST` y el juego sale con código 1. Vortek: 0 llamadas fallidas, 0 errores fatales.

**Causa identificada (código):**
- `vkWaitForFences` con espera exporta cada fence como SYNC_FD y el cliente hace `poll()`. Según la especificación de Vulkan, esa exportación **resetea el fence**.
- Si DXVK vuelve a esperar o consulta ese fence, el driver lo ve sin señal y sin trabajo pendiente, y la exportación falla.
- El cliente de Vortek (`libvulkan_vortek.so`, desensamblado) devuelve `-4 = VK_ERROR_DEVICE_LOST` ante **cualquier** error de ese camino, o si no recibe ningún descriptor. Es decir, era un «device lost» sin pérdida real de la GPU, y eso encaja con los 0 errores de Vortek.
- Defecto adicional: con un fence ya señalado, el driver puede devolver `-1`. El kernel rechaza `-1` en `SCM_RIGHTS` (`EBADF`), así que no se enviaba nada y el cliente quedaba colgado. Además, en caso de error se cerraban descriptores sin inicializar.

**Corrección (`fence_export.c`):**
- Un fence ya señalado no se exporta: el cliente recibe un `eventfd` que `poll()` ve listo al instante.
- Un fence pendiente se exporta **una vez**, y se guarda su sync fd como estado real para esperas y consultas posteriores.
- Al resetear, reenviar, adquirir o destruir el fence, se olvida esa copia.
- Nunca se envía un descriptor inválido.
- Cada fallo restante de este camino queda registrado (`FAIL vkWaitForFences(export sync fd)` con el error del driver).

**Prueba reproducible (host):** con un driver simulado que sigue la especificación, el algoritmo original produce `VK_ERROR_DEVICE_LOST` en la segunda espera; el nuevo no. **No está verificado en Mali** que su driver se comporte exactamente así: si GTA V sigue fallando, el nuevo informe mostrará el error real del driver en esa línea `FAIL`.

**Errores de GStreamer/GTK en el mismo informe:** `libgtk-3.so.0` no se encuentra al cargar `libgstreamer` (winegstreamer, reproducción multimedia de Wine). Box64 sigue adelante («Using native libgstaudio…»). No hay evidencia de que intervenga en el cierre: el error fatal es el del fence. Se considera secundario hasta que un informe diga lo contrario.

## Evidencia real del Dimensity 8350 (informe del 22‑11‑2026, 1.1.1‑dev, release)

Primera prueba con captura de arranque. Juego: «GTA IV LITE V3 By OptiJuegos», lanzado desde el gestor de archivos de Wine. Contenedor: Vortek + DXVK 1.10.3, preset de Box64 PERFORMANCE, 1280×720, audio ALSA.

| Hecho medido | Valor |
|---|---|
| Entorno (Box64, Wine, escritorio) | Arranca en ~1 s |
| `GTAIV.exe` creado | T+14 s |
| DXVK inicializado (instancia y dispositivo Vulkan en Mali‑G615) | T+17 s. Heap anunciado: 7615 MiB |
| Primer fotograma | T+27 s |
| Presentaciones por segundo (estimadas con los contadores de Vortek) | ~180–250/s entre T+28 y T+39 s; media ~206/s entre T+39 y T+113 s; 56–181/s (media ~107/s) entre T+113 y T+133 s. **No se sabe qué pantalla se veía** (menú, carga o juego) |
| Carga de recursos | Imágenes 2141→4563 y memoria 752→1280 MB entre T+113 y T+135 s |
| Fallos de llamadas Vulkan / errores fatales | 0 / 0 |
| `gta4Browser.exe` | **~45–54 % de un núcleo de forma continua durante toda la sesión** (un solo hilo, 59 MB) |
| Después de T+135 s | La conexión Vulkan del juego deja de informar, pero `GTAIV.exe` no termina antes del final de la captura (172 s). Con la sonda anterior no se puede saber si el juego se cerró, se colgó o fuiste tú (ver «Defectos de la sonda») |

**Defectos de la sonda en ese informe, corregidos en 1.1.1‑dev4:** todas las conexiones Vulkan escribían el mismo archivo de actividad (por eso el informe alterna con un cliente inactivo de 42 peticiones). Además, al lanzar desde el escritorio no se sabía cuál era el ejecutable del juego y no se muestrearon los hilos de `GTAIV.exe`.

**Pendiente de aclarar para GTA IV:** qué FPS hay realmente *en juego* y si el corte a T+135 s es un cuelgue. Prueba A/B sin tocar archivos del juego: con el juego ya en marcha, cerrar `gta4Browser.exe` desde el administrador de tareas y comparar los FPS (benchmark del menú del juego).

**GTA V: todavía no hay ningún informe.** El archivo recibido como «GTA V» era de GTA IV.

## Qué se ha investigado y qué se sabe

**Probado (host, no Mali)**
- El decodificador BC (BC1–BC7) coincide con un decodificador independiente: 54/54 casos; 396/396 con tamaños de mip de 1×1 a 7×7 bajo AddressSanitizer. Un fallo de BC4 multihilo de la base original ya está corregido.
- El modo de subida «En línea» no produce errores de validación ni de sincronización en lavapipe; los modos anteriores sí.

**Defectos encontrados por lectura de código en 1.1.0 y corregidos en esta rama**
1. Cada cierre normal de sesión registraba un «FATAL» falso en `vortek-errors.log` (y el benchmark lo contaba como fallo). **No** puede impedir arrancar un juego.
2. La consulta de propiedades de imagen para BC era más estricta que en 1.0.0 (rechazaba usos distintos de muestreo/transferencia). Ahora responde el driver.

**Descartado con evidencia**
- `-DNDEBUG` del build `release` frente a `debug`: los únicos `assert` están en `virglrenderer` (ruta OpenGL), no en Vortek.
- `DEVICE_LOST` espurio por cambios de ventana: solo sale de un submit real del driver.

**Hipótesis sin confirmar (necesitan logs del teléfono)**
- **Memoria de GTA V:** el decodificador guarda las texturas BC descomprimidas (BC1 ×8, BC3 ×4, BC7 ×4, BC6H ×8, BC4/BC5 ×2) y Vortek limita las asignaciones a 2/3 del mayor heap (o al valor configurado). Si GTA V pide más, `vkAllocateMemory` falla con `OUT_OF_DEVICE_MEMORY`. Es una limitación de diseño, no un fallo puntual. Una solución real sería transcodificar BC a formatos nativos de Mali (ETC2/EAC/ASTC) sin ampliar el tamaño, pero requiere un transcodificador con pérdida y no está implementada.
- **BC6H/BC7 (1.1.0):** antes no se anunciaban; ahora sí. Puede arreglar o romper un juego según cómo reaccione DXVK. Hay un interruptor para aislarlo.
- **Caché de pipelines persistente (1.1.0):** puede influir en el arranque si el driver rechaza datos de caché. Tiene interruptor.
- **Wine/Box64/DXVK:** ningún cambio de 1.1.0 los toca, así que, si la regresión no viene de Vortek, tiene que venir de la configuración del contenedor o de la instalación (p. ej. contenedor nuevo, APK con otra firma, rootfs reinstalado). Solo se puede comprobar con el log.

## Diagnóstico que genera esta rama (sin coste cuando todo va bien)
- `files/logs/vortek-diagnostics.log`: una línea `INFO` al arrancar (dispositivo, driver, decodificador, modo de subida, caché, tope de memoria) y una línea `FAIL` por cada llamada Vulkan que falla (`vkAllocateMemory` con tamaños pedidos/usados/tope, imagen, buffer, vista, framebuffer, sampler, pipelines, dispositivo). Máximo 200 líneas por sesión.
- `files/logs/vortek-errors.log`: errores fatales (`VK_ERROR_DEVICE_LOST`).
- Con el benchmark activo: `events.csv` y `summary.txt` cuentan ambos.

## Referencia de configuración
**Todavía no existe.** No se publica una configuración «de referencia» hasta que una combinación demuestre arrancar, cargar y jugar en el Dimensity 8350. La guía para encontrarla es `docs/PRUEBAS_GTA.md`.
