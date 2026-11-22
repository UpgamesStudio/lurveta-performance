# Registro de modificaciones — LURVETA Performance

Base: Winlator 11.2 (`winlator-app@a030f55`, tag `upstream-winlator-11.2`). Cada punto indica cómo se verificó.
- **[V‑host]**: probado en Linux con el código real (ver `tools/host-tests`).
- **[V‑apk]**: comprobado en el APK compilado (aapt/strings).
- **[Disp.]**: requiere medición en el dispositivo.

## 1.1.1 — en preparación (NO publicada; rama de trabajo, sin pruebas en dispositivo)

Objetivo: aislar y corregir la regresión de arranque de GTA IV/GTA V. **No se declara compatibilidad de ningún juego**; ver `docs/COMPATIBILIDAD_GTA.md`.

### Correcciones
- El cierre normal de una sesión ya no registra un «FATAL» falso en `vortek-errors.log` (y el benchmark deja de contarlo como fallo). **[V‑código]**
- `vkGetPhysicalDeviceImageFormatProperties` para BC vuelve a no ser más estricto que 1.0.0: responde el driver con el formato descomprimido. **[V‑código]**

### Captura de arranque (nuevo)
- Cada lanzamiento genera `Download/LURVETA/logs/launch-latest.txt`: etapas alcanzadas (Box64, Wine, explorer, proceso del juego, conexión/instancia/dispositivo Vulkan, primer fotograma), **código de salida o señal**, tiempo, veredicto y evidencia (procesos, entorno exacto, salida de Box64/Wine, log de DXVK, fallos Vulkan). Antes la salida iba a `/dev/null`, los errores de `exec` se ignoraban y la app volvía al menú sin avisar (comportamiento heredado de 11.2).
- Si el arranque termina de forma inesperada, el menú lo avisa con el veredicto.
- Solo cambia lo que se **imprime**: `DXVK_LOG_LEVEL=info` (si estaba en `none`), `WINEDEBUG=fixme-all` (si estaba en `-all`, conserva los `err:`) y `BOX64_LOG=1`; nunca sobrescribe valores puestos por el usuario. Se puede desactivar con la preferencia `lurveta_launch_trace`.
- **[V‑host]** la lógica del veredicto (cada punto de parada → diagnóstico correcto). **No probado en dispositivo** la captura en sí.
- `docs/DIFERENCIAS_CON_WINLATOR_11_2.md`: comparación del camino de arranque con 11.2 (verificado: rutas reubicadas sin restos).

### Corrección: falso `VK_ERROR_DEVICE_LOST` al esperar fences (GTA V)
- Causa: exportar un fence como SYNC_FD lo resetea (especificación de Vulkan). La siguiente espera o consulta del mismo fence fallaba, y el cliente de Vortek convierte cualquier fallo en `VK_ERROR_DEVICE_LOST`. GTA V lo registraba tras 2 envíos a la GPU, sin pérdida real del dispositivo.
- Ahora: cada fence se exporta como mucho una vez y su sync fd se conserva como estado real; los fences ya señalados no se exportan; nunca se envían descriptores inválidos (antes, un `-1` hacía fallar `sendmsg` y el cliente quedaba colgado); los fallos restantes se registran.
- **[V‑host]** driver simulado según la especificación: el algoritmo original reproduce el `DEVICE_LOST`, el nuevo no; el kernel rechaza `-1` en `SCM_RIGHTS`. **Pendiente de validar en el Dimensity 8350 con GTA V.**

### Captura: correcciones tras el primer informe real (GTA IV en el Dimensity 8350)
- Cada cliente Vulkan escribe su propio archivo de actividad (`vortek-activity-<pid>.txt`); antes compartían uno y el informe mezclaba el escritorio de Wine con el juego.
- FPS estimados por cliente a partir de las presentaciones (último tramo y mejor tramo).
- Juegos lanzados desde el escritorio: se detecta cualquier `.exe` que no sea de Wine y se muestrean sus hilos.
- Cada lanzamiento deja además `Download/LURVETA/logs/launch-<fecha>.txt`, que no se sobrescribe; el informe indica el proceso de juego detectado.
- **[V‑host]** ejecutado en Linux: dos clientes, FPS estimados, juego lanzado desde el escritorio.

