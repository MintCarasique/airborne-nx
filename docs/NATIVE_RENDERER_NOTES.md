# Заметки: нативный рендерер NFSMW и перенос на MoH:Airborne

Источник: `tools/reference-nfsmw` (app\ GPL-3.0, sdk\ BSD-3, shaders\ MIT; см. `THIRD_PARTY_NOTICES.md`).
Всё ниже прочитано из кода; **[вывод]** помечает мои предположения, а не факты из кода. Идентификаторы в коде
NFSMW испанские (`nativo`, `anillo` = кольцо, `destinos` = цели, `dibujos` = отрисовки, `ganchos` = хуки).
Файлы app\src\ — `nfsmw_*` в `tools/reference-nfsmw/app/src/`. Счётчики строк снимал `wc -l`.

## 1. Архитектура

### 1.1 Файлы `nfsmw_nativo_*` (27 551 строка)

| Файл | Строк | Роль |
|---|---|---|
| `nfsmw_nativo_sistema.cpp` / `.h` | 4893 / 40 | Класс `SistemaGraficoNativo : IGraphicsSystem` (:708). MMIO-диапазон GPU (:455), поток кольца `BucleAnillo` (:1623), поток vblank (:1599), парсер PM4 `Paquete`/`PaqueteTipo3` (:1757-2200), зеркало регистров `EscribirRegistro` (:1106), `WAIT_REG_MEM`, `EVENT_WRITE*`, `MEM_WRITE`, прерывания, `XE_SWAP` -> `Presentar` (:3509), `EmparejarDibujo` (:3096), отчёты. |
| `nfsmw_nativo_destinos.cpp` / `.h` | 6155 / 142 | Цели рендера (C2): `ObtenerDestino` (:3858) = обычные VkImage по ключу (base EDRAM, формат, pitch); resolve/clear `Copiar` (:~1385), resolve без копии (своп образов), глубина `ObtenerProfundidad`, окклюзия (`ConsultaOclusion` в sistema :939), показ кадра `Presentar` с гамма-рампой, 3 рабочих слота (команды, fence, 64 МБ аплоад), обратная запись 64x64 в гостевую память (для экспозиции, :490-493), GPU-тайминги. |
| `nfsmw_nativo_dibujos.cpp` / `.h` | 13074 / 352 | Основная часть (C3-C6): `Dibujar` (:2191) — состояние draw из регистров, топология, вершины (fetch-константы, перестановка байт, дедупликация), индексы (NEON), кэш текстур (двухуровневый, отпечаток XXH3 сырых байт, детайлинг 2D/3D/cube, мипы), сэмплеры (:9871), ключ пайплайна `ClavePipeline` (:1975), `vkCreateGraphicsPipelines` (:11663), prewarm в фоне (:11675-12063), файл `cache/nfsmw_nativo_pipelines.bin` (:1129), dynamic UBO констант (:8034). |
| `nfsmw_nativo_ganchos.cpp` / `.h` | 1035 / 214 | «Что кольцо не говорит»: хуки конструкторов шейдеров `sub_8259BC90` (PS), `sub_8259C038` (VS), `Draw*` пишут запись (vs, ps, args) в очередь; кольцо сопоставляет её с `DRAW_INDX` (`EmparejarDibujo`). Зеркало регистров устройства D3D, маркер `NOP`, фильтр «вегетации» теневой карты на стороне игры. |
| `nfsmw_nativo_shaders.cpp` / `.h` | 474 / 113 | Идентификация шейдеров: загрузка библиотеки `.nfsp`, разбор контейнера XDK (формат 2005), поиск PS по XXH3 микрокода с маской, список vertex-элементов, сэмплеров, выходов PS. |
| `nfsmw_nativo_texturas_pool.cpp` / `.h` | 405 / 238 | Пул субаллокаций памяти для текстур (блоки 16 МБ), потому что каждый `vkAllocateMemory` на Horizon ~1.9 мс (комментарий :1-40 .h). |
| `nfsmw_nativo_vertices_dedupe.h` | 200 | Дедупликация загрузок вершин в пределах кадра (сбрасывается на `WAIT_REG_MEM`, sistema :1912). |
| `nfsmw_nativo_captura.cpp` / `.h` | 193 / 23 | PNG-захват кадра (только отладка). |

