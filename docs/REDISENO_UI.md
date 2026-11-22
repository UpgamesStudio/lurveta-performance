# Rediseño visual de LURVETA Performance

Este documento recoge las cinco fases del rediseño visual: auditoría, sistema de diseño, implementación, validación y entrega.

**Regla del proyecto:** solo se cambia la presentación. La lógica, la emulación, los contenedores, Wine/Box64/DXVK/Vulkan/Vortek, los archivos, las preferencias y la navegación quedan como estaban. Cuando un componente no se podía rediseñar sin riesgo, se cambiaron solo los estilos que lo rodean.

---

## Fase 1 — Auditoría

### 1.1 Arquitectura de la interfaz

- **Vistas clásicas de Android:** XML, AppCompat 1.4.0 y Material 1.4.0. No usa Compose. Tiene 4 actividades, 6 fragmentos y 18 diálogos `ContentDialog`, además de diálogos propios (`TaskManagerDialog`, `PreloaderDialog`, `DownloadProgressDialog`, `Box64EditPresetDialog`).
- **Tema:** `AppThemeDark` (por defecto) o `AppThemeLight`, según la preferencia `app_theme`, que se aplica en `AppUtils.setActivityTheme`. Los colores salen de atributos propios (`colorPrimaryText`, `colorSecondarySurface`, `popupMenuBackground`, `toolboxBackground`, `preloaderBackground`…).
- **Aspecto heredado de Winlator:** botones, combos, campos, diálogos, menús emergentes, *toolbox*, interruptores y *spinner* de carga eran PNG 9‑patch fijos, y los iconos eran PNG negros teñidos por código o por tema.

### 1.2 Inventario de pantallas

| # | Pantalla | Archivos | Clase | Función que debe conservarse |
|---|---|---|---|---|
| 1 | Inicio (cajón de navegación + barra) | `main_activity`, `main_menu_header`, `menu/main_menu` | `MainActivity` | Navega a Accesos directos, Contenedores, Controles, Ajustes y Acerca de. Recuerda `show_shortcuts_first`, pide permisos e instala el rootfs |
| 2 | Biblioteca de juegos (accesos directos) | `base_file_manager_fragment`, `file_grid_item`, `file_list_item`, `menu/shortcuts_menu`, `menu/file_manager_popup_menu` | `ShortcutsFragment` | Rejilla o lista (`shortcuts_view_style`), carpetas, crear carpeta, copiar, cortar y pegar, eliminar, ajustes del acceso directo y **ejecutar el juego** (`XServerDisplayActivity` + `shortcut_path`) |
| 3 | Contenedores | `containers_fragment`, `container_list_item`, `menu/containers_menu`, `menu/container_popup_menu` | `ContainersFragment` | Crear, **ejecutar**, gestor de archivos, editar, duplicar, eliminar e información de almacenamiento |
| 4 | Crear / editar contenedor | `container_detail_fragment`, `wincomponent_list_item`, `drive_list_item`, `tagged_selection_box`, `menu/drive_search_popup_menu` | `ContainerDetailFragment`, `GraphicsDriverPicker`, `DXWrapperPicker` | Nombre, resolución, versión de Wine, controlador gráfico (Vortek/VirGL…), DXVK/WineD3D/VKD3D, audio, HUD y cinco pestañas: Wine, componentes, variables, unidades y avanzado (preset de Box64, arranque, versión de Windows, afinidad de CPU) |
| 5 | Gestor de archivos del contenedor | `base_file_manager_fragment`, `file_*_item`, `menu/container_file_manager_menu` | `ContainerFileManagerFragment` | Navegar por las unidades, abrir, favoritos, renombrar, información, crear acceso directo, etc. |
| 6 | Controles de entrada | `input_controls_fragment`, `external_controller_list_item`, `menu/open_file_popup_menu` | `InputControlsFragment` | Perfiles (añadir, editar, duplicar, borrar, importar, exportar), velocidad del cursor, opacidad, editor y mandos externos |
| 7 | Editor de controles | `controls_editor_activity`, `control_element_settings`, `binding_field` | `ControlsEditorActivity` | Barra flotante (mover, añadir, eliminar, ajustes) y ajustes de cada elemento |
| 8 | Asignaciones de mando externo | `external_controller_bindings_activity`, `external_controller_binding_list_item` | `ExternalControllerBindingsActivity` | Lista de asignaciones resaltada según la entrada del mando |
| 9 | Ajustes | `settings_fragment`, `wine_debug_channel_list_item`, `wine_install_dialog` | `SettingsFragment`, `GeneralComponents` | Ratón, mandos y jugadores, MIDI/SoundFont, idioma, tema claro/oscuro, protección en segundo plano, versión y presets de Box64 (*toolbox* con tags `install`/`remove`), depuración de Wine, logs, Wine y reinstalar archivos del sistema |
| 10 | En juego (menú lateral) | `xserver_display_activity`, `menu/xserver_menu` | `XServerDisplayActivity` | Teclado, controles, pantalla completa, administrador de tareas, ventanas activas, lupa, efectos, PiP, logs, **benchmark**, ayuda del touchpad y salir |
| 11 | HUD de rendimiento | `frame_rating` | `FrameRating` | FPS, 1 %/0,1 % low, frametime, GPU, RAM, CPU, temperatura e indicador REC |
| 12 | Acerca de | `about_dialog` | `AboutDialog` | Versión, enlace al código, **hardware detectado** (GPU Vulkan, CPU) y créditos y licencias |
| 13 | Diálogos de configuración gráfica | `vortek_config_dialog`, `dxvk_config_dialog`, `vkd3d_config_dialog`, `wined3d_config_dialog`, `virgl_config_dialog` | `*ConfigDialog` | Extensiones Vulkan, memoria, caché BC, *performance hints* (ADPF), versión de DXVK, límite de FPS, etc. |
| 14 | Otros diálogos | `content_dialog` (base), `audio_driver_config_dialog`, `add_env_var_dialog`, `box64_edit_preset_dialog`, `box64_env_var_list_item`, `container_storage_info_dialog`, `create_folder_dialog`, `debug_dialog`, `debug_toolbar`, `file_info_dialog`, `gamepad_player_config_dialog`, `screen_effect_dialog`, `shortcut_settings_dialog`, `soundfont_test_dialog`, `touchpad_help_dialog`, `active_windows_dialog`, `task_manager_*`, `cpu_list_dialog`, `process_info_list_item`, `input_controls_dialog`, `download_progress_dialog` | Clases homónimas | Confirmar, cancelar o restablecer, prompts y listas de selección única o múltiple |
| 15 | Estados de carga y vacíos | `preloader_dialog`, `TVEmptyText` (en 4 pantallas), `custom_toast`, `lurveta_splash` | `PreloaderDialog`, `AppUtils.showToast` | Bloqueo durante tareas largas, mensajes y pantalla de inicio |
| 16 | Componentes reutilizables | `number_picker`, `cpu_list_item`, `env_vars_list_item`, `image_picker_*`, `magnifier_view`, `simple_list_item_*` | `widget/*` | Selectores numéricos, afinidad de CPU, variables de entorno, selector de imagen y color, lupa |

