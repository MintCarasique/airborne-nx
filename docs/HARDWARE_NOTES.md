# Первый прогон на ReXGlue (2026-10-23, сборка 8b57ce5+6cc5eaa4df8e, лог `analysis/hardware/rexglue_*_result.txt`)
- Запуск в режиме приложения, handheld 1280x720; NVK (Mesa 26.2.4, git-0ff52079ca) создаёт устройство; арена гостевой
  памяти маппится; аудио audout 48 кГц 2 канала; монтирование 1249 файлов игры с SD — 2,65 с.
- Игра проходит заставку и доходит до меню (`Hus_BG_Bkd_Fairway`, музыка меню), ошибок и падений нет за ~2 мин.
- Скорость: по ощущению пользователя < 1 FPS. Создание каждого графического пайплайна (трансляция шейдера + NAK)
  занимает 0,1–1,5 с; в первые ~40 с меню их ~60. FPS и время CPU по потокам в логе пока не пишутся.
- Профайлер SDK (`logs/rex/rex_perfil.log`, отчёт каждые 10 с): в меню 0,6–0,8 FPS при ~160 draw на кадр; один
  гостевой поток `XThread2FE2D080` занимает 100 % ядра, поток эмуляции GPU («GPU Commands») — единицы процентов,
  всего CPU 130–170 % из 300 %. Узкое место — CPU одного потока игры, не эмуляция GPU. Частоты: CPU 1020 МГц,
  GPU 460,8 МГц, память 1600 МГц; 3 ядра (маска 0x7). При загрузке меню (компиляция пайплайнов) 262 % CPU, 0–0,1 FPS.
- Прогон 2 (aad0efd+4577d1b23b5d, nxlink, стек-сэмплинг): то же 0,7 FPS; локальные регистры не меняют FPS.
  Стеки: 87 % времени горячего потока — `sub_829B0E38` (XDK `D3DQuery_GetData`, тип 9 = occlusion), вызываемый в
  цикле ожидания из `sub_82453C88` (0x82453D70): UE3 крутится, пока эмулятор GPU не запишет результат запроса.
  Шейдерный кэш SDK на SD работает: во втором прогоне фазы компиляции пайплайнов при загрузке меню нет.
- Прогон 3 (aad0efd+a95feff5cf97, occlusion-запросы отвечают сразу): ожидание запросов из стеков исчезло, FPS тот же
  0,7. Теперь горячий поток ждёт GPU-фенс в XDK (`sub_829AF948` → цикл `db16cyc` в `sub_829B6490`), а процесс ждёт
  фенсы NVK 1000 мс в секунду: упор в реальный GPU Tegra, ~1,4 с GPU на кадр меню. На кадр: 167 draw игры,
  170 EDRAM-переносов (23 RT, 115 дополнительных draw), 10 resolve, площадь scissor ~126 экранов.
- Прогон 4/5 (fbc5e09+6ce5dd5de8b5, `--gpu_timing=2`, метки времени ×1,627), меню, GPU на кадр:
  host render targets: draw 125–180 мс, EDRAM-переносы 1180–1280 мс (~83 %), resolve 86–94 мс, прочее ~25 мс,
  всего ~1500 мс, 0,7 FPS. FSI (`--render_target_path_vulkan=fsi`): переносов нет (0,07 мс), но draw ~1000 мс,
  resolve ~19 мс, всего ~1050 мс, 0,9–1,0 FPS. Для сравнения PC (RTX 4070 SUPER, host RT): draw ~5 мс, переносы 3–6 мс.
  NVK не объявляет `VK_EXT_shader_stencil_export`: перенос в глубину = 1 проход глубины + 8 проходов битов stencil.
- Прогон 6 (f4b320c+4a88abf91c2f, `--cl=Tra_Jmp_P`): вылет в конце загрузки уровня. `AllocFixed`: коммит 4 КБ
  гостевой памяти → `svcMapProcessCodeMemory` 0xCE01 (OutOfResource) при 411 МБ backing / 658 МБ с зеркалами,
  затем запись игры по 0x10. Причина: SDK коммитил ровно запрошенный диапазон, и постраничные коммиты UE3 давали
  отдельное отображение ядра на каждые 4 КБ (+ по одному на зеркало) — кончаются объекты блоков памяти ядра, не байты.
  Переносы в меню по типам: depth→stencil bit 631 мс/кадр (56 draw), color→stencil bit 344 мс (40 draw),
  depth→depth 67 мс; каждый draw переноса ~10 мс GPU независимо от площади.