Зависимые (не `nativo_`, но входят в нативный путь): `nfsmw_d3d_trace.cpp` (790, карта XDK D3D + хуки Draw*),
`nfsmw_d3d_registros_nativo.cpp` (2482, нативный FlushState-дамп + маркер), `nfsmw_render_targets.cpp` (610, отключение
тайлинга/MSAA), `nfsmw_espera_anillo.cpp` (312) и `nfsmw_espera_fotograma.cpp` (439) — убирают busy-wait игры,
`nfsmw_shader_library.cpp/.h` (190, формат `.nfsp`), `nfsmw_microcodigo_versiones.h` (кэш IM_LOAD без memcmp),
`nfsmw_esperas_tiron.h`, `nfsmw_ajustes_graficos.*` (настройки), `nfsmw_reflejo_demanda.h`.
Нативные замены горячих функций игры (все со «стражем»): `nfsmw_material_nativo.cpp` (969), `nfsmw_matrices_nativo.cpp`
(998), `nfsmw_eview_nativo.cpp` (865), `nfsmw_escenario_nativo.cpp` (857), `nfsmw_efecto_pasada_nativo.cpp` (1047),
`nfsmw_pegamento_nativo.cpp` (1393), `nfsmw_d3d_registros_nativo.cpp` (2482).

### 1.2 Как это встраивается в SDK

- **Заменяется вся графическая система, не часть плагина.** `OnPreSetup(RuntimeConfig&)` кладёт
  `config.graphics = nfsmw::nativo::CrearSistemaGrafico()` (`nfsmw_app.h:53-56`); `ReXApp` грузит GPU-плагин (xenos)
  только если `config.graphics` пуст (`sdk/include/rex/rex_app.h:170`, `runtime.h:67-70`). Эмуляция Xenos (CommandProcessor,
  EDRAM, трансляция шейдеров) вообще не создаётся.
- Интерфейс `rex::system::IGraphicsSystem`: `SetupPresentation` (:719; создаёт `VulkanProvider` и `VulkanPresenter` из SDK,
  фича `vulkan_native_shader_features` для SPIR-V XenosRecomp), `SetupGuestGpu` (:739), `SetInterruptCallback` (:783),
  `InitializeRingBuffer(ptr, size_log2)` (:788), `EnableReadPointerWriteBack` (:797), `Shutdown`, `presenter()/provider()`.
- **Кольцо читается прямо из гостевой памяти** хост-потоком (`XHostThread`): `lector.base = memory_->TranslatePhysical(base)`
  (:1699), указатель записи приходит через MMIO `CP_RB_WPTR` (`EscribirRegistroMmio` :872, store-release), кольцо ждёт
  condvar 4 мс; после прохода пишет read pointer обратно в гостевую память (`store_and_swap`, :1740) и будит ожидающих
  (`AvisarProgresoAnillo`). Пакеты разбираются сразу (типы 0/1/2/3, косвенные буферы `INDIRECT_BUFFER`, глубина ограничена).
- MMIO: `memory_->AddVirtualMappedRange(0x7FC80000, ..., LeerMmio, EscribirMmio)` (:455, :751) — без него `REX_MM_LOAD_U32`
  читает мусор. Фиксированные значения регистров как в эмуляции (`graphics_system.cpp:232-262`).
- Обходится из SDK: CommandProcessor, `RenderTargetCache`/EDRAM, `PipelineCache`/`SpirvShaderTranslator`, texture cache,
  primitive processor. Остаётся из SDK: Presenter (`GuestOutputRefreshContext`), Vulkan-устройство/утилиты, memory/xenos.h/
  registers.h (константы, опкоды PM4), `rex/hook.h` (`REX_HOOK_RAW`, `__imp__sub_X`) для перехватов игры.
