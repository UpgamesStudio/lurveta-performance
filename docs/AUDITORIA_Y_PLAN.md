# LURVETA Performance — Informe técnico (Fase 1) y Plan (Fase 2) · rev. 2

**Identidad definitiva**
- Marca: LURVETA · Producto: **LURVETA Performance** · applicationId **`com.lurveta.co`** · repositorio **`lurveta-performance`** (independiente de UpGames).
- Base técnica: `winlator-app@a030f55`, el código del APK oficial de Winlator 11.2. **No** se usa el tag `v11.2.0`.
- La interfaz, el nombre, el icono y la pantalla de inicio son de LURVETA. Se conservan los avisos de copyright, las licencias (LGPL‑2.1 para Winlator, Vortek y Gladio; BSD‑2 para libadrenotools, que se retira; MIT, zlib, etc. para el resto) y la atribución "basado en Winlator, de BrunoSX".

Hardware objetivo: MediaTek Dimensity 8350 · Mali‑G615 MC6 · 12 GB · arm64‑v8a
Fecha: 2026‑11‑22.

> **Rev. 2:** las secciones 3 (rutas) y 4 (plan) sustituyen a las de la rev. 1. La auditoría técnica de las secciones 0–2 sigue vigente.

Leyenda:
- **[V]** Verificado leyendo el código o el binario (con archivo y línea).
- **[H]** Hipótesis: hay que medirla en el dispositivo.
- **[N/A]** No se puede analizar con los recursos de este entorno.

---

## 0. Qué es realmente "Winlator 11.2" (hallazgo previo importante)

| | Tag `v11.2.0` del repo `winlator` | APK oficial `Winlator_11.2.apk` |
|---|---|---|
| Commit del submódulo `app` | `c2f4ad4` | coincide con `winlator-app@a030f55` (14‑sep‑2026) |
| versionCode / versionName | 28 / "11.1" | 33 / "11.2" |
| Box64 | 0.4.0 | 0.4.4 |
| Gladio / Turnip | 1.0 / 26.1.0 | 1.1 / 26.2.0 |

- **[V]** El tag `v11.2.0` apunta a un código **más antiguo** que el APK publicado. Lo comprobé comparando assets, versionCode y los símbolos exportados de `libvortekrenderer.so` del APK oficial (`TextureDecoder_isFormatSupported`, `getSurfaceMinImageCount`, etc.).
- **Decisión técnica:** la base real de la 11.2 es `winlator-app@a030f55`. Toda la auditoría siguiente está **reverificada sobre `a030f55`**.
- **[V]** Después de la 11.2, upstream (6‑13 nov 2026) reescribió partes de Vortek: semáforos con futex, swapchain con present modes y un pool de rings. **No está publicado:** el asset `vortek-2.1.tzst` (el cliente del lado del juego) no se ha recompilado. Es trabajo a medio camino.

Licencias **[V]**: Winlator es LGPL‑2.1. Vortek y Gladio tienen su propio LICENSE en sus repos. Box64 es MIT, DXVK zlib, VKD3D‑Proton LGPL y Mesa MIT. Un fork independiente es legal si se publica el código modificado y se mantienen la atribución y las licencias.

---

## 1. Arquitectura (verificada)

```
Juego x86‑64 (Windows)
  └─ Wine (x86‑64 PE/unix)  ── ejecutado por Box64 (dynarec x86‑64 → ARM64)
       ├─ D3D9/10/11 → DXVK 2.4.1 (o 1.10.3 si Vulkan < 1.3)
       ├─ D3D12      → VKD3D‑Proton 2.14.1
       └─ OpenGL     → WineD3D/Gladio (GL→GLES) o Zink (GL→Vulkan)
            └─ libvulkan_vortek.so (ICD aarch64 dentro del rootfs glibc)
                 └─ ring buffers en ashmem + socket Unix
                      └─ libvortekrenderer.so (proceso Android de Winlator)
                           └─ driver Vulkan del sistema (ARM Mali)
                                └─ AHardwareBuffer por ventana X
                                     └─ compositor GLES (GLSurfaceView) → SurfaceFlinger
```