En total hay **64 layouts, 353 ids de vista, 11 menús y 50 entradas de menú** (ver `tools/ui-audit/contract_before.json`). `installed_wine_list_item.xml` no se usa en el código y se conserva igualmente.

### 1.3 Dependencias estructurales (lo que el rediseño **no** puede tocar)

Código que depende de la jerarquía, además de los ids:

| Dónde | Dependencia | Consecuencia para el diseño |
|---|---|---|
| `FrameRating` | `LL*Panel.getChildAt(0)` es la etiqueta y `getChildAt(1)` el valor | Cada panel del HUD conserva exactamente 2 `TextView` en ese orden |
| `SettingsFragment` | `LLGamepadPlayer` solo contiene los 4 `ImageView` de jugador | No se añaden hijos decorativos |
| `ContainerDetailFragment.getWinComponents` | Recoge **todos** los `Spinner` de `LLTabWinComponents` | No se añaden *spinners* en esa pestaña |
| `GraphicsDriverPicker`, `DXWrapperPicker` | Los hijos de `LLGraphicsDriver`/`LLDXWrapper` son `TaggedSelectionBox` | Se rellenan por código; el XML no añade hijos |
| `GeneralComponents` | `findViewWithTag("install"/"remove")` en `SoundFontToolbox` y `Box64Toolbox` | Se conservan los tags |
| `ControlsEditorActivity` | `toolbox.getParent()` es el `FrameLayout` raíz (`FLContainer`) | La barra sigue siendo hija directa |
| `CPUListView` | `findViewWithTag("CPU"+i)` | Se conservan los tags |
| Fragmentos | Hacen *cast* de la raíz inflada (`FrameLayout`, `ScrollView`…) | Las raíces mantienen su clase |
| `ColorPickerView`, `ImagePickerView`, `GLRenderer` | `BitmapFactory.decodeResource` de `color_frame*`, `icon_image_picker` y `cursor` | Esos 4 recursos siguen siendo PNG |
| `ShortcutsFragment` y otros | `setImageResource(R.drawable.icon_run / icon_open / container_*)` | Se conservan los nombres de los recursos (se puede cambiar su dibujo) |

