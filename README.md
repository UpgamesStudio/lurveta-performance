<p align="center">
  <img src="logo.png" width="376" height="128" alt="LURVETA Performance" />
</p>

# LURVETA Performance

Ejecuta aplicaciones y juegos de Windows (x86‑64) en Android ARM64 con Wine + Box64.
Está optimizado para **MediaTek Dimensity** con **GPU ARM Mali** y toma como hardware de referencia el **Dimensity 8350 con Mali‑G615 MC6**.

- **applicationId:** `com.lurveta.co`. Se instala **junto a** Winlator original, sin compartir datos.
- **Base técnica:** Winlator 11.2 (`winlator-app@a030f55`, el código del APK oficial 11.2).
- **Licencia:** GNU LGPL‑2.1 (ver [`LICENSE`](LICENSE) y [`NOTICE.md`](NOTICE.md)).

> LURVETA Performance es un producto de LURVETA **basado en Winlator**, de BrunoSX
> (<https://github.com/brunodev85/winlator>). Se conservan todos los avisos de copyright y las licencias del proyecto original y de sus componentes.

## Qué aporta frente a Winlator 11.2

| Área | Cambio | Estado |
|---|---|---|
| Identidad | Paquete propio, rootfs reubicado con sustitución binaria de igual longitud, icono/tema/inicio LURVETA | Verificado (análisis estático del APK y prueba en host) |
| Texturas BC en Mali | Subidas por lotes (1 submit en lugar de submit + espera por región), caché corregida, sRGB correcto, BC4→R8, BC5→RG8 (canales y SNORM corregidos) | Verificado con pruebas en host (lavapipe, ASan/UBSan). **Impacto en FPS sin medir aún en el dispositivo** |
| Componentes | Retirados Turnip y libadrenotools (exclusivos de Adreno) | APK 2,98 MB más pequeño |
| Detección | GPU leída del driver Vulkan (Mali / Immortalis / Adreno / PowerVR / Xclipse) y clústeres de CPU | Verificado en host |
| Diagnóstico | HUD con 1 %/0,1 % low, frametime, temperatura y estado térmico; benchmark a CSV desde el menú del juego | Implementado |
| Sostenido | Prioridad del hilo de Vortek y ADPF (Android 13+), con interruptor | Implementado. **Efecto por medir** |
| Perfiles | Rendimiento (Mali) / Equilibrado / Compatibilidad / Estabilidad por juego, aplicados solo por el usuario | Implementado |

Los detalles están en [`CHANGELOG.md`](CHANGELOG.md), el análisis técnico en [`docs/AUDITORIA_Y_PLAN.md`](docs/AUDITORIA_Y_PLAN.md) y las limitaciones conocidas en [`docs/LIMITACIONES.md`](docs/LIMITACIONES.md).

## Documentación
- [Instalación y compilación](docs/INSTALACION.md)
- [Metodología de benchmarks](docs/BENCHMARKING.md) y resultados
- [Componentes eliminados y conservados, y tamaño](docs/COMPONENTES.md)
- [Limitaciones conocidas](docs/LIMITACIONES.md)

## Estructura
- `app/`: aplicación Android (Java, C/NDK). Vortek (servidor), Gladio, VirGL y el resto del código nativo están en `app/src/main/cpp`.
- `tools/host-tests/`: pruebas reproducibles en Linux (decodificador BC, decodificador de texturas sobre Vulkan, relocalizador de rutas, estadísticas de fotogramas, detección de GPU).
- `tools/bench/`: captura por adb y análisis A/B de benchmarks.

El primer commit del repositorio (tag `upstream-winlator-11.2`) es una **copia limpia sin modificar** de la base original.
