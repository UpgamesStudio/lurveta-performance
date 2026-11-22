# Componentes eliminados y conservados

## Eliminados (exclusivos de Qualcomm Adreno / Snapdragon)
| Componente | Tamaño en APK (comprimido) | Motivo |
|---|---:|---|
| `assets/graphics_driver/turnip-26.2.0.tzst` (Mesa Turnip) | 2.655.341 B | Driver Vulkan de código abierto solo para Adreno; no funciona en Mali |
| `libmain_hook.so`, `libhook_impl.so`, `libfile_redirect_hook.so`, `libgsl_alloc_hook.so` (libadrenotools) | ≈216 KB (con el código nativo asociado) | Solo sirven para cargar drivers Adreno personalizados |
| `TurnipConfigDialog`, layout, tipo de componente TURNIP / ADRENOTOOLS_DRIVER, opción "adrenotools driver" de Vortek, `TU_DEBUG=noconform` | código/recursos | Dependen de lo anterior |

## Conservados
| Componente | Motivo |
|---|---|
| Vortek (servidor en la app + ICD en el rootfs) | Ruta Vulkan principal en Mali: usa el driver del sistema |
| Gladio | OpenGL → GLES, independiente del fabricante |
| Zink, VirGL (Mesa) | OpenGL sobre Vulkan/GLES: funcionan en Mali |
| DXVK 1.10.3 / 2.4.1, D8VK, D7VK, VKD3D 2.14.1, cnc‑ddraw, WineD3D | Rutas D3D independientes del fabricante (VKD3D/D3D12 depende de las capacidades del driver Mali) |
| Box64 0.4.4, Wine, rootfs glibc, PulseAudio/ALSA, FluidSynth | Comunes en ARM64 |

## Tamaño (medido)
| | Winlator 11.2 oficial | LURVETA Performance 1.0.0 | Diferencia |
|---|---:|---:|---:|
| APK | 157.549.075 B (150,25 MiB) | 154.571.955 B (147,41 MiB) | **−2.977.120 B (−1,89 %)** |
| Contenido sin comprimir | 168.606.726 B | 165.210.257 B | −3.396.469 B |

Desglose del APK comprimido: graphics_driver −2.655.341 B, bibliotecas nativas −216.285 B, dex −30.199 B, recursos −64.341 B, fondos −18.750 B, licencias +10.827 B.

El rootfs extraído en el dispositivo (unos 400 MB) tiene el mismo tamaño que en Winlator 11.2.