- **[V]** En Mali no hay Turnip. `GraphicsDrivers.getDefaultDriver()` solo detecta Adreno con una regex sobre `GL_RENDERER`. Cualquier otra GPU recibe Vortek + Gladio. El default es correcto para Mali, pero el usuario puede elegir Turnip a mano sin ninguna advertencia.
- **[V]** No existe detección explícita de Mali, Immortalis, PowerVR ni Xclipse.
- **[V]** La versión de DXVK se elige así: Vortek con `vkGetApiVersion() >= 1.3` → 2.4.1, y si no → 1.10.3 (`DefaultVersion.DXVK`).

---

## 2. Cuellos de botella identificados

### 2.1 Decodificación de texturas BC en la CPU (específico de GPUs sin BC: Mali)

Contexto: D3D usa formatos BC (DXT1/5, BC4/5…). **[H, casi seguro]** El driver Mali no expone `textureCompressionBC`; hay que confirmarlo con `vulkaninfo` en el dispositivo. Sin él, Vortek activa `TextureDecoder`, que descomprime en la CPU. En Adreno el decodificador ni se crea (`TextureDecoder_create` devuelve NULL), así que **todo lo de esta sección solo afecta a Mali y no toca a Adreno.**

1. **[V] Sincronización CPU↔GPU por cada región.** En `texture_decoder.c:271-329`, `TextureDecoder_decodeAll()` se ejecuta **dentro** de `vt_handle_vkQueueSubmit` (`request_handler.c:257` y `:3421`), en el **único hilo** que atiende todas las llamadas Vulkan del juego. Por cada región copiada (cada nivel de mip de cada textura) hace lo siguiente:
   - mmap del búfer, hash, lectura de caché en disco o decodificación;
   - `vkQueueSubmit` + `vkWaitForFences(UINT64_MAX)` + `vkResetFences`.

   Mientras tanto, todo el juego queda bloqueado. Una textura de 2048² con 12 mips supone 12 viajes de ida y vuelta completos a la GPU.
   **[H]** Es la causa más probable de los tirones al cargar texturas en mundo abierto (conducir por la ciudad).

2. **[V] Bug en la caché de texturas** (`texture_decoder.c:81-123`, `:292-306`):
   - El tamaño leído y escrito es `image->width*height*4` (el del **mip 0**), no el de la región. Con los mips ≥ 1024 px de una textura grande se escriben y leen bytes de más: hasta 64 MB por entrada para una textura de 4096². Eso llena la caché de 256 MB con unas pocas entradas y genera I/O enorme en el hilo crítico.
   - Lee la caché **antes** de asegurar el búfer de decodificación. En una sesión nueva, `decompressedData == NULL`, y entonces `fileGetContents()` entra en otra rama: reserva memoria nueva (que nunca se libera) y devuelve "éxito". Después se copia desde un `VkBuffer` nulo. Resultado: **fuga de memoria y uso inválido de Vulkan.** **[H]** Posible causa de cierres o "device lost" al relanzar juegos con texturas ≥ 1024 px.
   - La clave del nombre de archivo no incluye el nivel de mip ni la extensión de la región.

3. **[V] sRGB incorrecto.** `DECOMPRESSED_FORMAT = VK_FORMAT_B8G8R8A8_UNORM` (`texture_decoder.h:62`) se usa también para `BC*_SRGB_BLOCK`, y las vistas sRGB se convierten a UNORM (`request_handler.c:827-829`). La GPU no linealiza esos texels, así que los colores salen lavados o demasiado claros. Afecta a la calidad, no a los FPS.

4. **[V] BC4 (1 canal) y BC5 (2 canales, mapas de normales) se expanden a 4 bytes por píxel.** Con R8 o RG8 bastaría la mitad o la cuarta parte de la memoria y del ancho de banda. En Mali el ancho de banda es el recurso más caro.

5. **[V]** El pool de decodificación es de 8 hilos fijos (`THREAD_POOL_NUM_THREADS 8`), sin afinidad. Compite con Box64 por los núcleos.

### 2.2 Esperas activas (busy‑wait): consumo, calor y throttling

- **[V]** `events.h:88-94` y `ring_buffer.c:149-170`, en ambos lados (app y cliente Vortek): hasta 500 `thrd_yield()` y luego `usleep(100 µs)` en bucle infinito. Un hilo que espera se despierta unas 10.000 veces por segundo y no deja que el núcleo entre en reposo profundo.
- **[H]** Contribuye al calentamiento en sesiones largas. Hay que medirlo con potencia y temperatura en el dispositivo.
- **[V]** Upstream ya lo resolvió con futex en noviembre (`winlator-app@2a4aa69`, `vortek@e4d4335`, `a7746fb`), pero sin publicar.