- **Что игра ожидает от GPU и что сделано вручную** (sistema :6-24): MMIO; read pointer; прерывания (vblank — отдельный
  поток; `PM4_INTERRUPT`); запись GPU в память (`MEM_WRITE`, `COND_WRITE`, `REG_TO_MEM`, `EVENT_WRITE_SHD`, `EVENT_WRITE_ZPD`);
  `SCRATCH_REG` -> `SCRATCH_ADDR`; `WAIT_REG_MEM` исполняется потоком кольца с потолком ожидания (:1884-1932).
- Хуки на потоке игры нужны, потому что кольцо не несёт: идентичность VS (D3D правит микрокод VS «на месте»), связь
  Draw -> шейдеры (ganchos.h:1-12). Для PS идентичность — по микрокоду.
- Синхронизация: fence на каждый из 3 слотов; `Presentar` по `PM4_XE_SWAP` (VdSwap пишет "SWAP", физ. адрес фронтбуфера,
  размер; sistema :1861); текстура Swap берётся из fetch-константы 0 (`TexturaSwap`, destinos.h:55).

## 2. Специфичное для игры vs. общее

| Компонент | Общее (Xenos/PM4/XDK D3D) | Специфичное для NFSMW |
|---|---|---|
| Парсер PM4, зеркало регистров, MMIO, прерывания, vblank (`sistema`) | почти всё | только отладочные чтения `0x82A2D1AC`, `0x82A2CEE4` (:1045-1053); вызовы `sub_82597690` (ожидание read ptr), `sub_82597960` (счёт vblank) — **адреса XDK D3D** |
| Resolve/clear/ключи целей (`destinos`) | логика копии (`GetResolveInfo`), гамма-рампа, слоты | константы: 1600 (карта теней, `kLadoSombras` :5836), категории GPU по размеру 1600/1280/640/320 (`dibujos.h:63-76`), «сцена/отражение/кубкарта», пара теней «минимум двух карт», пасс композиции `sub_82442478`/`sub_824427F8`/`sub_82442908`/`sub_82448168` (:231-233, :472), обратная запись 64x64 для автоэкспозиции |
| Отрисовка (`dibujos`) | регистры, топология, вершины, формат текстур, untile, пайплайн-кэш, prewarm | большой слой «cielo» (небо, 140 упоминаний), «sombra» (99), «resplandor» (свечение), «vegetacion», «reflejo», варианты SPIR-V свечения (`nfsmw_nativo_resplandor_*_spirv.h`, не в репозитории) |
| Шейдеры (`shaders`, `shader_library`) | чтение контейнера, XXH3, выходы PS | формат контейнера 2005 (24-байтный заголовок), маска `kFetchConserva` |
| Хуки (`ganchos`, `d3d_trace`, `d3d_registros`) | идея | все адреса `0x825xxxxx` (XDK D3D, ниже) |
| Нативные замены (`material/matrices/eview/escenario/efecto/pegamento`) | шаблон «нативный код + guard 50k-200k вызовов, потом 1/4096» | 100 % специфика NFSMW (EA-движок), переносить только шаблон |
| `render_targets`, `espera_*`, `recorte*`, `sombras_lod`, `escenario_lod` | идея (убрать тайлинг/MSAA, спин-ожидания) | 100 % специфика |

### 2.1 Жёстко зашитые гостевые адреса NFSMW (что искать в MoH:A `default.xex`)

**Карта XDK D3D** (`nfsmw_d3d_trace.cpp:144-177`, это и есть список функций для поиска в MoH:A; у MoH:A XDK-код лежит около
`0x829Axxxx-0x829Bxxxx`, напр. `sub_829B0E38` = D3DQuery_GetData, `sub_829AF948` ожидание fence — из нашего кода/заметок):
CreateDevice `825A1658`, Swap `825989D8`, SyncToPresentationInterval `82598868`, PresentComposite `82598FC8`,
SetRingBufferParameters `825981E8`, Clear `8259A450`, ClearF `8259A500`, Resolve `82592538`, BeginTiling `825992F0`,
EndTiling `82599680`, SetRenderTarget `8258DB18`, SetDepthStencilSurface `8258DE80`, CreateTexture `8258A0A0`,
CreateSurface `8258A1C0`, SetTexture `8258A648`, Create/Lock VertexBuffer `82595570`/`82595620`, Create/Lock IndexBuffer
`825956D0`/`82595780`, Unlock `82595170`, Release `825953D8`, SetStreamSource `8258D968`, SetIndices `8258DA60`,
Create/SetVertexDeclaration `8259C470`/`8259C3D0`, SetVertexShader `8259C2A8`, SetPixelShader `8259BDC0`,
DrawVertices `82593A10`, DrawIndexedVertices `82593C50`, DrawVerticesUP `82593588`, DrawIndexedVerticesUP `82593940`,
**FlushState `825A40C0`**, SetGammaRamp `8258E138`, QueryBufferSpace `82597DA0`.

