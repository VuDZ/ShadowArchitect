# F11. Выяснить, что удерживает память после выгрузки 100 планов в Ghost

**Текущий маршрут F11** — [проверка повторного использования памяти в Shipping](11-check-memory-reuse-in-shipping.md). Ниже сохранён первоначальный протокол для Development. Его критерии применимы при наличии игрового клиента с трассировкой.

**Цель.** С помощью Unreal Memory Insights определить, какие выделения переживают выгрузку мира с неподвижными проекциями и какие из них объясняют дополнительный расход RAM при включении `-ShadowPerPlanISM`.

В F10 после возврата в пустой слот частный рабочий набор относительно первого посещения того же слота вырос на 558–563 МиБ при обычной отрисовке и на 856–884 МиБ с ISM. Это результаты прогонов без переключений. В обоих случаях счётчики проекций вернулись к нулю. Свободный кэш страниц Binned2 объяснял менее 5 МиБ прироста в диагностическом повторе. [Методика и результаты F10](10-trace-memory-growth-across-view-cycles.md)

## Сценарий

1. Подготовьте один Development-клиент с символами для игры, мода и SaveCheck. Запишите трассу памяти с начала процесса (`-trace=default,memory`). На коротком пробном запуске откройте трассу в Memory Insights и убедитесь, что видны события выделения и освобождения, а стеки раскрываются до имён функций. Запишите версии и хеши использованных бинарников.
2. Возьмите тестовые копии слотов и маршрут `-SaveCheckMemoryRoute=idle` из F10. Пройдите отдельным процессом каждый вариант в порядке «обычная отрисовка, ISM», затем повторите в обратном порядке. Внутри пары меняется только флаг ISM. Сохраните камеру, графические настройки, расписание окон и длительность ожидания F10 (90 интервалов по 1,8 с в Ghost). Пользовательская игра должна быть закрыта. [Сборщик F10](../../../scripts/run_projection_memory_trace.py) · [Подготовка слотов и клиента](../../background-testing.md)
3. Поставьте в трассе метки для первого пустого слота `SAQ_F02_000_PILOT`, начала и конца ожидания на сетке `SAQ_F02_100_PILOT`, меню после выгрузки до и после диагностического GC с барьером рендера и повторного открытия исходного пустого слота. Сопоставьте их по времени с окнами SaveCheck и замерами процесса.

## Независимая проверка

- [ ] Есть четыре завершённых прогона. В каждом на сетке подтверждены 100 планов, 3 800 визуальных деталей и ноль смен вида. При обычной отрисовке зарегистрированы 3 800 mesh-компонентов, с ISM — 600, включая 500 групп. После GC и в конечном пустом слоте число компонентов проекций прежнего мира, включая ожидающие удаления, равно нулю либо расхождение разобрано по журналу.
- [ ] По каждому прогону сведены в МиБ частный рабочий набор, private bytes, живые байты по трассе и счётчики Binned2 `TotalAllocated` и `TotalOSAllocated` в контрольных точках. Покажите рост за время ожидания в Ghost и остаток между двумя посещениями пустого слота. Для каждой пары приведите разность этих приростов «ISM минус обычная отрисовка» и сопоставьте знак и разброс с F10. Если счётчик недоступен, укажите это явно.
- [ ] В Memory Insights сравните два посещения пустого слота запросом `Growth Vs Decline`. Отдельно отберите выделения, созданные при загрузке сетки или во время ожидания в Ghost и всё ещё живые в конечном пустом слоте. Это отделяет их от выделений повторной загрузки пустого мира. Для крупнейших групп приведите стек, подсистему или доступный тег LLM, число выделений и объём в обоих режимах. Покажите, воспроизводится ли вклад группы в обеих парах.
- [ ] В выводе указано, какую часть остатка удалось связать с конкретными стеками и какие данные указывают на удержание страниц аллокатором. Учитывайте разный охват счётчиков процесса, Binned2 и трассы. Для необъяснённой части укажите величину по соответствующему счётчику, ограничение измерения и следующий различающий замер. Если Development не воспроизводит разницу из Shipping, зафиксируйте этот результат и границу применимости вывода.
- [ ] Для повторной проверки приложены команды запуска, ревизия и версии, хеши входных слотов до и после, строки SaveCheck, таблица с метками времени и экспорт запросов с параметрами отбора и группировки. Укажите локальное расположение и SHA-256 исходных `.utrace`. В Git включите компактные результаты, исходные трассы и символы храните локально.

## Проверка доступного стенда

