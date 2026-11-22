# Limitaciones técnicas conocidas

## No verificado en el dispositivo
- **Rendimiento:** ningún efecto en FPS, tirones, temperatura o consumo está medido en el Dimensity 8350. Ver `BENCHMARKING.md`.
- **Instalación y primer arranque:** el APK se ha validado de forma estática (manifest, authorities, paquete, rutas compiladas) y el relocalizador con pruebas en host sobre los assets reales. La instalación y el primer arranque en un teléfono real **no se han probado aquí**.
- **Driver Mali sin BC:** que el driver no exponga `textureCompressionBC` se deduce de la documentación de Mali. LURVETA lo comprueba en tiempo de ejecución ("Acerca de" › Hardware detectado) y solo activa el decodificador si falta.
- **ADPF y prioridad:** el sistema puede rechazar el nice −4 (se mantiene la prioridad por defecto) y ADPF solo existe en Android 13+. Su efecto real depende del gobernador de MediaTek.

## Limitaciones de arquitectura (heredadas)
- **Vortek serializa las llamadas Vulkan:** el cliente usa un mutex global y el servidor un solo hilo. Rediseñarlo es de alto riesgo y queda fuera de esta versión.
- **Swapchain de un solo búfer sin esperar los semáforos de presentación** (posible tearing). La corrección (P7) está aplazada, porque requiere cambiar el protocolo y recompilar el ICD del lado del juego.
- **Esperas activas en los ring buffers** (consumo). La corrección con futex (P6) está aplazada por la misma razón.
- **Copias de la caché:** la carga de texturas sigue haciéndose antes del submit del juego, en el mismo hilo. Ahora es una sola subida por submit, pero la decodificación en CPU de texturas nuevas sigue costando tiempo de CPU.
- **Usuario principal:** solo funciona en el usuario principal de Android, por la ruta `/data/data` compilada en los binarios del rootfs.
- **Formatos sin decodificador:** BC6H y BC7 no tienen decodificador por software, igual que en la 11.2. En Mali esas texturas no se pueden usar.
- **Imágenes BC5 con vistas UNORM y SNORM a la vez** (formatos typeless): los bytes decodificados se interpretan con el formato de la vista. Es un caso raro.

## Fuera del alcance de esta versión
- Recompilar Wine, DXVK, VKD3D, Mesa o Box64. Se usan los binarios de la 11.2 y se seleccionan y configuran, no se modifican.
- Mejorar el driver Mali (es cerrado), forzar frecuencias o voltajes, o saltarse la protección térmica.
- Compatibilidad garantizada con AAA modernos: depende de Box64, Wine, DXVK y el driver Mali. GTA V mantiene el límite `dxgi.maxDeviceMemory = 512` del `dxvk.conf` original, porque subirlo sin pruebas puede provocar falta de memoria.
- Traducción al español de la interfaz: los textos nuevos están en inglés, como el resto de la interfaz original (en/pt/ru).

## Distribución (LGPL‑2.1)
Si distribuyes el APK debes ofrecer el código fuente correspondiente. El repositorio `lurveta-performance` se creó privado, así que habría que hacerlo público o entregar el código por otro medio. El enlace de "Acerca de" apunta a `BuildConfig.SOURCE_URL`.