### 2.3 Presentación de fotogramas

- **[V]** La swapchain de Vortek en la 11.2 es de **un solo búfer**: `getSurfaceMinImageCount()` devuelve 1, acquire siempre da el índice 0 y todas las imágenes son alias del mismo `AHardwareBuffer` (`xwindow_swapchain.c`).
- **[V]** `vt_handle_vkQueuePresentKHR` **ignora `pWaitSemaphores`** (`request_handler.c:2087`): avisa al compositor sin esperar a que la GPU termine.
- Consecuencias:
  - **[H]** Tearing o fotogramas parciales.
  - El lado bueno: el juego nunca se bloquea por VSync, así que no hay cuantización de FPS.
- **[V]** El compositor GLES usa `RENDERMODE_WHEN_DIRTY` y `eglSwapInterval(0)` (commit `4f539f0`). Compone un fotograma por cada present del juego.
- **[V]** El limitador de FPS de Winlator es el de DXVK (`dxgi.maxFrameRate` / `d3d9.maxFrameRate` en el `dxvk.conf` generado), basado en sleep. Como el present no bloquea, no se suma una espera de VSync encima.

### 2.4 Serialización de llamadas Vulkan

- **[V]** El cliente Vortek usa **un mutex global** para todas las llamadas (`vortek/src/vulkan_calls.c:20-25`), y el servidor un solo hilo de peticiones (`vk_context.c:44`). Los hilos de DXVK (CS, submit, compilación de pipelines) se serializan ahí.
- La creación de pipelines sí es asíncrona en el servidor (`async_pipeline_creator.c`).
- Arreglarlo exige rediseñar el protocolo. **Riesgo alto: lo dejo fuera de esta versión.**

### 2.5 CPU, hilos y térmica

- **[V]** No se usa ninguna API de rendimiento o térmica de Android: ni `setThreadPriority`, ni ADPF (`PerformanceHintManager`), ni `getThermalHeadroom`, ni `setSustainedPerformanceMode`.
- **[V]** La temperatura se lee probando una lista fija de rutas (`CPUStatus.getTemperature`), sin mirar `thermal_zone*/type`. **[H]** En MediaTek probablemente lee una zona que no es la de la CPU o la del SoC.
- **[V]** La afinidad por defecto son todos los núcleos (`Container.getFallbackCPUList`). Existe afinidad por contenedor y por juego, aplicada vía WinHandler (`Win32AppWorkarounds`).
- Topología del Dimensity 8350 según la ficha pública (**se verificará leyendo `/sys/devices/system/cpu/*/cpufreq`**): 1× A715 a 3,35 GHz + 3× A715 a 3,2 GHz + 4× A510 a 2,2 GHz. No tiene SMT/HyperThreading: **no hay nada que "desactivar"**.

### 2.6 Box64

- **[V]** Se distribuye **solo como binario** (`assets/box64/box64-0.4.4.tzst`); el código fuente de Box64 no está en este repo. 0.4.4 es el último tag estable upstream.
- **[V]** Los presets son solo variables de entorno (`Box64PresetManager.java`). El preset por defecto de la 11.2 cambió en el commit `c292889`.
- **[H]** Recompilar Box64 con ajustes para Cortex‑A715 daría poca ganancia: el código que genera el dynarec no depende de `-mcpu`. Solo lo haré si las mediciones lo justifican.

### 2.7 Configuración específica de GTA V

- **[V]** `DXVKConfigDialog.getDXVKConfigContent()` escribe siempre `[GTA5.exe] dxgi.maxDeviceMemory = 512` y `maxSharedMemory = 512`. GTA V ve 512 MB de VRAM y limita sus opciones gráficas. Es un workaround deliberado: subirlo puede provocar falta de memoria. Debe ser una opción por perfil, probada en el dispositivo y no aplicada a ciegas.

### 2.8 Lo que no se puede optimizar aquí

- **[N/A]** Wine, DXVK, VKD3D‑Proton, Mesa (Zink/VirGL/Turnip), Box64 y el rootfs glibc vienen precompilados en `.tzst`. Recompilarlos es posible, pero cada uno es un proyecto aparte. Esta versión solo **selecciona y configura** esos binarios, y lo hace con mediciones.
- **[N/A]** No tengo el Dimensity 8350. Ninguna cifra de rendimiento saldrá de aquí: **todas las mediciones se harán en tu dispositivo.**
- No se puede mejorar el driver Mali (es cerrado), ni subir frecuencias o voltajes, ni saltarse la protección térmica. Tampoco lo voy a intentar.