24 ноября 2026 года. Базовая ревизия `c5d2617` с закладками SaveCheck для F11. Проверены игровой Satisfactory Steam `rel-main-1.2.6-CL-520140`, SML 3.12.1, Shadow Architect 0.3.1 из ZIP F08 и SaveCheck 1.0.0. Входные слоты `SAQ_F02_000_PILOT` и `SAQ_F02_100_PILOT` сохранили хеши после маршрута. Автосохранение было отключено. Выходной слот не создавался. [Версии, команда игры и SHA-256](../../evidence/F11-shipping-pilot.json)

Unreal Build Tool отклонил игровой таргет `FactoryGameSteam Win64 Development`. Установленный SDK предоставляет игровой Shipping и Development Editor. Реализации загрузки сохранений в исходниках моддингового SDK представлены заглушками. SaveCheck успешно собран в Shipping и в `FactoryEditor Win64 Development`. В импортах модуля редактора подтвердились `FMiscTrace::OutputBookmarkSpec` и `FMiscTrace::OutputBookmarkInternalCycles`. [Конфигурации сборок SML](https://github.com/satisfactorymodding/Documentation/blob/master/modules/ROOT/pages/Development/Cpp/setup.adoc)

```text
Targets cannot be built in the Development configuration with this engine distribution.
```

Короткий пилот проверил, сможет ли F08 Shipping записать трассу по тем же параметрам Unreal. Игра приняла `-trace=default,memory,bookmark` и абсолютный путь `-tracefile`. Маршрут завершился при неподвижном Ghost, подтвердил 100 планов и 3 800 деталей и вернулся в пустой слот. Получено 257 отсчётов RAM. Все проекции прежнего мира исчезли к окну меню перед диагностическим GC. Файл `.utrace` после выхода процесса отсутствовал. `counters=0` означает, что в Shipping недоступны `TotalAllocated` и `TotalOSAllocated`. [Шесть контрольных точек пилота](../../evidence/F11-shipping-pilot.csv)

```text
LogInit: Build Configuration: Shipping
snapshot step=2 phase=grid mode=Ghost plans=100 actors=100 meshes=3800 ism_groups=0 ism_instances=0 visual_instances=3800
allocator step=10 name=Binned2 counters=0 allocated=0 os_allocated=0 os_cache=6963200 immediate_cache=6963200
snapshot step=11 phase=empty_after mode=Ghost plans=0 actors=0 meshes=0 ism_groups=0 ism_instances=0 visual_instances=0
pass=1 step=11 reason=complete memory route
```

Команды проверки стенда из корня репозитория. Для пилота положите собранный модуль Shipping и дескриптор в `Saved/F11-memory/SaveCheck`. Локальный лог пилота находится в `Saved/F11-memory/shipping-insights-pilot.log`.

```text
dotnet "<Engine>/Binaries/DOTNET/UnrealBuildTool/UnrealBuildTool.dll" FactoryGameSteam Win64 Development -Project="<SML>/FactoryGame.uproject" -Module=SaveCheck
python -B scripts/run_projection_memory_trace.py --output Saved/F11-memory --insights-pilot
python -B scripts/projection_memory_trace_report.py Saved/F11-memory --check shipping-insights-pilot
```

## Продолжение с игровым Development-клиентом

Для четырёх прогонов нужен готовый игровой `FactoryGameSteam Win64 Development` с поддержкой тестовых сохранений и соответствующими бинарниками Shadow Architect и SaveCheck. Запустите его со флагами маршрута F10 для `idle` и 30 циклов, а также с параметрами ниже. Используйте отдельный профиль, файл журнала и путь трассы для каждого запуска. Собирайте системную RAM существующим `sample_projection_memory.py --ram-only --timeout 660`, передав путь фактически загруженной DLL мода.

```text
-SaveCheckProjectionMemory -SaveCheckMemoryRoute=idle -SaveCheckMemoryCycles=30
-SaveCheckMemorySettle=0.9 -SaveCheckMemoryWindow=0.9 -SaveCheckMemoryBookmarks
-trace=default,memory,bookmark -tracefile=<путь-к-utrace> -notraceserver
```

Закладки используют тот же QPC, что и журнал SaveCheck. Для `Growth Vs Decline` возьмите `window_begin empty_before` и `window_begin empty_after`. Для выделений, переживших выгрузку сетки, в запросе `Memory Leaks` задайте A=`travel grid`, B=`window_end grid` последнего Ghost и C=`window_begin empty_after`. Группируйте результаты по стеку выделения и тегу. Сначала подтвердите события alloc/free и разрешение имён функций на коротком Development-пилоте. [Запись и запросы Memory Insights](https://dev.epicgames.com/documentation/unreal-engine/memory-insights-in-unreal-engine)

F11 остаётся открытой. Четыре Development-прогона, экспорты запросов и атрибуция остатка зависят от наличия совместимого игрового клиента.