**Внутренности D3D, нужные нативному рендереру:**
- конструкторы шейдеров: PS `8259BC90`, VS `8259C038` (ganchos.cpp:977-989); патчер VS-fetch `825A2FB8` (два вызова из
  `825A3AF0` и `825A37D8`), обнуление выходов VS `825A36A8`, запись IM_LOAD `825A3AF0`; дамп грязных регистров `825A2AA0`
  (register mirror); `825A5D18` (проверка read ptr/зависание GPU) и `82597690` (ожидание GPU) — espera_anillo.cpp;
  внутренняя рутина рисования `825932D8` (капли дождя/VisualTreatment, d3d_trace.cpp);
- окклюзия: Issue `8258F810`, End `8258EA28`, GetData `8258F998` (sistema.cpp:930-933), оккл. солнца `82225438`;
- смещения в объекте устройства: VS `+0x4FE8`, PS `+0x3290` (ganchos.cpp:~57-58), таблицы RenderState `+96`, SamplerState
  `+484`, дерево зеркала регистров `+0x2000..0x4900` (ganchos.h:46-48), регистр дампа `dev+0x28C0`;
- глобалы: указатель устройства `82A2CF60` (d3d_trace.cpp:72), размеры видео `82A2CF68/6C/70/74`, режим вывода `82A2CF80`,
  `82A2D1AC` (рендерер, режим AA), `82A2CEE4` (качество), флаг двух потоков кадра `82A2CF40`, список команд `82909650`,
  таблица дескрипторов целей `82A4527C`, D3DPRESENT_PARAMETERS `828FBB70`.

**Специфика движка NFSMW (переносить не нужно, только как образец):** AA-режимы `82458310` (конструктор, таблица режимов
+4/+28/+52/+76/+100), `824402F0` (XGetVideoMode), `82441990`, `82458850`, `8245D320`, `8245D5F8`, `8243C1C8`;
пассы `82442478`, `824427F8`, `82442908`, `82443B18` (теневой пасс), `82448168`; цикл кадра `824411B8`, `82445660`,
`82441CC8`, `82441F18`, Sleep(0)-точки `82441FB4`, `82441D24`, `82441DC4`; горячие функции `824511E8`, `824538D0`,
`8243E358`, `824C2850`, `82448E80`, `82452730`, `82454B50`; таблица видов `82A38070`; видео-декод `828C35D8`, `8278A518`,
`828C58C8`; CRT `826BE610`, `826BE1B0`.

## 3. Шейдеры

Конвейер NFSMW (`docs/shaders.md`, `shaders/README.md`):
1. Поиск контейнеров по сигнатуре (`10 2A 0E 00` PS, `10 2A 0E 01` VS) во всех файлах и в XEX (`nfsmw_buscar_contenedores.cpp`);
   порядок сканирования определяет имена, его менять нельзя.
2. Конвертация контейнера 2005 -> 2008 (`nfsmw_contenedor.h`), т.к. XenosRecomp ждёт новый формат.
3. XenosRecomp (`shaders/XenosRecomp`, правки под `NFSMW_RECOMP`) -> HLSL с общим `shader_common.h` (`nfsmw_hlsl.cpp`).
4. DXC -> SPIR-V (скрипт `nfsmw_regenerar_biblioteca_pcf.sh` делает всё на ПК; WASM-сборка для веб-установщика).
5. `nfsmw_empaquetar.cpp` -> `nfsmw_shaders.nfsp`: сортировка по отпечатку, формат с проверкой целостности
   (`nfsmw_shader_library.cpp:78-186`). Библиотека производная от игры — не распространяется.