---

## 3. applicationId `com.lurveta.co` y rutas internas (auditoría completa)

### 3.1 Inventario verificado
Analicé con un script **todos** los assets incluidos en el APK, una vez extraídos: rootfs, container_pattern, pulseaudio, rootfs_patches, Box64, los cinco drivers gráficos, DXVK/D8VK/D7VK/VKD3D/cnc‑ddraw y los nueve wincomponents. También las bibliotecas precompiladas de `jniLibs` y el código C y Java.

| Dónde | Pares (cadena distinta, archivo) | Tipo |
|---|---|---|
| Binarios ELF: intérprete `PT_INTERP`, RPATH/RUNPATH, cadenas de `.rodata` (prefijos de GStreamer, Wine `ntdll.so`, krb5…) | 352 | ELF |
| `etc/ld.so.cache` y otros binarios no ELF | 48 | binario |
| Configuración: fonts.conf, alsa.conf, pulse/client.conf, X11 Compose, JSON del ICD de Vortek… | 25 | texto |
| Enlace simbólico `container_pattern/.wine/dosdevices/z:` | 1 | symlink |
| `jniLibs` (bibliotecas precompiladas del APK) | 0 | — |
| Código propio: `AppUtils.java:47`, `vortek.h:8`, `gladio.h:16`, `winlator.h:14`; FileProvider `com.winlator.FileProvider`; acción `com.winlator.action.ENSURE_FOREGROUND` | 6 | fuente |

**[V] Hallazgo clave:** el 100 % de las apariciones en binarios y textos empiezan por el **mismo prefijo**: `/data/data/com.winlator/files/rootfs`. No existe ninguna otra ruta.

En total, el relocalizador hizo **457 sustituciones en 171 archivos** (rootfs: 447 en 165 archivos; Box64, Vortek, Gladio, VirGL y rootfs_patches: el resto).

### 3.2 Por qué no se puede hacer una sustitución "a lo bruto"
- `com.lurveta.co` tiene 14 caracteres frente a los 12 de `com.winlator`.
- Alargar cadenas dentro de un ELF desplaza offsets y rompe el intérprete, las tablas de cadenas, `ld.so.cache` y los literales que el enlazador comparte (tail‑merging).

### 3.3 Estrategia elegida (sin alargar ninguna cadena)
- El rootfs pasa a vivir en **`files/rtfs`** en lugar de `files/rootfs`.
- La nueva ruta absoluta `/data/data/com.lurveta.co/files/rtfs` mide **exactamente 36 bytes**, igual que la original. Cada aparición se reemplaza **byte a byte, con la misma longitud**: no cambia ningún offset ni ningún terminador NUL.
- Es la única sustitución que se aplica, y solo con ese prefijo exacto. No se toca nada más.
- **Dónde se aplica:** en un único punto, `TarCompressorUtils.extract()`, por el que pasan todas las extracciones (rootfs, componentes del APK y componentes descargados). Funciona por streaming y detecta coincidencias que crucen el límite del búfer.
- **Enlaces simbólicos:** su destino se reescribe sin restricción de longitud.
- **Código propio:** las rutas se generan desde el applicationId (Gradle → CMake → C/Java). El FileProvider pasa a `${applicationId}.FileProvider` y la acción del servicio al namespace del paquete.
- **Comprobación previa a la entrega:** se ejecuta el mismo código Java del patcher sobre los assets reales en esta máquina y se verifica que:
  - no queda ninguna aparición de `com.winlator` en binarios ni textos;
  - los ELF siguen siendo válidos (`readelf`) y su intérprete apunta a la ruta nueva;
  - el resto de bytes de cada archivo no cambia.

### 3.4 Instalación simultánea con Winlator original
- **Datos separados:** applicationId distinto, FileProvider distinto (si no, Android rechaza la instalación con `INSTALL_FAILED_CONFLICTING_PROVIDER`), acción del servicio distinta y ningún permiso personalizado compartido.
- **Paquete Java:** el namespace interno `com.winlator.*` se mantiene. No es visible para el usuario y renombrarlo solo añadiría riesgo y ruido de diffs.
- **Límite heredado de upstream:** usar `/data/data` implica que solo funciona en el usuario principal de Android. En perfiles de trabajo o usuarios secundarios falla, igual que Winlator original.
- **[N/A]** La instalación conjunta real solo se puede comprobar en un dispositivo. Aquí verifico el manifest final (`aapt dump`) para descartar conflictos.

