# Prueba controlada en el teléfono: GTA IV y GTA V

Objetivo: **encontrar qué cambio rompe el arranque**, con evidencia, en pocos pasos. No hace falta saber programar: solo repetir y enviar los archivos indicados.

## Preparación (una vez)
1. Instala el APK de prueba `1.1.1-dev` (mismo paquete `com.lurveta.co`).
2. Contenedor nuevo para GTA (no toques los que ya tienes) con **Vortek** como controlador gráfico.
3. En el contenedor › Variables de entorno añade `DXVK_LOG_LEVEL=info` (y, si quieres, `DXVK_HUD=fps,frametimes,memory`).
4. Ajustes › Wine › activa la depuración con el canal `err` (y deja el archivo de log por defecto).
5. Cierra todas las apps y deja enfriar el teléfono. Sin cargador, mismo brillo.

## Matriz de arranque (GTA IV primero, luego GTA V)
Para cada fila, cambia **solo** lo indicado en Vortek › Configuración, arranca el juego, espera 3 minutos o hasta el fallo, y anota el resultado.

| # | Subida BC | Caché de pipelines | BC6H/BC7 | Qué comparar |
|---|---|---|---|---|
| 1 | En línea | activada | activado | configuración por defecto de 1.1.x |
| 2 | En línea | **desactivada** | activado | ¿influye la caché? |
| 3 | En línea | activada | **desactivado** | ¿influye BC6H/BC7? (comportamiento de 1.0.0) |
| 4 | En línea | **desactivada** | **desactivado** | 1.1.x lo más parecido a 1.0.0 |
| 5 | **Síncrono (Winlator 11.2)** | desactivada | desactivado | método más conservador |
| 6 | Por lotes (1.0) | desactivada | desactivado | para comparar con 1.0.0 |

Anota por fila: ¿abre ventana? ¿llega al menú? ¿carga una partida? ¿se ve bien (vehículos, peatones)? ¿se cuelga y a los cuántos segundos?

**Interpretación (solo si se repite en 2 intentos):** si el resultado cambia entre dos filas, el interruptor que difiere es el responsable. Si **todas** fallan igual, la causa no está en esos tres cambios y hay que mirar la configuración del contenedor, el rootfs o Wine/Box64 con los logs.

## Comparación con Winlator 11.2 y con 1.0.0
Mismo contenedor y ajustes del juego. Si tienes 1.0.0 o Winlator 11.2 instalados, repite la fila 5 en ellos y anota si el juego arranca ahí. Es la prueba más útil: dice si la regresión es de LURVETA o de la configuración.

## Qué enviar
- `files/logs/vortek-diagnostics.log` y `files/logs/vortek-errors.log` (si existen) — contienen las llamadas Vulkan que fallaron y el tope de memoria.
- El log de Wine y los `*_d3d9.log` / `*_d3d11.log` de DXVK (junto al `.exe` del juego).
- La tabla de arriba rellenada y el modelo exacto/versión de Android.
- Si grabas con el benchmark: la carpeta de `Download/LURVETA/benchmarks/`.

## Qué buscar en `vortek-diagnostics.log`
- `FAIL vkAllocateMemory ... requested X MB, already allocated Y MB, cap Z MB` → falta de memoria (hipótesis de GTA V). Se puede subir el tope en Vortek › memoria máxima del dispositivo (con prudencia: es memoria compartida con Android).
- `FAIL vkCreateImage(decoded BC) ... format ...` → formato BC que no se pudo crear.
- `FAIL vkCreate*Pipelines` → fallo de shaders o de caché.
- Ninguna línea `FAIL` y el juego sigue sin arrancar → el problema no está en Vortek.