### Sondas para pantallas negras (nuevo)
- Vortek escribe cada 2 s `files/logs/vortek-activity.txt` (desde un hilo vigía): peticiones, envíos, presentaciones, swapchains, imágenes, memoria y **la llamada Vulkan en curso y desde hace cuánto**; el informe marca `SERVER BLOCKED INSIDE THIS VULKAN CALL` si dura más de 5 s.
- El informe muestrea cada 5 s los hilos del proceso del juego y de sus procesos auxiliares (p. ej. `gta4Browser.exe`): CPU por hilo, estados y memoria, para distinguir bloqueo de ejecución.
- Se registran los fallos de `vkCreateSwapchainKHR` (Vortek rechaza un swapchain cuyo tamaño no coincide exactamente con la ventana).
- **[V‑host]** captura completa ejecutada en Linux contra un GTA IV simulado (proceso ocupado, auxiliar dormido, llamada Vulkan bloqueada). **No probada en dispositivo.**

### Diagnóstico (nuevo)
- `files/logs/vortek-diagnostics.log`: resumen del dispositivo al arrancar y una línea por cada llamada Vulkan que falla (memoria con tamaños y tope, imágenes, buffers, vistas, pipelines…). Límite de 200 líneas por sesión; sin coste si todo va bien. El benchmark cuenta estos fallos (`vulkan_call_failures`, `events.csv`).
- Interruptor Vortek › **Emular BC6H/BC7** (por defecto activado): desactivado reproduce el comportamiento de 1.0.0. Junto con el modo de subida BC y la caché de pipelines, permite aislar la regresión sin cambiar de APK (`docs/PRUEBAS_GTA.md`).

### Pruebas
- 396 casos de mips de 1×1 a 7×7 de los 11 formatos BC bajo AddressSanitizer: sin errores. **[V‑host]**

### Investigado y descartado
- `-DNDEBUG` del build release: solo afecta a `virglrenderer`, no a Vortek.

### Pendiente (requiere el teléfono)
Causa real del fallo de arranque; límite de memoria con texturas BC descomprimidas en GTA V; rendimiento de ambos juegos.

## 1.1.0 — rediseño visual + Vortek (release)

Incluye el rediseño visual (solo presentación, ver `docs/REDISENO_UI.md`) y las mejoras técnicas siguientes.

### Subidas de texturas BC (corrige P1b)
- **Modo "en línea" (nuevo, por defecto):** la copia decodificada se graba **dentro del command buffer del juego**, en la posición exacta de su `vkCmdCopyBufferToImage`, leyendo de un staging propio de ese command buffer. Se decodifica al enviarlo y el staging se recicla cuando el juego lo reinicia o libera. Las barreras, transiciones de layout y el orden del juego se aplican tal cual: no hay copias adelantadas, no hay escrituras concurrentes sobre la misma imagen y la CPU nunca espera a la GPU. **[V‑host]** en lavapipe con validación y validación de sincronización: 0 errores, 0 envíos extra, 0 esperas; reenviar un command buffer con datos nuevos, command buffers secundarios, mips y tamaños impares, 200 fotogramas con reinicio de pool (staging máximo 4 MB). Rendimiento: **[Disp.]**
- Modos anteriores conservados para comparar (Vortek › *Subidas de texturas BC*): *Por lotes + barreras*, *Por lotes (LURVETA 1.0)* y *Síncrono (Winlator 11.2)*. **[V‑host]**: los tres muestran errores de layout (la copia se adelanta a la transición del juego) y no vuelven a subir la textura al reenviar un command buffer; *Por lotes* además produce `SYNC-HAZARD-WRITE-AFTER-WRITE`, y *Síncrono* hace una espera bloqueante por región (211 en la prueba).
- El cliente recibe ahora el **resultado real** de `vkQueueSubmit` (antes se respondía `VK_SUCCESS` antes de enviar). Si falla la subida de texturas por un error que no sea pérdida del dispositivo, el trabajo del juego se envía igualmente.
- Decodificación en un pool de hilos propio, que espera solo sus tareas (antes esperaba también a las compilaciones de pipelines en curso). Escrituras de la caché de texturas en un hilo aparte (antes bloqueaban el submit del juego). **[V‑host]**
- El decodificador se libera en `vkDestroyDevice` y solo atiende a su dispositivo (antes podía usar objetos de un dispositivo ya destruido).