### 3.5 Componentes exclusivos de Adreno/Snapdragon (se retiran)
| Componente | Motivo |
|---|---|
| `graphics_driver/turnip-26.2.0.tzst` (≈2,5 MB) + descarga de versiones de Turnip | Mesa Turnip solo funciona en Adreno |
| `TurnipConfigDialog` y la opción "Turnip" del selector | Igual |
| `libadrenotools` y sus hooks (`libmain_hook`, `libhook_impl`, `libfile_redirect_hook`, `libgsl_alloc_hook`) + la opción "adrenotoolsDriver" de Vortek | Solo sirven para cargar drivers Adreno personalizados. Vortek usa el `libvulkan` del sistema en cualquier otro caso. |
| `TU_DEBUG=noconform` en las variables por defecto | Variable de Turnip |

**Se conservan** porque funcionan en Mali o son ARM64 genéricos: Vortek, Gladio, Zink, VirGL, DXVK, D8VK, D7VK, VKD3D, cnc‑ddraw, WineD3D, Box64, rootfs, PulseAudio/ALSA y FluidSynth.

**Migración:** un contenedor que tuviera "turnip" configurado pasa automáticamente a Vortek. En un dispositivo Adreno, LURVETA Performance funcionaría solo con Vortek. Es una decisión de producto aceptada: el objetivo es Mali.

---

## 4. Plan de implementación (Fase 2)

Ordenado por **impacto ÷ riesgo**. Cada punto indica los archivos que toca, el beneficio esperado y el riesgo. Las optimizaciones de Mali se activan **solo si el driver es ARM** (`driverID == VK_DRIVER_ID_ARM_PROPRIETARY` o `vendorID == 0x13B5`). Adreno y el resto mantienen el comportamiento original.

| # | Cambio | Archivos | Beneficio esperado | Tipo | Riesgo |
|---|---|---|---|---|---|
| P0 | LURVETA Performance: `com.lurveta.co`, rootfs en `files/rtfs`, relocalizador de rutas de igual longitud (§3), identidad visual propia con atribuciones, retirada de Turnip/adrenotools (§3.5), registro de errores en archivo | `build.gradle`, `TarCompressorUtils`, `RootFS`, `AppUtils`, `AndroidManifest`, `CMakeLists`, `vortek.h`, `gladio.h`, `winlator.h`, recursos, nuevos `PathRelocator` y `CrashLogger` | Producto propio, instalable junto a Winlator | Requisito | Medio (rutas: verificado en host) |
| P1a | Corregir la caché de texturas (tamaño por región, búfer asegurado antes de leer, clave con nivel de mip) | `texture_decoder.c` | Menos I/O en el hilo crítico, sin fuga de memoria ni copias desde búfer nulo | **[V]** bug | Bajo |
| P1b | Decodificación por lotes: un solo command buffer y un solo submit por `vkQueueSubmit`, con barrera; sin `vkWaitForFences` por región; staging en anillo | `texture_decoder.c/.h`, `request_handler.c` | Menos tirones en streaming de texturas (exteriores y tráfico) | **[H]** a medir | Medio |
| P1c | sRGB correcto (`B8G8R8A8_SRGB` + lista de formatos mutables) | `texture_decoder.c`, `request_handler.c` | Colores correctos | **[V]** bug | Bajo‑medio |
| P1d | BC4→R8 y BC5→RG8 | `bc_decoder.h`, `texture_decoder.c`, `vulkan_helper.c` | 2–4× menos memoria y ancho de banda en esas texturas | **[V]** en bytes; FPS **[H]** | Medio |
| P2 | Detección de GPU por Vulkan: vendor, driverID y nombre (Mali / Immortalis / Adreno / PowerVR / Xclipse); aviso si se elige Turnip en una GPU que no es Adreno | `GPUHelper.java`, `gpu_helper.c`, `GraphicsDrivers.java`, UI | Perfiles correctos y sin configuraciones de Adreno en Mali | **[V]** | Bajo |
| P3 | Telemetría y HUD: frametime real, media, 1 % y 0,1 % lows, CSV por sesión, frecuencias por clúster, temperatura por `thermal_zone/type` + `getThermalHeadroom`, RAM del sistema y del proceso | `FrameRating.java`, `CPUStatus.java`, nuevo `PerfLogger` | Mediciones reproducibles para la Fase 5 | Herramienta | Bajo |
| P4 | Prioridad y ADPF para los hilos críticos del host (servidor Vortek, servidor X, compositor) | `vk_context.c`, `XServerDisplayActivity.java` | Menos jitter y mejor reparto entre clústeres | **[H]** a medir | Bajo‑medio |
| P5 | Perfiles (Rendimiento / Equilibrado / Compatibilidad / Estabilidad), aplicados por contenedor o por acceso directo **solo cuando el usuario lo elige**: Box64, resolución, límite de DXVK, opciones de Vortek, memoria de GTA V | nuevo `PerformanceProfiles.java`, diálogos | Configuración por juego | Herramienta | Bajo |
| P6 | Portar los futex de upstream (adiós al busy‑wait) y recompilar el cliente Vortek desde `vortek@a7746fb` | ring buffers en ambos lados, `vortek-2.x.tzst` | Menos consumo y calor en sesiones largas | **[H]** a medir | Alto (protocolo, toolchain glibc) |
| P7 | Swapchain multibúfer con espera de semáforos (backport de `7c21e8d`) | `xwindow_swapchain.c`, `request_handler.c` | Sin tearing ni fotogramas parciales | **[V]** problema, mejora **[H]** | Alto (depende de P6) |