- Прогон 7 (c429d95+62b19985bf81, коммит блоками по 2 МБ): учебный уровень загрузился, >5 мин без ошибок памяти
  (422 МБ backing / 716 МБ с зеркалами). 0,1–0,3 FPS. GPU на кадр в уровне: всего ~1090 мс, переносы 909 мс
  (depth→stencil bit 553 мс/32 draw, color→stencil bit 217 мс/24 draw), draw игры 158 мс. Все типы переносов
  дают ~25–35 Мпикс/с независимо от источника.
- Прогон 8 (7d0cd99+4f709f05677d, `--nfsmw_renderizador=nativo`, библиотека шейдеров v3 186 МБ отдельным файлом
  `sdmc:/switch/airborne-nx/airborne_shaders.nfsp`, лог `analysis/hardware/live_native1.log`): нативный рендерер на
  железе. Титульный экран 17–20 FPS (эмуляция: 0,7), дальше тяжелее, 4–25 FPS. GPU 32–41 мс на Swap (сцена 20–32 мс,
  копии 3–4 мс, «320» 2 мс), CPU 150–270 % из 400. 188 разных шейдеров, 182 опознаны (6 — без контейнеров),
  отказов draw 0, падений нет. Барьеры вокруг копий (`airborne_barreras_copias`) включены. Одна `DIFERENCIA` у
  IM_LOAD без memcmp (страж отключил ярлык). Картинку с консоли не видели — нужен скриншот.
- SDK: «Too few processor cores - scheduling will be wonky»; общая память GPU — полный буфер 512 МБ (sparse нет).

# Измерено на железе (Switch, HOS 22.1.0, applet_type 0 = Application)

Источник: логи пользователя из старого рантайма (`analysis/hardware/`; сырые `.txt` хранятся локально, заметки
о прогонах удалены, см. `git show 8ca98b1:analysis/`). Консоль/версию Atmosphere лог не называет.
Метки сборок: v0.1 ecb1e73175f9, v0.2 f6741f128e30 (memory), v0.3 811084736791, v0.4 7c22a4a387d5,
v0.5 fefdd112f4ab, v0.6 ba51b874a908, v0.7 6d74b214954e, e779 e77977c5ffe5, e05d e05d08748c72,
f4ba f4bac5364158, 6271 627190085788 (последний измеренный).

## 1. Память
- svcGetInfo при старте homebrew (v0.3): TotalMemory 0xC7500000 (3188,5 МиБ), UsedMemory 0xC710C000 (3184,5 МиБ);
  Used почти равен Total сразу (куча hbloader), это не "занято игрой". Крупнейший свободный регион кучи 3 325 956 096 Б
  (v0.3), 3 113 672 704 Б (v0.5, после старта рантайма).
- Адресные регионы процесса (v0.3): alias 6 ГиБ (0x180000000), heap 8 ГиБ (0x200000000), ASLR 0xFF8000000.
- Резерв непрерывного VA 4 ГиБ через libnx virtmem: успешно (v0.2, v0.3). Это адресное пространство, не RAM.
- ОТКАЗ (v0.2): `svcMapProcessMemory` с источником из обычной кучи (тип Normal=5) -> rc 0xD401 (module 1,
  description 106, InvalidMemoryState). У Normal-страниц нет CanMapProcess.
- ИСПРАВЛЕНИЕ (v0.3, PASS 1569 проверок): `svcMapProcessMemory`->`MapProcessCodeMemory` кучи в staging-область
  (тип 9 AliasCodeData), `SetProcessMemoryPermission(RW)` на staging (perm=3), затем гостевые алиасы из staging
  (тип 15 SharedCode, RW). Все map/unmap вернули 0. Обратный порядок очистки: алиасы, затем возврат страниц куче.
- v0.3: физические окна A/C/E согласованы на скаляр/вектор/atomic/dcbz, 4 МиБ heap x 1024 страницы, охранные
  страницы (проверка запросом состояния памяти, без падения).
- v0.4+: пул физической памяти 512 МиБ staging создан и смаплен кусками (A/C/E только на выделенные срезы); образ
  20 054 016 Б, стек 256 КиБ, 81 146 отображений функций; отказов map/unmap нет (v0.4-6271).
- Виртуальные коммиты как AliasCodeData-алиасы с последующим `SetMemoryPermission` работают (v0.4+).
- Снятие отображений при ещё живых рабочих потоках не делали: выход через `svcExitProcess` (v0.4) дал
  пользователю стандартную HOS-ошибку после "+" (код неизвестен). Кооперативный join потоков -> unmap -> return из
  main: CLEANUP=PASS, EXIT_ACTION=RETURN_TO_LOADER (v0.5, v0.6, v0.7, e779, e05d, 6271).