### Formatos
- **BC6H (UFLOAT/SFLOAT → RGBA16F) y BC7 (UNORM/sRGB → RGBA8)** con `bcdec` (MIT/Unlicense). Antes se anunciaban como compatibles pero la creación de la imagen fallaba. **[V‑host]**: 54/54 casos idénticos (o dentro de la tolerancia del redondeo) frente a un decodificador independiente (Pillow) para BC1–BC7, tamaños impares y decodificación multihilo.
- **Corregido un fallo heredado en BC4 multihilo:** usaba bloques de 16 bytes en vez de 8 al repartir la textura entre hilos, de modo que el 75 % de los texels salían mal y se leía fuera del búfer de origen (**AddressSanitizer: heap-buffer-overflow**) en texturas BC4 de 128 px de alto o más. **[V‑host]**
- Propiedades de formato: para BC emulado se exponen solo las capacidades que un formato comprimido puede tener (muestreo, filtrado, transferencia, blit de origen), y las propiedades de imagen se piden al driver con el formato descomprimido (antes eran valores fijos inventados). ETC2/EAC/ASTC, nativos en Mali, no se tocan; si un driver no los soporta, siguen sin anunciarse.
- `bcdec`: corregido un desplazamiento con signo indefinido detectado por UBSan (ver `third_party/README.LURVETA`).

### Congelamientos de Vortek
- `VK_ERROR_DEVICE_LOST` (submit, acquire, waits) y cualquier salida del bucle del servidor marcan `RING_STATUS_EXIT` en los dos anillos: el juego recibe `VK_ERROR_DEVICE_LOST` en lugar de quedarse esperando para siempre. Se registra en el log y en `files/logs/vortek-errors.log`. Las esperas internas del decodificador tienen límite (avisos cada 2 s, fallo controlado a los 20 s). Comprobación con el juego colgado: **[Disp.]**

### Optimizaciones
- **Caché de pipelines persistente** (Vortek › *Caché de pipelines persistente*, activada por defecto): solo cuando el juego no aporta caché (DXVK 2.x, wined3d). Cabecera propia con dispositivo, driver, UUID y suma de comprobación; un archivo ajeno o dañado se descarta. **[V‑host]**: guardar/cargar exacto y rechazo de archivos corruptos o de otro driver. Menos tirones a partir de la segunda partida: **[Disp.]** (lavapipe no guarda pipelines en su caché).
- **Variante `release`** no depurable, firmada con la misma clave que la debug. Mismo applicationId. **[V‑apk]**
- `BOX64_DYNACACHE` disponible (desactivado por defecto) en los presets personalizados de Box64; el Box64 0.4.4 incluido lo admite. Efecto: **[Disp.]**

### Benchmark
- `events.csv` con tirones (> 1 s), congelamientos (ningún fotograma en 5 s) y errores fatales de Vortek. `summary.txt` añade tirones por encima de 100 ms, tiempo hasta el fallo (≥ 10 s sin fotogramas o error fatal), CPU media y memoria máxima de los procesos de la app (app + Wine + Box64), temperaturas máximas y la fuente de los contadores de GPU (o "no legible"). `sensors.csv` añade `proc_cpu_pct`, `proc_rss_mb`, `procs`, `top_proc`, `gpu_load_pct` y `gpu_freq_mhz`. **[V‑host]**: muestreadores sobre `/proc` real.
- `tools/bench/analyze.py`: añade tirones por minuto, paradas y tiempo congelado a la comparación A/B (también con capturas de SurfaceFlinger de Winlator 11.2). **[V‑host]** con registros sintéticos de resultado conocido.

### Pruebas
- `tools/vortek-host-tests/run_tests.sh` y `tools/host-tests/run.sh` (este último ejecuta todo). Requisitos: `cc`, Python 3 con Pillow, Mesa lavapipe y la capa de validación de Khronos.

## 1.0.0

### P0 — Producto independiente
- applicationId `com.lurveta.co` (el namespace Java `com.winlator` es interno). Una sola fuente en `app/build.gradle`, exportada a BuildConfig y a CMake (`APP_ID`). **[V‑apk]**
- Rootfs en `files/rtfs`. `PathRelocator` sustituye al extraer `/data/data/com.winlator/files/rootfs` por `/data/data/com.lurveta.co/files/rtfs`. Las dos rutas miden 36 bytes, así que la sustitución es byte a byte y sin desplazar offsets, y también se reescriben los destinos de los enlaces simbólicos. Cubre el 100 % de las apariciones de los assets: 457 sustituciones en 171 archivos. **[V‑host]**: 0 apariciones restantes; los archivos modificados difieren solo en el prefijo; intérpretes ELF, RUNPATH e ICD de Vortek reubicados.
- FileProvider `${applicationId}.FileProvider` y acción del servicio derivada del paquete, para poder instalarse junto a Winlator. **[V‑apk]**
- Retirados Turnip (asset de 2,5 MB, diálogo, tipo de componente, `TU_DEBUG`) y libadrenotools junto con sus 4 bibliotecas hook. Vortek abre siempre el `libvulkan` del sistema. La selección "turnip" de contenedores existentes migra a Vortek. **[V‑apk]**
- Identidad LURVETA:
  - nombre en en/pt/ru, icono adaptativo vectorial con capa monocroma (Android 13), icono de notificación y cabecera del menú;
  - paleta grafito/cian, pantalla de inicio sin retardo artificial;
  - fondos de escritorio originales, logo del repositorio;
  - "Acerca de" con atribución, créditos, enlace al código y licencia incluida. **[V‑apk]**