**Ключ.** `huella = XXH3_64bits(оригинальный контейнер целиком)` (library.cpp:78, :153), поиск бинарным поиском + сравнение
байт (коллизия не выберет чужой). Контейнер хранится в библиотеке (`Shader{original, spirv, huella, vertices}`). Во время игры:
- **PS** находится по микрокоду из `IM_LOAD`: XXH3 микрокода с маской `kFetchConserva` (shaders.cpp:40) -> `Identificar`;
- **VS** по микрокоду НЕ находится (XDK D3D перестраивает fetch'и и обнуляет неиспользуемые выходы), поэтому хук
  конструктора запоминает исходный контейнер для объекта; `ShaderDeObjeto(объект)` (ganchos.h) + `FetchCoherentes`
  проверка (shaders.h:109); Draw* оставляет запись VS/PS, кольцо сопоставляет по типу примитива и счёту вершин.
- **Отсутствующий шейдер:** `Identificar` возвращает nullptr, `Dibujar` возвращает false/`Rechazar` (причина логируется один
  раз), draw пропускается; «подмены другим шейдером» нет. Статистика `identificados/sin_identificar/ambiguos` (shaders.h:80).
- Варианты — через specialization constants (alpha test 8 функций, источник констант, 1/size текстур, R11G11B10-нормали) и
  входят в ключ пайплайна. Интерфейс SPIR-V: push-константы с адресами буферов, наборы 0-2 текстуры 2D/3D/cube, сэмплеры
  набор 3; константы через dynamic UBO (дало +18-23 % FPS). Пакет правок XenosRecomp для NFSMW: `g_NdcScale/g_NdcOffset`
  (экранные пиксели при `PA_CL_VTE_CNTL=0x400`), нормали/тангенты как float, `SetpInv`, склейка `if (p0)`, `max(a,a)`.

**Что у нас уже есть:** `tools/extract_game_shaders.py` (контейнеры XDK `0x102A1100/1101` — это уже «2008»-формат, конвертер
2005 не нужен; в нашем `pack` ключ — FNV-1a 64 от контейнера, в NFSMW — XXH3, форматы несовместимы:
`AIRSPV01` vs `NFSSPV`), `translate_game_shaders.py` (изолированный процесс на шейдер, сохраняет первую ошибку),
`pack_game_shaders.py` (записи: ключ, размеры, stage, маска spec-констант, crc), патч `tools/patches/xenosrecomp-airborne.patch`
(86 строк, `shader_recompiler.cpp/.h`); 969 из 1036 шейдеров дали SPIR-V. **Не хватает** по сравнению с NFSMW: (а) заметка о
том, что VS у MoH:A тоже может переписываться XDK-слоем — проверить; (б) метаданные в записи (vertex elements, sampler
types, выходы PS, число `OpKill`, размер используемых констант), которые NFSMW вычисляет при загрузке (`EntradaShader`);
(в) сопоставление 67 нетранслированных шейдеров (какие это и чем рисуются).

**XDK D3D в MoH:A (`analysis/d3d_map/identified.md`, проверено по коду 2026-10-23).** Конструкторы шейдеров:
`sub_829A3158` (PS; r3 = контейнер, возвращает объект; указатель на микрокод в объекте по +24) и `sub_829A32A8` (VS;
контейнер копируется в объект с +872, fetch'и патчит `sub_829A18A8` один раз при создании; микрокод по +32). У NFSMW
было +12/+40 и патчер на каждом draw (`sub_825A2FB8`) — у нас его хук не нужен. Хуки конструкторов включены в
`native_hooks.cpp`. Записей draw (замены Draw* из NFSMW) нет: кольцо берёт VS/PS из своей идентификации по `IM_LOAD`
(`UsarIdentidadDelAnillo`); `Identificar` маскирует поля fetch'ей, поэтому VS тоже находится по микрокоду. Контейнеры
MoH:A — формат 2008 (707 PS `0x102A1100`, 329 VS `0x102A1101`); `Leer` и `shader_library` теперь принимают его, микрокод
берётся из `[physicalOffset, +size)` физической части. Библиотека: `airborne_shaders.nfsp` рядом с exe.

**Первое изображение (2026-10-23, ПК).** Что мешало и как найдено (RenderDoc + `nfsmw_nativo_diag_fotograma_s`):
- Шейдеры: большинство пакетов UE3 сжаты LZO1X (флаг 2) — `tools/ue3_decompress.cpp` + `tools/ue3_scan_shaders.py`
  дают 9989 контейнеров (было 1036). Библиотека: 9692 шейдера, ~160 МБ (лимиты подняты до 32768/512 МБ; для Switch
  потом нужна ленивая загрузка или сжатие).
- XDK MoH:A при создании VS переписывает в `vfetch` не только формат/шаг/смещение, но и swizzle назначения (float3 ->
  w = 1), поэтому swizzle убран из маски сравнения, а исходный swizzle хранится в `ElementoVertice::swizzle` для
  `g_InputRemap`. Без этого полноэкранные квады схлопывались в x = 1.
- Одинаковый микрокод встречается в контейнерах с разными таблицами констант; XenosRecomp выкидывает tfetch без
  объявленного сэмплера — выбираем контейнер с наибольшим числом сэмплеров.
- Predicated tiling: сцена 1280x720 рисуется тайлами 512 + 208 строк; второй тайл resolve-ится с базой на строке 512
  той же текстуры — такие копии теперь идут в содержащую текстуру.
- 7e3 (и `_AS_16_16_16_16`) на хосте RGBA16F: UE3 пишет глубину сцены (~3500) в альфу, Xbox обрезает её до 2 бит.
  Спец-биты 5/6/7/10 просят PS применить диапазон 7e3 к oC0..oC3 (`salida7e3`, нужна библиотека из
  `tools/shaders_native`). Без этого DOF-композит берёт почти только размытое изображение.
- Тайтл-экран: тонмаппинг — UE3 UberPostProcess (PS с BlurredImage/SceneColor/SceneDepth); UI-шейдеры (логотип,
  текст) пока не опознаются: их контейнеры, видимо, среди не прошедших трансляцию/DXC.

**Транслятор NFSMW на контейнерах MoH:A (2026-10-23).** `nfsmw_hlsl.cpp` + их XenosRecomp (`-DNFSMW_RECOMP`) собираются
clang++ под Windows (нужен `-DNOMINMAX`) и принимают наши контейнеры 2008 без конвертации: 969/1036 в HLSL, сборка в
`rex/out/native_shaders/`. Все 67 отказов — вершинные шейдеры без объявлений в контейнере:
- 50: `vfetch` из fetch-константы 31 при пустом списке vertex elements (ручная выборка, не через vertex declaration);
- 17: экспорт в `o32` (memexport) при пустом списке интерполяторов.
Для них нужен fallback в трансляторе (формат/шаг/смещение есть в самой инструкции vfetch) и поддержка memexport; что
ими рисуется — выяснить по кольцу, когда нативный путь начнёт находить шейдеры.

## 4. Не поддерживается / ограничено (важно для UE3 + HDR)

Из кода NFSMW (проверять строки перед тем, как опираться):
- **Форматы цели цвета:** только `k_8_8_8_8` и `k_8_8_8_8_GAMMA` (destinos.cpp:1412-1415; dibujos.cpp:2308-2315 — иначе
  `Rechazar(100+формат)`). **Нет `k_2_10_10_10_FLOAT` (7e3), `k_16_16_16_16(_FLOAT)`, `k_2_10_10_10`, `k_32_FLOAT`** — MoH:A
  (HDR 7e3) этим упрётся сразу.
- **Форматы resolve-назначения:** только `k_8_8_8_8`, `_A`, `_AS_16_16_16_16` (destinos.cpp:1436-1443). Нет 3D/массивов как
  цели (`Rechazar(3)`), нет формата float16/7e3, нет resolve части с произвольным субпрямоугольником (стирает целиком,
  destinos.cpp:22-23).
- **MSAA:** `RB_SURFACE_INFO` MSAA >1x просто используется как 1x (destinos.cpp:1394-1396, «MSAA вешает NVK на Switch»);
  в NFSMW MSAA ещё и выключен в самой игре (render_targets.cpp: режимы AA 3-5 приведены к режиму 2). MoH:A, судя по нашим
  заметкам (`ASSESSMENT.md`), использует 2x MSAA с тайловым рендером.
- **EDRAM-алиасинг не моделируется:** образ на ключ (base, формат, pitch) (destinos.cpp:3863). Игры, переинтерпретирующие
  EDRAM (глубина -> stencil-перенос, 7e3 <-> depth; именно это и душило нас в эмуляции), требуют либо явной эмуляции
  отдельных случаев, либо перехвата уровня RHI. Вывод: **главный архитектурный риск для UE3**.
- **Глубина/stencil:** D24S8 или D32S8; stencil-состояние переносится (`dibujos.cpp:3320-3604`), но копии stencil из
  глубины, `EDRAM_MODE` кроме `kColorDepth` и 5 (depth-only) отвергаются (`Rechazar(1)`, dibujos.cpp:2203-2205). Режим
  `kStencil/kCopy`-варианты и MRT >4 не рассматривались. Копия из глубины даёт текстуру `k_24_8`/`k_24_8_FLOAT` (destinos.cpp:1396-1407,
  dibujos.cpp:1704); глубина с float24 — как D24 (теряется точность).
- **Примитивы:** line list/strip, triangle list/fan/strip, quad list (-> треугольники). Нет point list, rect list (UE3 использует
  `RectList` для fullscreen-проходов!), line loop, polygon-as-points/lines, immediate indices (`Rechazar(2,4,100+тип)`,
  dibujos.cpp:2265-2286). `tipo == 13` quad list обработан, `RECT_LIST` (тип 8) — нет. **[вывод]** UE3 Xbox 360 рисует
  quad/rect lists для постпроцесса — добавить в первую очередь.
- **Текстуры:** 2D/cube/3D (база), мипы 2D есть, мипы 3D нет; нет vertex-fetch текстур, нет signed/gamma-текстур (dibujos.cpp:27-29).
  Поддержанные форматы: 8, 8_8, 8888, 2_10_10_10, 16, 16_16, 16F, 16_16_16_16(F), 32F..., DXT1/3/5, DXT5A, DXN
  (dibujos.cpp:1724-1790). Нет k_DXT3A/DXT5A_AS_16, k_CTX1, k_8_8_8_8_GAMMA в сэмплере, k_24_8 как произвольный источник.
- **Вершинные форматы:** как таблица на :1648-1700 (8888, 2_10_10_10, 10_11_11, 16*, 32*); остальные -> `Rechazar(300+формат)`.
- **Memexport / стрим-аут:** нет совсем. **Occlusion:** отдельный механизм измерения в хост-сэмплах из прошлого кадра
  (`EmpezarOclusion/TerminarOclusion`, destinos.h:64-70, sistema :939), у нас сейчас «всегда видимо» (`rex/src/occlusion_queries.cpp`).
  Окклюзия на predicated rendering (`PM4_COND_EXEC`, `SET_BIN_*`) — только маски bin (sistema :2037-2066), условное исполнение
  кольца не проверено.
- **Тайлинг (BeginTiling/EndTiling):** не реализован, отключён в игре (render_targets.cpp). UE3 использует predicated tiling
  для 2x MSAA 720p — потребуется то же отключение через конфигурацию устройства или обработка `bin_select`.
- **Запись в гостевую память:** только маленькие resolve (64x64, 320x180) для экспозиции NFSMW; у MoH:A, **[вывод]**, свои
  CPU-чтения (отражения, автоэкспозиция HDR) — потребуется свой список.
- **Гамма/PWL, colorkey:** NFSMW применяет рампу, загруженную игрой (`SetGammaRamp 8258E138`).
- **Проверка падений:** Присутствуют `Rechazar(...)` на каждую неподдержанную ветку, но «чёрного кадра с понятным логом»
  достаточно только для 8888-путей.

## 5. Предлагаемый порядок переноса на MoH:A

Первая веха: **меню MoH:A через нативный путь на ПК**.

| # | Шаг | Объём | Заметки |
|---|---|---|---|
| 1 | Скопировать каркас: `nfsmw_nativo_sistema/destinos/dibujos/ganchos/shaders/texturas_pool/vertices_dedupe` в `rex/src/`, переименовать пространства имён, лицензия GPL-3.0 (согласовать с `LICENSE` проекта), подключить `OnPreSetup(config.graphics=...)` (у нас уже есть `OnPreSetup`: `rex/src/airborne_app.h:58`), CMake. **Проверить**, что наш SDK содержит `IGraphicsSystem`/`config.graphics`/`VulkanPresenter::kGuestOutput*` (у нас `rexglue_setup_target(... GPU_PLUGINS xenos)`) | средний | Версия SDK у нас может отличаться от `reference-nfsmw/sdk` (тот — форк с Switch-слоем). |
| 2 | Карта XDK D3D MoH:A: найти аналоги списка из п. 2.1 (CreateDevice, Swap, SetRenderTarget, Draw*, FlushState, конструкторы шейдеров, SetTexture, Resolve). Сначала только трассировка (`nfsmw_d3d_trace.cpp` как шаблон) на ПК | средний | XDK-код — статически слинкованная версия 2007; адреса другие. Сигнатуры из заметок: `sub_829B0E38` (GetQuery), `sub_829AF948` (ожидание fence) |
| 3 | Кольцо без рисования: парсер PM4, MMIO, прерывания, WAIT_REG_MEM, VdSwap -> показ тестового цвета. Игра должна доходить до меню как с эмуляцией | малый-средний | Почти без правок. |
| 4 | Библиотека шейдеров: адаптировать `EntradaShader`/`Cargar` под наш `AIRSPV01` (ключ FNV-1a по контейнеру -> можно оставить, но XXH3 быстрее); расширить запись метаданными; разобраться, как XDK MoH:A меняет VS; PS по микрокоду; VS по хукам конструкторов | средний | Покрытие 969/1036. |
| 5 | Цели 8888 + resolve 8888: сначала только пассы меню/интерфейса (UI рисуется в 8888 **[вывод]**) | средний | 7e3 и MSAA для меню, возможно, не нужны. |
| 6 | Отрисовка: тип примитива RectList, форматы текстур, сэмплеры, blend/depth/stencil; пайплайн-кэш. Включить `Rechazar`-статистику и дойти до пустой «0 отвергнутых» в меню | средний-большой | Сначала правильность на ПК, пайплайн-кэш потом. |
| 7 | Вынести тайлинг/MSAA (аналог `render_targets.cpp`) через настройки устройства/режима в UE3 | малый-средний | Найти таблицу режимов AA. |
| 8 | Уровень: 7e3/FP16 цели, глубина -> stencil перенос, k_24_8 источники, MRT, окклюзия, stencil-тени/свет | большой | Нужен собственный дизайн: либо эмуляция EDRAM-алиасинга частными случаями, либо хук слоя RHI UE3. |
| 9 | Нативные замены горячих функций UE3 + guards (шаблон NFSMW), профиль на Switch | большой | Только после измерений на железе (см. `AGENTS.md`). |
| 10 | Switch-специфика: пул текстур, prewarm, NEON-untile, потоки/приоритеты | средний | Уже в скопированном коде; проверить на железе пакетом. |

### Главные риски
1. **EDRAM-переинтерпретация UE3** (depth<->stencil, 7e3): нативная модель «образ на (base, формат, pitch)» её не выдерживает
   (п. 4). Мы уже видели, что именно перенос глубины/stencil ломает цвет (`cff5004`).
2. **7e3 и MSAA**: нужны новые форматы целей/resolve и отключение тайлинга; без них уровень не нарисуется.
3. **Идентичность вершинных шейдеров**: XDK-слой правит микрокод; нужны хуки аналогов `8259BC90/8259C038/825A2FB8` в MoH:A.
4. **RectList и другие примитивы** постпроцесса UE3 (не поддержаны в NFSMW).
5. **Лицензия**: `app/` — GPL-3.0; код нельзя вставлять без согласования лицензии нашего проекта (копировать идеи/структуры — ок,
   файлы целиком — решение владельца проекта).
6. **Измерения**: по `AGENTS.md` аппаратные проверки пакетами; первая веха — только ПК.