### 1.4 Funcionalidades que deben conservarse

1. Ejecutar un contenedor (`container_id`) y un juego desde un acceso directo (`shortcut_path`).
2. Crear, editar, duplicar y eliminar contenedores. Ver su información de almacenamiento y abrir su gestor de archivos.
3. Guardar toda la configuración del contenedor: gráficos, DX wrapper, audio, HUD, componentes, variables, unidades, Box64, versión de Windows y CPU.
4. Ajustes del acceso directo, incluido el **perfil LURVETA** por juego.
5. Configuración de Vortek, VirGL, DXVK, VKD3D y WineD3D.
6. Ajustes globales con guardado en `SharedPreferences` (botón confirmar) y reinicio cuando cambian idioma o tema.
7. Perfiles y editor de controles, y mandos externos.
8. Menú en juego: benchmark CSV, HUD, administrador de tareas, lupa, efectos de pantalla, PiP, logs y salir.
9. Hardware detectado y créditos en "Acerca de".
10. Instalación del rootfs con su pantalla de carga, permisos y avisos (toasts).

---

## Fase 2 — Sistema de diseño

Inspiración: interfaces de consola actuales (fondos casi negros, superficies grafito por capas, un único color de acento y foco muy visible), con identidad propia de LURVETA. No reproduce recursos, iconos ni composiciones de ninguna consola.

### 2.1 Tokens de color (`res/values/colors.xml`)

| Token | Oscuro (por defecto) | Claro | Uso |
|---|---|---|---|
| Fondo | `lv_d_bg` `#07090D` | `lv_l_bg` `#F1F3F6` | Ventanas y listas |
| Superficie 1–4 | `#0F141B` → `#232D3A` | `#FFFFFF` → `#DEE4EB` | Tarjetas, paneles, campos, estados pulsados |
| Borde / borde fuerte | `#1F2833` / `#2D3948` | `#DCE2E9` / `#C6CFDA` | Separadores, contorno de campos |
| Texto 1 / 2 / 3 | `#EDF2F7` / `#8D9AAA` / `#5A6676` | `#0D131A` / `#556172` / `#8D97A5` | Títulos, etiquetas, ayudas |
| Acento | `lv_cyan` `#00C8F5` (pulsado `#00A2C7`) | igual | Acción principal, foco, selección, indicador de pestaña |
| Estado | éxito `#2BD9A0`, aviso `#FFB547`, peligro `#FF5A6A` | igual | Mensajes y acciones destructivas |
| Cabecera de marca | `lv_header` `#07090D` | `#0F151D` | Barra superior y cabecera del cajón (oscuras en los dos temas) |
| Superposición en juego | `lv_overlay` `#B3070A0F` | igual | HUD y menús sobre el juego |

Los colores se consumen a través de atributos de tema (`res/values/attrs.xml`, prefijo `lv*`), de modo que `AppThemeDark` y `AppThemeLight` cambian todo el aspecto sin tocar layouts ni código.

### 2.2 Forma, tipografía y espaciado

- Radios: 12 dp en tarjetas y diálogos, 10 dp en campos y botones, 8 dp en chips, 999 dp en el botón de ejecutar.
- Tipografía del sistema (sin fuentes añadidas). Títulos 18–20 sp en negrita, cuerpo 14–15 sp y etiquetas de campo 12 sp en mayúsculas con espaciado (`LvFieldLabel`).
- Rejilla de 4 dp: márgenes de pantalla de 16 dp y separación entre campos de 12 dp.
- Foco y pulsación: contorno o relleno de acento y *ripple* del sistema, sin sombras costosas.

### 2.3 Componentes