- `CrashLogger`: guarda las excepciones Java no capturadas en `files/logs` y `Download/LURVETA/logs`.

### P1 — Decodificador de texturas BC de Vortek (solo GPUs sin BC, como Mali)
- **Subidas por lotes:**
  - Antes: un `vkQueueSubmit` seguido de `vkWaitForFences(UINT64_MAX)` **por cada región**, todo en el único hilo de Vortek.
  - Ahora: un anillo de 2 búferes de staging y **un solo command buffer y un solo submit** por cada submit del juego, con barrera de memoria TRANSFER→ALL_COMMANDS.
  - La CPU solo espera al reutilizar un slot en vuelo o si el juego envía a otra cola (en ese caso se mantiene la garantía anterior).
  - **[V‑host]**: 5 regiones → 1 submit y 0 esperas; lote de más de 32 MB → 2 slots; lectura byte a byte correcta. Ganancia en FPS y tirones: **[Disp.]**
- **Caché en disco:** se usa el tamaño de la región (antes, el del mip 0: hasta 64 MB de I/O por entrada); el búfer se asegura antes de leer (antes había fuga de memoria y copia desde un `VkBuffer` nulo en cada sesión nueva); la clave incluye la extensión de la región; el tamaño total se lleva en memoria. **[V‑host]**
- **sRGB:** BC1/2/3 `_SRGB` se guardan como `B8G8R8A8_SRGB` y las listas de formatos mutables se mapean por formato. **[V‑host]**
- **BC4 → R8 y BC5 → RG8** (UNORM/SNORM). Antes los canales se escribían en azul/verde de un texel BGRA, quedaban bytes con datos residuales y los valores SNORM se guardaban como UNORM (mapas de normales incorrectos). Ahora se usan 2–4× menos memoria y ancho de banda. **[V‑host]**
- Arrays y slices 3D se decodifican por capa. Las copias pendientes se invalidan al destruir su imagen o búfer (antes había uso de memoria liberada). Corregido un comportamiento indefinido (`0x3 << 30` con signo) en el decodificador BC1. **[V‑host]**

### P2 — Detección de hardware
- `HardwareInfo` y la función nativa `vkGetDeviceInfo`: vendorID, driverID, nombre, driver, versión de Vulkan y soporte BC/ETC2/ASTC; familias Mali / Immortalis / Adreno / PowerVR / Xclipse; clústeres de CPU y nombres de núcleo. Bloque "Hardware detectado" en "Acerca de". **[V‑host]** (clasificación)

### P3 — HUD y benchmarks
- `FrameStats`: anillo de timestamps sin bloqueos (en el camino del juego solo se ejecuta un `nanoTime`); media, 1 %/0,1 % low, percentiles y desviación. **[V‑host]**
- HUD completo: 1 %/0,1 % low, frametime, temperatura de CPU/batería, estado térmico de Android y GPU detectada por Vulkan; indicador REC.
- Menú del juego, "Benchmark (start/stop)": `frames.csv`, `sensors.csv` y `summary.txt` en `Download/LURVETA/benchmarks`.
- `tools/bench`: `sf_capture.py` (SurfaceFlinger por adb, mismo método para A y B) y `analyze.py` (varias pasadas con comprobación de varianza).

### P4 — Rendimiento sostenido
- Hilo de peticiones de Vortek: nice −4 y sesión ADPF (Android 13+, vía dlsym). Se informa el trabajo real de cada fotograma (intervalo menos el tiempo bloqueado esperando al juego, medido solo en el camino lento del ring buffer). Objetivo: el límite de FPS de DXVK si existe, y si no 60. Hilo compositor GLES con prioridad DISPLAY. Interruptor en la configuración de Vortek. Efecto: **[Disp.]**

### P5 — Perfiles por juego
- Ajustes del acceso directo › Avanzado › Perfil LURVETA: Rendimiento (Mali), Equilibrado, Compatibilidad y Estabilidad.
  - Rellenan campos existentes (preset de Box64, resolución, caché y opciones de Vortek) y no se aplican hasta pulsar OK.
  - El perfil "sugerido" según la GPU y la RAM es solo una indicación.
  - El límite de FPS de DXVK no se modifica.

### Aplazado (requiere autorización y benchmarks): P6 futex en los ring buffers y P7 swapchain multibúfer con espera de semáforos.