**Estado de aprobación (rev. 2):**
- **P0–P5 aprobados** y en implementación.
- **P6–P7 aplazados** hasta tener benchmarks reales del dispositivo. No se implementará ningún cambio de alto riesgo sin autorización.
- **Reducción del tamaño de instalación:** viene de retirar Turnip y adrenotools (§3.5). La cifra exacta se mide comparando el APK final con el de la 11.2.

**Fuera de esta versión, con justificación:** rediseño multi‑hilo del protocolo Vortek (§2.4), recompilar Wine/DXVK/Mesa, VKD3D en Mali (D3D12 exige funciones que hay que comprobar antes) y forzar afinidad o frecuencias por defecto.

---

## 5. Metodología de validación (Fase 5)

- **A** = APK oficial 11.2 (`com.winlator`) · **B** = DPE (`com.winlatdp`), instalados a la vez.
- Mismo juego y versión, mismo contenedor exportado, misma resolución, mismo dxvk.conf, mismo preset de Box64 y misma escena.
- Inicio térmico controlado: pantalla apagada hasta que `thermal_zone` del SoC baje de 35 °C; brillo fijo; sin cargador; modo avión.
- Cada escena: **3 pasadas de 120 s** más una pasada sostenida de 20 min. Se registran media, 1 % y 0,1 % lows, percentiles de frametime, RAM, temperatura, frecuencias y tiempo de carga.
- El HUD de B graba un CSV. Para A se usa `adb shell dumpsys SurfaceFlinger --latency` con el mismo script de análisis.
- Escenas de GTA V: calle con tráfico, centro urbano, vegetación (Paleto o Chiliad), campo abierto, interior y una zona con muchos NPC.
- Criterio: una mejora solo cuenta si la media de las 3 pasadas supera la variación entre pasadas.

---

## 6. Estado de la compilación (Fase 4, verificado)

- Toolchain instalada: JDK 17, Android SDK (platform 34/35), NDK 24.0.8215888 y CMake 3.22.1.
- Para esta máquina hicieron falta dos ajustes locales, **sin tocar el proyecto**:
  - un mirror de Maven Central, por errores 429 de `repo.maven.apache.org`;
  - `pickFirst` para bibliotecas `.so` duplicadas por AGP. Comprobé byte a byte que son idénticas.
- **[V] Compilación de referencia de la 11.2 real (`winlator-app@a030f55`): BUILD SUCCESSFUL.**
  - `app-debug.apk`, 157.548.847 bytes; el oficial pesa 157.549.075 (la diferencia es la firma).
  - `package com.winlator`, versionCode 33, versionName 11.2, arm64‑v8a.
  - **Misma lista de bibliotecas nativas y assets que el APK oficial.** La base es reproducible y está lista para modificarse.
- Copia limpia del original: worktree en `a030f55`, sin cambios. El APK oficial se descargó aparte como referencia y no se ha modificado.
