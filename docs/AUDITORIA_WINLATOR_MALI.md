# Auditoría técnica y plan de transformación: Winlator Mali → LURVETA Performance

Fecha: 2026-11-23 · Estado: **propuesta, pendiente de aprobación** · Nada de esto modifica `main`.

Material auditado:

| Fuente | Identificación |
|---|---|
| APK probado por el usuario | `Winlator_Mali_1.2.1_Bionic_Ludashi.apk`, SHA-256 `eb4a65e3adf941ba23d0bcbb6634c9bdce491c87c9ad3915fef4a0bb1e6a7066`, paquete `com.ludashi.benchmark`, versionCode 23, firma `CN=WinlatorMali` (SHA-256 `1ac40582…89b6`) |
| Código fuente | `github.com/GunaCharanTeja/WinlatorMali`, commit `0411a4b` (2026-11-01) |
| Nuestra base | `UpgamesStudio/lurveta-performance`, rama de trabajo `ccr-964d8b63-2s3tyb` (último commit `c76908a`) y `main` |

Convención: **[C]** comprobado en el código o en el APK · **[I]** inferido, sin verificar · **[D]** requiere medición en el dispositivo.

---

## 1. Resumen ejecutivo

1. Winlator Mali no es un ajuste del Winlator que usábamos: es **otra arquitectura**. La cadena es brunodev85 → Winlator Bionic (Pipetto-crypto) → CMod (`com.winlator.cmod`) → Winlator Mali. **[C]**
2. Su ventaja de rendimiento se explica por tres decisiones de diseño, no por un ajuste fino:
   - **Wine ARM64EC nativo.** Solo el código x86 del juego se traduce (FEXCore/WOWBox64); Wine en sí no pasa por Box64. **[C]**
   - **Vulkan dentro del proceso del juego.** El "Wrapper" basado en Mesa carga el driver Mali en el mismo proceso, sin el viaje por socket que hacía Vortek. **[C]**/**[I]**
   - **Texturas BC transcodificadas por GPU a ASTC/ETC2**, formatos comprimidos que el Mali soporta, en lugar de descomprimirlas a RGBA. **[C]** configuración / **[I]** funcionamiento interno
3. **El proyecto está abandonado**: el README declara 1.2.1 como versión final. Todo mantenimiento será nuestro. **[C]**
4. **El código fuente no basta para reproducir el APK.** **[C]**
   - `imagefs.tzst` (190 MB) se descarga al compilar desde los releases del autor.
   - Wine/Proton no viene en el APK: se instala desde repositorios de contenidos de terceros.
   - El Wrapper y la capa BCn son binarios precompilados.
5. Dos defectos que ya corregimos en LURVETA **siguen presentes** en Winlator Mali y se pueden portar tal cual. **[C]**
   - El servidor X escribe con `write()` bloqueante. Es lo que congeló GTA V con un ANR.
   - No se sube `RLIMIT_NOFILE` con esync activo. Es lo que atascó la carga de GTA V con 1019 descriptores abiertos.
6. **El audio** usa por defecto PulseAudio-GN con salida AAudio. En el sistema base hay `libasound.so.2` pero no `libpulse.so.0`, solo `libpulse.so`. Si el Proton instalado enlaza `libpulse.so.0`, el controlador `winepulse` de Wine no puede conectar. **[C]** archivos / **[I]** causa. La primera prueba es el controlador ALSA.
7. **Parte de los FPS puede venir del nombre de paquete Ludashi**, que activa perfiles de rendimiento del fabricante. Con `com.lurveta.co` ese truco no existe. Hay que medirlo antes de fijar cualquier objetivo de FPS. **[D]**

---

## 2. Auditoría de Winlator Mali

### A. Arquitectura de emulación

| Elemento | Hallazgo | Evidencia |
|---|---|---|
| Wine principal | `proton-10-arm64ec`; alternativas `proton-9.0-arm64ec` y `proton-9.0-x86_64` | `core/WineInfo.java:20` (`MAIN_WINE_VERSION = proton 10 arm64ec`), `res/values/arrays.xml` `wine_entries` **[C]** |
| Origen de Wine | **No viene en el APK** (no hay `proton-*.tzst` en assets ni en `imagefs`). `installWineFromAssets` ignora en silencio los assets que faltan; Wine llega por "Contents" desde Bannerlator, WinNative, StevenMXZ y Winlator-Extras | `xenvironment/ImageFsInstaller.java:48-60`, `contents/ContentsManager.java:28-31` **[C]**. Versión exacta del Proton que usó el usuario: **[D]** (hay que leerla en su contenedor) |
| Ejecución ARM64EC | Wine se lanza directamente (`winePath/bin/wine …`), sin Box64. Para x86 de 32 bits fija `HODLL=libwow64fex.dll` (FEX) o `wowbox64.dll` | `GuestProgramLauncherComponent.java` ~l. 495-515 **[C]** |
| Wine x86_64 (no ARM64EC) | `box64 wine …`, igual que nuestra arquitectura anterior | mismo archivo **[C]** |
| FEXCore | Versiones 2508, 2601, 2605 (por defecto) y `fexcore-hybrid-x86`. Las DLL se copian en `system32`; las unixlib (`libarm64ecfex.so`, `libwow64fex.so`) se reconcilian en `imagefs/usr/lib/wine/aarch64-unix` | `GuestProgramLauncherComponent.java:132-195`, `core/DefaultVersion.java` **[C]** |
| Box64 / WOWBox64 | Box64 0.3.7 y 0.4.3 (por defecto); WOWBox64 0.3.7 y 0.4.3 | assets `box64/`, `wowbox64/` **[C]** |
| Espacio de usuario | `imagefs` con `usr/lib/libc.so`, sin `ld-linux`: sistema base **Bionic** (estilo Termux), no glibc | listado de `imagefs.tzst` **[C]** |
| Entorno | `LD_PRELOAD` con `libandroid-sysvshm.so`, `libfakeinput.so` (evdev simulado) y `libjpeg`/`libcrypto` del sistema; `WINEESYNC=1` por defecto; `BOX64_MMAP32=0` en Mali | `GuestProgramLauncherComponent.java:340-470`, `container/Container.java:26` **[C]** |
| Diferencia con LURVETA | LURVETA: Box64 emula **todo** Wine 10.10 x86_64, sobre un sistema base glibc. Winlator Mali: Wine es ARM64 nativo y solo se traduce el código del juego | **[C]** |
| Ventaja real | Menos instrucciones traducidas: las llamadas del sistema, ntdll y win32u se ejecutan nativas | **[I]**; la magnitud es **[D]** |
| Riesgo | La compatibilidad de FEX/ARM64EC varía por juego (protecciones anticopia, JIT, código automodificable). Depende de Proton de terceros sin versión fijada | **[I]** |

### B. Motor gráfico y Vulkan

| Elemento | Hallazgo | Evidencia |
|---|---|---|
| Driver por defecto | `graphicsDriver = "wrapper"`, versión "System": el driver Vulkan del sistema (Mali) a través del Wrapper | `Container.java:28`, `DefaultVersion.java:7` **[C]** |
| Wrapper | `libvulkan_wrapper.so` (8,7–19 MB según variante; hay 5: `wrapper`, `wrapper-1.1`, `wrapper-v2`, `wrapper-leegao`, `wrapper-gamenative`). Compilado desde un árbol de **Mesa** (`src/vulkan/wrapper/wrapper_*.c`, `src/util/format/*`), con ruta de compilación `/home/gunanxt/gamenative-wrapper`. No está despojado de símbolos (incluye debug_info) | `strings`/`readelf` del binario **[C]**. Su código fuente **no** está en el repositorio **[C]**; que exista un repositorio público de origen es **[I]** |
| Comunicación juego → driver | DXVK (en Wine) → `winevulkan` → cargador Vulkan del sistema base (`libvulkan.so.1.4.315`) → ICD `wrapper_icd.aarch64.json` → driver Mali, **dentro del mismo proceso** | listado de `imagefs` y de los tzst **[C]**; que no haya ninguna comunicación entre procesos es **[I]** (es lo que implica el diseño del ICD) |
| Presentación | Extensiones X `DRI3` y `Present` con AHardwareBuffer (`gpu_image.c`, `renderer/egl.cpp`), más un renderizador EGL propio con FSR/SGSR/FXAA y generación de fotogramas (ZeroFG). Incluye `readback.cpp`, que conviene medir porque puede costar | `xserver/extensions/*.java`, `cpp/winlator/renderer/*` **[C]**; coste **[D]** |
| Modos de presentación | `MESA_VK_WSI_PRESENT_MODE`, `WRAPPER_MAX_IMAGE_COUNT`, `WRAPPER_DISABLE_PRESENT_WAIT`, `MESA_VK_WSI_DEBUG=forcesync` | `XServerDisplayActivity.java:2425-2441` **[C]** |
| Texturas BC | Capa Vulkan implícita `libbcn_layer.so` (16 MB) y `leegao_bcn_legacy`. `version.txt`: "Modified Version Of Leegao's Bcn Layer By Fcharan/WinMali-Dev". Variables: `WRAPPER_EMULATE_BCN` (0-3), `ENABLE_BCN_COMPUTE`, `BCN_COMPUTE_AUTO`, `BCN_TRANSCODE_TO_ASTC/ETC2/ETC1`, `BCN_QUALITY_PRESET`, `BCN_NATIVE_DIRECT`, `BCN_KEEP_RAW`, `WRAPPER_USE_BCN_CACHE` | `XServerDisplayActivity.java:2442-2474`, `leegao_bcn.tzst` **[C]**. Algoritmo interno **[I]** (binario cerrado) |
| Caché | `WRAPPER_CACHE_PATH=imagefs/usr/var/cache` (caché BCn en disco), `DXVK_STATE_CACHE_PATH` | `GuestProgramLauncherComponent.java` **[C]** |
| Memoria GPU | `WRAPPER_VMEM_MAX_SIZE` (opcional); `WRAPPER_DISABLE_PLACED` cuando `BOX64_MMAP32=1` y Wine no es ARM64EC | **[C]** |
| DXVK por defecto | `dxvk+vkd3d`, DXVK **1.7.2**, vkd3d desactivado. Coherente con nuestro hallazgo de que DXVK 2.x necesita `VK_EXT_robustness2`, que este Mali no tiene | `Container.java:38-39`, `DefaultVersion.java` **[C]** |
| Otros | `libVkLayer_khronos_validation.so` (21 MB) incluido en `layers.tzst`: peso muerto en un APK de producción | **[C]** |

**Por qué rinde más que nuestra implementación anterior.** En orden de impacto probable; la magnitud de cada uno es **[D]**:
1. **No hay viaje por socket en cada llamada Vulkan.** Vortek serializaba cada llamada y la mandaba por socket y memoria compartida a otro proceso.
2. **Se transcodifica a ASTC/ETC2 en vez de descomprimir a RGBA8.** Ocupa de 4 a 8 veces menos memoria y ancho de banda.
3. **Wine nativo ARM64EC.** Solo se traduce el código del juego.
4. **El nombre de paquete Ludashi** puede activar perfiles de rendimiento del fabricante.

### C. Rendimiento y temperatura

| Mecanismo | Implementado | Evidencia |
|---|---|---|
| Sustained Performance Mode | Sí, desactivado por defecto | `perf/PerformanceManager.java:172-177` **[C]** |
| GameManager (señal de modo juego) | Sí | `perf/GameModeSignal.java` **[C]** |
| Prioridad de hilos y núcleos grandes | Sí: `PerfPriority`, `CpuTopology`, `taskset` | `perf/*`, `core/ProcessHelper.java:162` **[C]** |
| Samsung SPerf | Solo Samsung | `perf/SamsungSPerfDriver.java` **[C]** |
| RAM Booster | Supervisa la RAM y "recorta" procesos en segundo plano con perfiles (Smart, Hammer) | `xenvironment/components/RamBoosterComponent.java` **[C]**; efecto real en Android 16 **[D]** |
| HUD con telemetría | GPU, frecuencias, CPU, vatios y frametimes (`HudDataSource` lee el estado térmico) | `widget/HudDataSource.java:333` **[C]** |
| ADPF / PerformanceHint | **No encontrado** | grep sin resultados **[C]** |
| Medición sostenida (15/30/45 min) | **No existe** | **[C]** |

Las causas del calentamiento y de los bajones son **[D]**: hacen falta mediciones a temperatura controlada. Hipótesis a contrastar:
- Generación de fotogramas o postprocesado activos.
- Transcodificación BC continua por falta de caché.
- `readback` en la presentación.
- FEX traduciendo bucles de espera activa.

### D. Audio

| Hecho | Evidencia |
|---|---|
| El controlador por defecto es `pulseaudio-gn`. Las opciones son ALSA, PulseAudio y PulseAudio-GN | `Container.java:36`, `arrays.xml` **[C]** |
| PulseAudio-GN arranca `libpulseaudio.so` (del bundle GameNative) con `module-native-protocol-unix` y `module-aaudio-sink performance_mode=1 low_latency=true`, y `PULSE_LATENCY_MSEC=40` por defecto | `PulseAudioComponent.java:107-174`, `Container.java:26` **[C]** |
| Wine elige su controlador con el registro `Software\Wine\Drivers\Audio` = `alsa` o `pulse`, que solo se reescribe cuando cambia el controlador del contenedor | `XServerDisplayActivity.java:2796-2807` **[C]** |
| ALSA usa el servidor `aserver` de la app (`android_aserver.conf`, `libasound_module_pcm_android_aserver.so`) y `libasound.so.2` existe | listado de `imagefs` **[C]** |
| `libpulse.so.0` **no existe** en `imagefs`; solo `libpulse.so` | listado de `imagefs` **[C]** |

Diagnóstico ordenado de más a menos probable (todo **[I]** hasta tener registros):
1. **Dependencia.** `winepulse.so` del Proton instalado busca `libpulse.so.0` y no la encuentra, así que no hay sonido con PulseAudio ni PulseAudio-GN.
2. **Integración.** El servidor PulseAudio-GN no arranca o no abre AAudio en este teléfono (ColorOS 16).
3. **Configuración.** El registro de Wine no coincide con el controlador elegido; por ejemplo, un prefijo con `Audio=""`.

Prueba de un minuto: controlador **ALSA**. Prueba completa: registros de su Log Manager (canales `+pulse,+alsa,+mmdevapi`) y `logcat` del servidor.

### E. Compatibilidad

| API / caso | Lo que existe | Real o teórico |
|---|---|---|
| DirectX 8 | D8VK 1.0, `dd7to9` | Implementado **[C]**; probado en Mali **[D]** |
| DirectX 9/10/11 | DXVK 1.5.5–1.12.1 (+async, +sarek), 2.3.1; WineD3D vía Gladio (OpenGL ≤ 3.3) | Implementado **[C]**. DXVK 2.x no funciona en este Mali (falta robustness2), algo ya demostrado en nuestras pruebas **[C]** |
| DirectX 12 | vkd3d 2.8 / 2.14.1 (vkd3d-proton) | Incluido **[C]**; desactivado por defecto; en Mali es **teórico** **[D]** |
| DirectDraw / Glide | cnc-ddraw, nGlide | Incluido **[C]**. nGlide es freeware cerrado (ver licencias) |
| OpenGL | Gladio 1.1 (limitado a OpenGL 3.3) | **[C]** |
| 32 y 64 bits | WoW64 con FEX/WOWBox64 en ARM64EC; Box64 en x86_64 | **[C]** |
| Launchers y multiproceso | `WINE_NO_DUPLICATE_EXPLORER`, Bannerlator `wfm.exe`/`winhandler.exe` | **[C]**; GTA V con launcher llegó a jugarse según el usuario **[D]** |
| Controles | Táctil, giroscopio, mandos USB con vibración, evdev simulado | **[C]** |
| Resoluciones y escalado | Fit, Stretch, Fill, Integer y alineación | Notas 1.2.1 y código de pantalla **[C]** |

Un juego solo cuenta como compatible si hay una partida jugable registrada en el dispositivo, no porque su proceso aparezca en ejecución.

---

## 3. Comparación con LURVETA

| Componente | LURVETA (rama de trabajo) | Winlator Mali | Decisión |
|---|---|---|---|
| Wine | Wine 10.10 x86_64 incluido en el rootfs (glibc) | Proton 10/9 ARM64EC descargado de terceros | **Conservar** el de Mali, pero **fijar** una versión verificada e incluida en el APK |
| Emulación | Box64 0.4.4 sobre todo Wine | FEXCore 2605 / WOWBox64 0.4.3 (ARM64EC); Box64 0.4.3 como alternativa | **Conservar** el de Mali; **Investigar** FEX contra WOWBox64 por juego |
| Vulkan | Vortek (servidor en la app, socket y memoria compartida) | Wrapper en el mismo proceso | **Reemplazar** Vortek por el Wrapper. Vortek queda archivado, no se incluye |
| DXVK | DXVK 1.10.3 y 2.x (2.x inservible en Mali) | DXVK 1.7.2 por defecto y 1.5–2.3 | **Conservar**; **Optimizar** con pruebas por juego (1.10.3 contra 1.7.2) |
| Texturas BC | Decodificador propio en Vortek (BC1–BC7, BC6H/BC7, a RGBA), un decodificador por dispositivo | Capa BCn por GPU a ASTC/ETC2 con caché | **Conservar** el de Mali. Nuestro decodificador no aplica, salvo como referencia de calidad en las pruebas de BC6H/BC7 |
| Caché de shaders | Caché de pipelines persistente en Vortek | Caché de estado DXVK y caché BCn | **Investigar**: no tiene caché de pipelines del driver propia; **Integrar** una solo si la medición lo justifica |
| Audio | ALSA (aserver) | PulseAudio-GN por defecto, sin sonido en el teléfono del usuario | **Corregir** |
| Servidor X | Escritura no bloqueante con cola (`c55bc59`) | `write()` bloqueante (`cpp/winlator/xoutput_stream.c:52-55`) | **Integrar** nuestro arreglo |
| Límite de archivos | Sube `RLIMIT_NOFILE` y desactiva esync si no alcanza (`c76908a`) | No existe | **Integrar** |
| Diagnóstico de lanzamiento | `LaunchTrace`: etapas, procesos, CPU por hilo, E/S, archivos, diario Vulkan | Log Manager: registros de Wine/Box64/FEX/DXVK, logcat, motivos de salida (`logs/SessionLog.java`) | **Integrar** `LaunchTrace` sin la parte de Vortek, encima de su Log Manager |
| Fallos nativos | Capturador de fallos y `CrashCollector` (ApplicationExitInfo y tombstone) | Motivos de salida en `SessionLog` | **Integrar** el capturador nativo y el análisis de tombstones |
| Contenedores | `.container` JSON, `files/rtfs/home/xuser-N`, prefijo Wine 10.10 | `.container` JSON, `files/imagefs/home/xuser-N`, prefijo Proton ARM64EC | **Migrar** con herramienta (sección 6); los prefijos no son intercambiables |
| Interfaz | Rediseño gaming de LURVETA (en `main`) | Interfaz CMod/Mali con temas, Contents, Community Configs, Saves y Logs | **Reemplazar** la presentación por la nuestra y **conservar** sus funciones |
| Identidad | `com.lurveta.co`, firma LURVETA | `com.winlator.mali`, `com.ludashi.benchmark`, `com.antutu.ABenchMark` | **Conservar** la nuestra |
| Rendimiento del sistema | ADPF (PerfHint) en Vortek | Sustained Performance, GameManager, afinidad, RAM Booster | **Conservar** y **añadir** ADPF al hilo de presentación; **Investigar** el RAM Booster |
| Pruebas en el contenedor | BC, fences, vtio, servidor X, capturador de fallos, informe de lanzamiento | No hay | **Integrar** las que aplican (servidor X, capturador de fallos, informe de lanzamiento) |

Trabajo de la rama `ccr-964d8b63-2s3tyb` que **no se pierde**: se integra el que aplica y se archiva con su historia en git el que depende de Vortek, que no se borra.

| Integrar | Archivar (solo aplica a Vortek) |
|---|---|
| Servidor X no bloqueante | `fence_export` |
| `ProcessLimits` | `vtio` (reintento de señales en el cliente Vortek) |
| `LaunchTrace` (sin Vortek) | Decodificador por dispositivo |
| `CrashCollector` y capturador nativo | Diario de llamadas Vortek |
| Pruebas de host de lo anterior | Caché de pipelines de Vortek |

---

## 4. Inventario de componentes (APK y repositorio)

| Componente | Forma | Origen | Licencia | ¿Modificable? |
|---|---|---|---|---|
| App Java (`com.winlator.cmod`, ~63 k líneas) | fuente | brunodev85 → Bionic → CMod → Mali | MIT (`LICENSE`) | **Sí** |
| `cpp/winlator`, `gladiorenderer`, `adrenotools`, `patchelf`, `proot`, `zerofg` | fuente | varios | MIT; adrenotools BSD-2 **[I]**; patchelf GPL-3.0 **[I]**; proot GPL-2.0 (`COPYING`); ZeroFG Apache-2.0 (`LICENSE`) | Sí; patchelf y proot con obligaciones copyleft |
| `jniLibs/arm64-v8a` (12 .so: PulseAudio/GN, sndfile, ltdl, conscrypt, zstd-jni, fakeinput…) | **binario** | GameNative (`libgn_*`), Termux, Google | PulseAudio/sndfile LGPL-2.1 **[I]**; conscrypt Apache-2.0; zstd-jni BSD | Sustituibles por compilaciones propias |
| `libvulkan_wrapper.so` (5 variantes) | **binario** con símbolos | Mesa / "gamenative-wrapper" | MIT (Mesa) **[I]** | Sin fuente en el repositorio: **usar tal cual** hasta conseguir el origen |
| `libbcn_layer.so` (capa BCn modificada) | **binario** | leegao, modificada por Fcharan | **desconocida** | **Usar tal cual**; confirmar licencia antes de redistribuir |
| `libVkLayer_khronos_validation.so` | binario (21 MB) | Khronos | Apache-2.0 | **Quitar** de producción |
| Turnip / adrenotools (`turnip25.1.0`, `v819`) | binario | Mesa / adrenotools | MIT / BSD-2 | Solo Adreno: **no se usa en Mali** y LURVETA no depende de él. Se conserva hasta decidir si se elimina (regla: no eliminar componentes sin auditarlos) |
| DXVK, D8VK, vkd3d | binario (tzst) | doitsujin, AlpyneDreams, Hans-Kristian | zlib / zlib / LGPL-2.1 | Sustituibles por compilaciones oficiales |
| FEXCore, Box64, WOWBox64 | binario | FEX-Emu, ptitSeb | MIT | Sustituibles |
| cnc-ddraw, dd7to9 | binario | FunkyFr3sh, — | MIT / **[I]** | Sí |
| nGlide | binario | Zeus Software | **freeware cerrado** | Revisar si permite redistribución antes de incluirlo |
| `imagefs.tzst` (190 MB, sistema base Bionic) | binario, **descargado al compilar** | releases del autor | mezcla GPL/LGPL/MIT | Reproducible solo extrayéndolo; conviene **reconstruirlo** a medio plazo |
| Wine/Proton | **no incluido**: contenidos de terceros | Bannerlator, WinNative, StevenMXZ, Winlator-Extras | LGPL-2.1 | **Fijar** una compilación y publicar su fuente |
| `wfm.exe`, `winhandler.exe` | binario | Bannerlator | **[I]** | Sí |

Obligaciones al redistribuir (**[I]**, que debe confirmar una revisión legal):
- Componentes GPL y LGPL (Wine, PulseAudio, proot, patchelf, vkd3d): ofrecer su código fuente o un enlace exacto a la versión usada.
- Componentes MIT y BSD: conservar los avisos de copyright.
- Atribución en la pantalla "Acerca de": brunodev85, Pipetto-crypto, CMod, GunaCharanTeja, leegao, GameNative, Bannerlator.

---

## 5. Identidad e interfaz de LURVETA

- **Identidad:**
  - `applicationId "com.lurveta.co"`, el nombre y la firma de LURVETA, y una única variante.
  - **Sin** variantes Ludashi ni AnTuTu: hacerse pasar por otra app contradice tus reglas.
- **Paquete Java:** se mantiene `com.winlator.cmod` en el código (cambiar el `namespace` no es necesario para el `applicationId`), para poder seguir comparando con el original. Lo que tiene que ser LURVETA es lo que ve el usuario.
- **Interfaz, por etapas:**
  1. **Sistema de diseño.** Colores, tipografía, iconos, botones y tarjetas de nuestro rediseño de `main` (2b1a316) llevados a los temas de CMod, que ya separa temas y "preset surfaces".
  2. **Pantallas principales.** Inicio, Contenedores, Ajustes de contenedor y Biblioteca de accesos directos con nuestra estructura estilo consola.
  3. **Pantallas secundarias.** Contents, Community Configs, Saves y Logs con nuestro estilo, conservando sus funciones.
  4. **Durante el juego.** El menú lateral y el HUD con nuestra marca.
- **Funciones propias que se recuperan:** el informe de lanzamiento con su aviso y el capturador de fallos (sección 3).

---

## 6. Migración de contenedores y datos

Hechos **[C]**:
- Los dos usan `home/xuser-N/.container` en JSON con claves en gran parte comunes (`name`, `screenSize`, `envVars`, `cpuList`, `graphicsDriver`, `dxwrapper`, `dxwrapperConfig`, `wincomponents`, `drives`, `box64Preset`, `audioDriver`, `wineVersion`…).
- Winlator Mali añade `emulator`, `fexcorePreset`, `fexcoreVersion`, `box64Version`, `ramBooster*` y `midiSoundFont`.
- Las raíces difieren: `files/rtfs` en LURVETA frente a `files/imagefs` en Winlator Mali.
- **Los prefijos no son compatibles**: Wine 10.10 x86_64 frente a Proton ARM64EC. Las DLL de `system32` son de arquitecturas distintas.

Procedimiento propuesto:
1. **Copia de seguridad primero.** Exportar cada contenedor a `Download/LURVETA/backup/<fecha>/` en tar.zst con un manifiesto SHA-256. La migración no empieza si la copia falla.
2. **Nuevo contenedor.** Se crea uno ARM64EC nuevo; el prefijo nunca se copia.
3. **Ajustes.** Se trasladan los compatibles: `name`, `screenSize`, `cpuList`, `envVars` (filtrando `BOX64_*`), `drives`, `dxwrapperConfig` con DXVK 1.10.3 o 1.7.2, `audioDriver`. Los incompatibles se traducen: `vortek` pasa a `wrapper`, el preset de Box64 pasa a FEX `INTERMEDIATE`.
4. **Datos de usuario.** Se copian `drive_c/users/xuser/*` (partidas guardadas, AppData y Documents) y las carpetas de juegos instalados dentro de `drive_c`. Los juegos en almacenamiento externo (`D:`) no se tocan.
5. **Accesos directos.** Se reescribe la ruta del `.desktop` al nuevo contenedor y se conservan sus extras.
6. **Reversible.** El contenedor original queda intacto y marcado como "migrado", nunca se borra automáticamente. Hay un botón "Restaurar desde copia".
7. **Desde Winlator Mali.** Es otro `applicationId`, así que Android no deja leer sus datos directamente. La vía es exportar a `Download` desde Winlator Mali, o pedir al usuario que copie la carpeta del juego. Hay que **medir** qué se puede leer sin root en Android 16.

Riesgo: una actualización encima con el mismo `applicationId` conserva `files/` de la LURVETA anterior; el primer arranque debe detectar `files/rtfs` y ofrecer la migración sin borrar nada.

---

## 7. Arquitectura propuesta para LURVETA Performance 2.0

```
LURVETA UI (rediseño gaming)  ──  servicios LURVETA: LaunchTrace, CrashCollector, migración, perfiles
        │
Núcleo Winlator Mali/CMod (Java + cpp/winlator)
  - servidor X con escritura no bloqueante (portado de LURVETA)
  - ProcessLimits: RLIMIT_NOFILE y protección de esync (portado)
  - componentes: PulseAudio/ALSA, SysVSHM, X server, RAM Booster (bajo evaluación)
        │
Invitado (imagefs Bionic)
  - Proton ARM64EC FIJADO por LURVETA (incluido en el APK, con fuente publicada)
  - FEXCore / WOWBox64 (32 bits) · Box64 solo para Wine x86_64
  - DXVK 1.10.3 / 1.7.2 · Gladio · vkd3d (experimental)
  - Vulkan: winevulkan → Wrapper (binario) → capa BCn (binario) → driver Mali
```

---

## 8. Hoja de ruta (cada etapa = un APK de prueba y una validación en el dispositivo)

| Etapa | Contenido | Clasificación | Verificación |
|---|---|---|---|
| **0. Línea base** | Registrar en el teléfono la configuración exacta con la que GTA V va a ~30 FPS (Proton, FEX/Box64, Wrapper, BCn, DXVK, resolución). Medir la variante Ludashi contra `com.winlator.mali` (misma configuración) durante 15/30/45 min: FPS medio, 1 % low, temperatura y consumo | Investigar | Tabla de mediciones firmada con fecha; sin cambios de código |
| **1. Importar la base** | Nueva rama `lurveta-mali-base` desde el commit `0411a4b` con su historia; `applicationId com.lurveta.co`, firma LURVETA, una sola variante; nombre "LURVETA Performance" | Conservar | El APK instala y arranca; GTA V con la configuración de la etapa 0 da los mismos FPS que la línea base, dentro del margen de ruido |
| **2. Fijar componentes** | Incluir en el APK un Proton ARM64EC concreto con su enlace de fuente; quitar la capa de validación (21 MB); inventario de licencias en "Acerca de" | Corregir / Optimizar | Instalación sin red; tamaño del APK; los FPS no cambian |
| **3. Estabilidad (portar LURVETA)** | Servidor X no bloqueante; `ProcessLimits`; capturador nativo y `CrashCollector`; `LaunchTrace` sin Vortek, encima del Log Manager | Integrar | Pruebas de host; GTA V 45 min sin ANR ni carga infinita; los informes salen en `Download/LURVETA/logs` |
| **4. Audio** | 1) Probar ALSA. 2) Si PulseAudio falla por `libpulse.so.0`, crear el enlace en el imagefs o reenlazar `winepulse`. 3) Medir latencia y huecos en 3 juegos (GTA V, GTA IV y uno DX9) | Corregir | Sonido continuo sin cortes; latencia percibida; no rompe el audio de los otros juegos |
| **5. Interfaz LURVETA** | Sistema de diseño y pantallas principales (sección 5) | Reemplazar presentación | Contraste visual con `main`; todas las funciones de CMod siguen accesibles |
| **6. Migración** | Copia de seguridad, migrador y restaurar (sección 6) | Integrar | Prueba con un contenedor de la LURVETA anterior: partidas guardadas intactas, original sin tocar |
| **7. Rendimiento sostenido** | Por separado y reversibles: ADPF en el hilo de presentación; caché BCn precalentada; revisar el coste de `readback` y FSR; afinidad de FEX en núcleos A715; RAM Booster con o sin él | Optimizar / Investigar | Cada cambio con prueba A/B de 15/30/45 min y su tabla. Objetivo de +5–10 FPS **solo si las mediciones muestran margen** |
| **8. Compatibilidad** | Matriz de juegos (DX9/10/11/12, 32/64 bits, con launcher), DXVK 1.10.3 frente a 1.7.2 por juego | Investigar | "Compatible" solo con partida jugable registrada |

Regla de las etapas: **un solo componente crítico por APK**. Cada APK lleva su commit en la cabecera del informe y se puede volver al anterior.

---

## 9. Plan de pruebas en el dispositivo (OPPO CPH2737, Dimensity 8350, Mali-G615 MC6)

- **Condiciones:** batería por encima del 60 %, sin cargador, brillo fijo, temperatura ambiente anotada y 10 min de reposo antes de cada prueba. Mismo punto de partida guardado del juego.
- **Métricas:**
  - FPS medio, 1 % y 0,1 % low, y frametime (HUD de DXVK o del Wrapper).
  - Temperatura de batería y de CPU (`HudDataSource`, `/sys/class/thermal`).
  - Consumo en vatios (HUD) y RAM (RSS del juego).
  - Tiempo de carga y tirones por compilación de shaders (picos de más de 50 ms en el frametime).
- **Duración:** minuto 0–5 (inicial), 15, 30 y 45 (sostenido). Así se distingue una bajada de FPS por temperatura de un límite del motor.
- **Comparación:** cada etapa contra la etapa 0 y contra la anterior, con la misma configuración. Se anota el commit del APK.
- **Registros:** `launch-*.txt`, `crash-*.txt`, el Log Manager y capturas del HUD. Nada se valida en lavapipe ni en un emulador de PC.

---

## 10. Registro de decisiones

| # | Decisión | Motivo | Fecha |
|---|---|---|---|
| D1 | Winlator Mali es la base principal (decisión del usuario) | GTA V jugable a ~30 FPS en el dispositivo real | 2026-11-23 |
| D2 | Vortek se archiva, no se borra | El Wrapper en el mismo proceso lo sustituye; su historia queda en git | propuesta |
| D3 | `applicationId com.lurveta.co` y una sola variante, sin Ludashi ni AnTuTu | Reglas de identidad del proyecto | propuesta |
| D4 | Proton fijado e incluido en el APK | Los contenidos de terceros no están versionados ni auditados | propuesta |
| D5 | Se portan el servidor X no bloqueante y `ProcessLimits` | Mismos defectos presentes en Winlator Mali (sección 3) | propuesta |