## 2. Потоки и время
- `threadCreate` с prio 44 (0x2C), core -2 (по умолчанию), стек 64 КиБ, join с таймаутом 3 с: OK (v0.3).
- Прио 59 (0x3B) через `svcSetThreadPriority` принято и считано обратно (rc 0) на всех 20 записях THREAD_POLICY (6271).
  Маска ядра там везде 0x1 (preferred=0), т.е. логом подтверждено ядро 0, а не 0..2; влияние на скорость не измерено.
- В логе f4ba: 15 вызовов ExCreateThread и 14 KeSetAffinityThread.
- Таймер: `svcSleepThread(10 мс)` = 500 201 / 500 237 тиков при диагностической частоте 50 МГц (~10,004 мс), монотонно
  (v0.1, v0.3).
- Игра за ~120 с вызывает RtlEnterCriticalSection 419 399 раз, KeWaitForSingleObject 1956 (6271): импорты
  ядра горячие, их нельзя сериализовать или логировать по одному.
- Построчный flush лога на SD останавливает старт: за 120 с не открыт ни один файл игры (e05d). С буфером 64 КиБ
  и flush раз в 250 мс первый кадр ролика на аптайме 43,5 с (6271).

## 3. Видео, NVK, VI
- NVK виден как `NVIDIA Tegra X1 (GM20B) (NVK GM20B)`, vendor 10de, device 0fe0, apiVersion 4211042 (Vulkan 1.4.98);
  int64, bufferDeviceAddress, runtimeDescriptorArray, gather, sampled-dynamic = 1 (v0.5, v0.6).
- VI-поверхность: 1280x720, minImages = maxImages = 3, форматы VkFormat 37 и 44, colorspace 0, usage 0x8009F
  (v0.6). Создание 3-кадровой цепочки (FIFO), submit + fence, `vkQueuePresentKHR` rc 0 (v0.6).
- Образ 1280x720 RGBA8: выделение 3 932 160 Б (v0.5). Нативные depth/stencil clear 640x360 x4 MSAA и resolve 720p
  выполнены на GPU, fence завершён (v0.6).
- ОТКАЗ VI (v0.7): повторное `viOpenDefaultDisplay` из потока vblank, пока libnx-окно держит Default display ->
  rc 0x1272 (module 114, description 9). libnx не даёт публичного getter; исправлено заимствованием владельца
  (перехват `viOpenDisplay`/`viCloseDisplay`); дальше display id = 83 (e779).
- ОТКАЗ vblank (e779): `eventClear` на автоочищаемом событии из `viGetDisplayVsyncEvent` -> rc 0xFA01
  (module 1, description 125, InvalidState). Исправление: только `eventWait`. После: 513 успешных ожиданий (e05d);
  одно измеренное ожидание 14,63 мс при таймауте 100 мс (e05d).
- Ролик (6271, 120 с, watchdog): 475 отрисовок кадров ролика; интервал между отрисовками mean 161,4 / median 99,6 /
  p95 219,3 мс (в него входят ожидания игры, это не чистое декодирование). Стадии, медиана: eDRAM resolve 14,9 мс,
  загрузка+draw+readback ролика 14,2 мс, decode+present 42,1 мс (p95 52,2) - при CPU-readback на каждый кадр.
  Пользователь: интро видно, "очень медленно" (f4ba, 6271).

## 4. Звук (6271)
- `audoutInitialize`/`audoutStartAudioOut` rc 0: 48 кГц, 2 канала, PCM16, буфер 1024 Б (256 сэмплов);
  `audoutAppendAudioOutBuffer` и `audoutWaitPlayFinish` rc 0. Игра регистрирует XAudio-клиент (callback 0x82AA3AF0)
  и сдаёт кадры по 6144 Б (256 сэмплов x 6 каналов x float32); 4 кредита обратного вызова. Слышимость и underrun
  не проверялись.

## 5. Файловый ввод-вывод (6271)
- Корень контента `sdmc:/switch/airborne-nx/game/MOHAGame` (6,33 ГБ, 1242 файла). Все открытия/чтения успешны (rc 0).
- Скорость чтения SD отдельно не измерена. Пакеты `.xxx` читаются кусками по 0x20000 (128 КиБ).
- Ошибок memory/file/audio на пути ролика нет; `RESULT=FAIL` в 6271 - это только сработавший watchdog 120 с.

# Что подтверждает/дополняет platform-notes.md NFSMW
- Подтверждает: ~3,2 ГБ памяти приложения, UsedMemory ~ Total с первого момента (3184,5 из 3188,5 МиБ у нас).
- Подтверждает: VI-цепочка ровно 3 образа; 1280x720; VI-окно через libnx/NVK.
- Дополняет: `MapProcessMemory` не принимает Normal-кучу, rc 0xD401 (InvalidMemoryState), а не 0xCE01; обход -
  `MapProcessCodeMemory` в staging + `SetProcessMemoryPermission(RW)`, потом алиасы (типы 9 -> 15). Права на staging
  менять можно; у notes записано, что на `MapProcessMemory`-памяти нельзя.