| Componente | Recurso | Notas |
|---|---|---|
| Botones (principal, neutro, fantasma, ejecutar) | `button_positive`, `button_neutral`, `lv_btn_ghost*`, `lv_btn_run*`, estilos `ButtonPositive`, `ButtonNeutral`, `LvGhostButton`, `LvRunButton` | Vectores y formas en lugar de 9‑patch |
| Campos, combos y casillas | `edit_text`, `combo_box`, `lv_chevron_down*`, `toggle_button_selector`, `lv_edit_cursor` | Estados normal, foco, pulsado y deshabilitado |
| Tarjetas y paneles | `lv_bg_card*`, `lv_panel_background`, `bordered_panel`, `lv_spec_background` | Contenedores, ajustes y "Acerca de" |
| Diálogos y menús emergentes | `lv_dialog_background`, `lv_popup_background`, `LvDialogAnimation` | Animación de entrada y salida de 150–180 ms |
| Navegación | `lv_drawer_header`, `lv_drawer_item_background`, `lv_nav_item_*`, `lv_tab_indicator`, `LvTabs` | Elemento activo con acento y fondo suave |
| Estados | `lv_spinner`, `lv_preloader_background`, `lv_empty_library`, `custom_toast_background`, `LvEmptyState` | Carga, vacío y avisos |
| HUD en juego | `lv_hud_background`, `frame_rating.xml` | Fondo translúcido; se conserva la estructura que lee `FrameRating` |
| Iconos | 81 vectores (`icon_*`, `container_*`, `number_picker_*`, `remove_button`) generados por `tools/ui-audit/gen_icons.py` | Sustituyen a los PNG y mantienen el mismo nombre de recurso |

### 2.4 Rendimiento

Solo formas y vectores (sin imágenes nuevas), sin sombras en tiempo real, sin desenfoques y con animaciones cortas basadas en `alpha` y `translation`. Nada se ejecuta mientras el juego está en primer plano, salvo el HUD, cuya estructura no cambia.

---

## Fase 3 — Implementación

| Área | Archivos |
|---|---|
| Tokens, atributos y estilos | `values/colors.xml`, `values/attrs.xml`, `values/styles.xml` (49 estilos y temas) |
| Selectores de color | `color/lv_button_neutral_text`, `lv_button_positive_text`, `lv_nav_item_icon`, `lv_nav_item_text`, `lv_tab_text` |
| Animaciones | `anim/lv_dialog_enter`, `lv_dialog_exit`, `lv_list_item_enter`, `lv_list_layout` |
| Fondos y formas | 28 `drawable/lv_*.xml` nuevos y 18 drawables existentes reescritos (`button_*`, `combo_box`, `edit_text`, `tab_layout_background`, `bordered_panel`, `toggle_button_selector`…) |
| Iconos | 81 PNG sustituidos por vectores con el mismo nombre |
| Layouts | 36 layouts retocados: atributos visuales, estilos y vistas decorativas **sin id nuevo con función** |
| Java | `widget/SeekBar.java`: el color de pista y texto sale de los atributos del tema (se elimina el degradado "glossy" fijo). Sin cambios de lógica |
| Herramientas | `tools/ui-audit/ui_inventory.py` (contrato de ids), `gen_icons.py` (generador de iconos), `contract_before.json` |

Lo que **no** se ha tocado: código de emulación, contenedores, Wine, Box64, DXVK, Vulkan/Vortek, preferencias, navegación, listeners, *callbacks*, menús (`res/menu/*`) y cadenas de texto.

---

## Fase 4 — Validación

| Comprobación | Resultado |
|---|---|
| Contrato de interfaz (`ui_inventory.py compare contract_before.json <después>`) | **OK**: 64 layouts, 353 ids, 11 menús y 50 entradas. No desaparece ningún id ni cambia la clase de ninguna vista o raíz |
| Dependencias estructurales de la tabla 1.3 | Revisadas a mano: los paneles del HUD mantienen 2 `TextView`, `LLGamepadPlayer` y `LLTabWinComponents` no reciben hijos, y se conservan los tags `install`/`remove`/`CPU<n>` y los 4 PNG que se decodifican por código |
| Compilación | `./gradlew assembleDebug` correcto (sin errores de recursos ni de Java) |
| Prueba en emulador (Android 11, x86_64 con traducción ARM) | **No concluyente**: sin aceleración por hardware el sistema del emulador se satura (ANR de System UI y del launcher) antes de mostrar la app, así que no se tomaron capturas fiables |
| Pendiente en el teléfono | Recorrido visual de las 16 pantallas del inventario en tema claro y oscuro, y una partida real (los emuladores no ejecutan Vortek sobre Mali) |

---

## Fase 5 — Entrega

- **Cómo probarlo:** instalar el APK de esta rama. Los temas claro y oscuro se cambian en *Ajustes → Tema*.
- **Cómo revertirlo:** `git revert` del commit del rediseño. Solo afecta a `res/`, a `SeekBar.java` y a las herramientas y documentación de `tools/ui-audit` y `docs/`.
- **Separación:** el rediseño es independiente de los cambios técnicos de rendimiento (Vortek, texturas BC, caché de pipelines), que se desarrollan en otra rama y no se incluyen aquí.
