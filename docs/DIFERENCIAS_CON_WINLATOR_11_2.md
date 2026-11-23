# Diferencias con Winlator 11.2 en el camino de arranque

Comparación hecha sobre el código: el primer commit del repositorio (`1dc6101`) es la importación sin modificar de Winlator 11.2; se compara con la rama actual. **Es análisis de código, no una prueba en el teléfono.**

## Lo que NO cambió
- `GuestProgramLauncherComponent` (el comando `box64 wine explorer …`, `PATH`, `LD_LIBRARY_PATH`, `BOX64_*`, presets de Box64): idéntico a 11.2.
- Construcción del comando de Wine (`getWineStartCommand`) y variables `WINEPREFIX`, `WINEESYNC`, `MESA_*`: idénticas.
- Los paquetes de Box64 0.4.4, Wine, DXVK 1.10.3/2.4.1, VKD3D y wincomponents: mismo contenido (solo cambia la ruta del rootfs, ver abajo).

## Lo que SÍ cambió en el camino de arranque
| Cambio | Origen | Efecto posible en el arranque | Estado |
|---|---|---|---|
| Rootfs en `files/rtfs`, ruta binaria reubicada (`com.winlator`→`com.lurveta.co`, misma longitud) | 1.0.0 (P0) | Si quedase una ruta sin reubicar, Wine/Box64 no encontrarían sus archivos | **Verificado completo**: 457 reubicaciones y 0 restos en los 24 paquetes; 0 rutas antiguas en Java/nativo/recursos |
| Turnip y libadrenotools retirados; «turnip» migra a Vortek | 1.0.0 (P0) | Contenedores que usaban Turnip pasan a Vortek | Sin efecto en Mali (Turnip no funciona en Mali) |
| `TU_DEBUG=noconform` retirado de las variables por defecto | 1.0.0 (P0) | Ninguno en Mali (solo afecta a Turnip) | Sin efecto |
| HUD (FrameRating) se crea siempre, también con el HUD desactivado | 1.0.0 (P3) | Un fallo al crearlo afectaría a **todos** los juegos | Sin evidencia de fallo; a comprobar en el log |
| Vortek: decodificador BC, subidas, caché de pipelines, `DEVICE_LOST`, BC6H/BC7 | 1.0.0 / 1.1.0 | Posible, afecta a juegos con texturas BC | Aislable con 3 interruptores (ver guía) |
| Rediseño visual (solo recursos) | 1.1.0 | No afecta a la ejecución | — |

## El hueco de diagnóstico (heredado de 11.2)
Winlator 11.2 y las versiones anteriores de LURVETA tienen estos comportamientos, que **ocultan** por qué un juego no arranca:
1. `ProcessHelper.exec` ignora cualquier excepción al lanzar el proceso.
2. Sin depuración activada, la salida de Wine y Box64 va a `/dev/null`.
3. Al terminar el proceso la app vuelve al menú **sin decir nada**, sea cual sea el código de salida.

Por eso «no inicia» no distinguía entre «Box64 no arrancó», «Wine falló», «el juego no se creó» o «Vulkan falló».

## Cómo se cierra esa brecha (esta rama)
Captura de arranque: ver `docs/PRUEBAS_GTA.md`. Cada intento genera `Download/LURVETA/logs/launch-latest.txt` con la última etapa alcanzada, el código de salida y la salida de Box64/Wine/DXVK/Vortek.
