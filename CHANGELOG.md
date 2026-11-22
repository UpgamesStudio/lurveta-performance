# Registro de modificaciones — LURVETA Performance

Base: Winlator 11.2 (`winlator-app@a030f55`, tag `upstream-winlator-11.2`). Cada punto indica cómo se verificó.
- **[V‑host]**: probado en Linux con el código real (ver `tools/host-tests`).
- **[V‑apk]**: comprobado en el APK compilado (aapt/strings).
- **[Disp.]**: requiere medición en el dispositivo.

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
