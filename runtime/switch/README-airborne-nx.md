AirborneNX — постоянная папка и имя приложения

Установка: /switch/airborne-nx/airborne-nx.nro
Журнал: /switch/airborne-nx/result.txt
Ресурсы: /switch/airborne-nx/game/MOHAGame

Все последующие обновления используют эти же имена. Номер версии и BUILD ID
записаны внутри приложения и журнала, а не в именах папок/исполняемых файлов.
Распакуйте switch из airborne-nx.zip в корень SD с заменой файлов сборки;
игровые ресурсы при обновлении остаются на месте. В небольшом ZIP ресурсов нет.
Готовая локальная папка dist/switch/airborne-nx содержит их полностью.

Переход со старой установки: перенесите game из папки 0.7 в airborne-nx на SD
или переименуйте старую папку в airborne-nx, а старый NRO — в airborne-nx.nro.
Затем распакуйте новую сборку поверх. Повторно копировать 6,4 ГБ с ПК не нужно.
Для совместимости приложение также находит ресурсы в старой папке 0.7,
если game в постоянной папке ещё нет.

Запускайте hbmenu в режиме приложения (R при запуске установленной игры).
После остановки/очистки нажмите +; при HOME_REQUIRED закройте через HOME.
Один запуск ограничен 120 секундами. Для разбора нужен один result.txt и
описание, появилось ли движущееся изображение. Старые тесты повторять не нужно.

Latest hardware result: user confirms the intro is visible on Switch (4 native movie draws).
The supplied log has no cleanup record; a successful exit is not claimed.
This build changes guest scheduling to libnx preemptive priority, accelerates buffer bounds,
and removes repeated diagnostic GPU readback from presentation after the first frame.
MOVIE_FRAME / GPU_STAGE_TIME record frame intervals and output-stage durations.
Speed on Switch is not measured yet. The 120-second watchdog remains.
Host entry still stops at XMADisableContext; complete intro, menu and gameplay are incomplete.

Контрольные суммы сборки — SHA256SUMS.txt. GAME_SHA256SUMS.txt в готовой папке
относится к ресурсам вашей копии. Результаты старых измерений сохраняются
отдельно; новые измеренные сборки архивируются перед заменой текущей.