- Дополняет: 512 МиБ физического пула staging + A/C/E-алиасы только на выделенные срезы не дают 0xCE01 до 120 с
  работы (6271); исключения/page fault-обработчик в нашем рантайме не использовались.
- Дополняет: прио 0x3B и 0x2C принимаются; маска ядра 0x1 принимается (notes: доступны 0x7). Эффект не измерен.
- Дополняет: `viOpenDefaultDisplay` повторно -> 0x1272; `eventClear` на vsync-событии -> 0xFA01. Wait на нём
  14,6 мс (таймаут 100 мс). `eventWait` сам делает reset.
- Дополняет: Vulkan API NVK = 1.4.98; форматы VI 37/44; usage 0x8009F.
- Дополняет: объём SD-лога - реальный тормоз: построчный flush на SD (5,2 МБ) оставил игру без открытых файлов за 120 с.

# Факты об игре (не зависят от рантайма)
- XEX: title 454107F6, media 5E593EE0, база 0x82000000, entry 0x82C5C310, образ 20 054 016 Б, 267 именованных импортов
  (xboxkrnl 316 + xam 228 записей); `default.xex` 6 270 976 Б, LZX.
- Содержимое: 1196 `.xxx` (1179 UE3-пакетов, 17 аудио/потоковых с другой сигнатурой), 38 `.wmv` (37 ASF GUID,
  `MOHA_Shell_EAID_DEU.wmv` с иным заголовком). `Coalesced.ini`/`Xbox360TOC.txt` - не plain text.
- Порядок открытия на старте (6271, пути `D:\MOHAGame\...`): `Xbox360TOC.txt` (78 447 Б), `Config\Xenon\Cooked\Coalesced.ini`
  (308 494 Б), `Localization\Coalesced.int` (477 789 Б), `CookedMovies\MOHA_Shell_EAID.wmv` (8 797 581 Б), затем
  `CookedXenon\`: Core, Engine, EAKynapse, EngineFonts, EngineMaterials, EngineResources, GameFramework, IpDrv,
  MOHAEngineMaterials, MOHAGame, MOHAGame_LOC_int, MOHAUISkin, OnlineSubsystemLive, Plasma,
  PostProcessMaterialPackage, Shell, UI_Fonts, UI_MenuModules, UnrealScriptTest (.xxx).
- Первое видео - интро-WMV; ролик рисуется 3 плоскостями R8 (Y/U/V) своими шейдерами игры (PC: 253 draw/resolve
  вместо Switch 475 до watchdog). Первый ролик на Switch начинался на 43,5 с аптайма (6271).
- Видео: игра спрашивает режим 0x500 x 0x2D0 (1280x720); первый VdSwap (PC и Switch v0.5): front buffer 0xFF657000,
  format 6, tiled, pitch 1280, swizzle B,G,R,1; к нему 72 PM4-пакета, 24 стартовых draw без вывода.
- Кольцо команд: 32 КиБ (закодированный размер 12), phys 0x1FCF7000 (наблюдалось на PC; в Switch-логе v0.5/v0.6 тот же fetch/ring).
  Ожидание на phys 0x1FCF5004 - темп кадров: прерывание источника 1 ставит flip, источника 0 (vblank) отпускает его (PC).
- Инициализация видео: `VdInitializeEngines` (LR 0x829C0788) - первая граница v0.4; `VdSwap` LR 0x829B3B08.
- Шейдеры: XDK-контейнеры (magic 0x102A11) внутри `.xxx`; найдено 1036 уникальных (329 VS + 707 PS) из 1197 файлов,
  969 транслируются в SPIR-V (прошли spirv-val), 67 не транслируются (внутри WMV-шейдеры). Это PC-анализ.
- Лист 0x82C5B7D8 - функция времени в мс (GetTickCount-подобная): перехватывается как steady uptime ms, 32 бита,
  16 байт кода сверены (6271).
- Аудио инициализации: таблица XMA 320 контекстов x 64 Б; PC останавливается на `XMADisableContext` (LR 0x82997EF8),
  затем `NetDll_XNetStartup` (LR 0x82A98A54) - PC-результат, на Switch не проверено.
- Счётчики импортов за 120 с (f4ba): NtAllocateVirtualMemory 341, MmAllocatePhysicalMemoryEx 36, RtlMultiByteToUnicodeN 1251,
  KeQueryPerformanceFrequency 51.
