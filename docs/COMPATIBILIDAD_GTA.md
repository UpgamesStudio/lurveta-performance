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
